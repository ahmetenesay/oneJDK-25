/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact WLFT or visit https://github.com/ahmetenesay/oneJDK-25
 * if you need additional information or have any questions.
 */

#include "gc/redjet/allocator/redjetStreamThrottle.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <intrin.h>
#define RJ_STREAM_PAUSE() _mm_pause()
#elif defined(_M_ARM64)
#include <intrin.h>
#define RJ_STREAM_PAUSE() __yield()
#else
#define RJ_STREAM_PAUSE() ((void)0)
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define RJ_STREAM_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define RJ_STREAM_PAUSE() asm volatile("yield" ::: "memory")
#elif defined(__riscv)
#define RJ_STREAM_PAUSE() asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory")
#else
#define RJ_STREAM_PAUSE() ((void)0)
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetStreamThrottle* RedjetStreamThrottle::_instance = nullptr;
std::atomic<bool>     RedjetStreamThrottle::_initialized{ false };

RedjetStreamThrottle::RedjetStreamThrottle()
  : _active_streams_count(0),
    _throttle_engaged(false),
    _global_pacing_factor(1),
    _total_streams_opened(0),
    _total_streams_closed(0),
    _total_pauses_injected(0),
    _total_bytes_streamed(0),
    _total_throttle_cycles_damped(0) {
  hs_ticket_lock_init(&_throttle_lock);
  for (size_t i = 0; i < MAX_CONCURRENT_STREAMS; ++i) {
    _streams[i].stream_id = static_cast<uint32_t>(i);
    _streams[i].ccd_id = 0;
    _streams[i].priority = RedjetStreamPriority::STREAM_GC_BACKGROUND;
    _streams[i].is_active = false;
    _streams[i].bytes_streamed = 0;
    _streams[i].pauses_injected = 0;
    _streams[i].last_transfer_tsc = 0;
    _streams[i].bandwidth_token_bucket = 1024 * 1024;
  }
}

RedjetStreamThrottle::~RedjetStreamThrottle() {
}

void RedjetStreamThrottle::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetStreamThrottle();
  }
}

void RedjetStreamThrottle::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

uint32_t RedjetStreamThrottle::open_stream(uint32_t ccd_id, RedjetStreamPriority priority) {
  hs_ticket_guard_t guard(_throttle_lock);

  for (size_t i = 0; i < MAX_CONCURRENT_STREAMS; ++i) {
    if (!_streams[i].is_active) {
      _streams[i].is_active = true;
      _streams[i].ccd_id = ccd_id;
      _streams[i].priority = priority;
      _streams[i].bytes_streamed = 0;
      _streams[i].pauses_injected = 0;
      _streams[i].last_transfer_tsc = sacculus_rdtsc();
      _streams[i].bandwidth_token_bucket = 1024 * 1024;

      _active_streams_count.fetch_add(1, std::memory_order_relaxed);
      _total_streams_opened.fetch_add(1, std::memory_order_relaxed);
      return static_cast<uint32_t>(i);
    }
  }

  return UINT32_MAX; // Capacity reached
}

void RedjetStreamThrottle::close_stream(uint32_t stream_id) {
  if (stream_id >= MAX_CONCURRENT_STREAMS) return;

  hs_ticket_guard_t guard(_throttle_lock);
  if (_streams[stream_id].is_active) {
    _streams[stream_id].is_active = false;
    if (_active_streams_count > 0) {
      _active_streams_count.fetch_sub(1, std::memory_order_relaxed);
    }
    _total_streams_closed.fetch_add(1, std::memory_order_relaxed);
  }
}

void RedjetStreamThrottle::pace_stream(uint32_t stream_id, size_t bytes_transferred) {
  if (stream_id >= MAX_CONCURRENT_STREAMS) return;

  RedjetStreamDescriptor& desc = _streams[stream_id];
  if (!desc.is_active) return;

  // Mutator Critical streams are never throttled
  if (desc.priority == RedjetStreamPriority::STREAM_MUTATOR_CRITICAL) {
    desc.bytes_streamed += bytes_transferred;
    _total_bytes_streamed.fetch_add(bytes_transferred, std::memory_order_relaxed);
    return;
  }

  desc.bytes_streamed += bytes_transferred;
  _total_bytes_streamed.fetch_add(bytes_transferred, std::memory_order_relaxed);

  // Evaluate throttling condition
  bool throttle = _throttle_engaged.load(std::memory_order_relaxed);
  uint32_t factor = _global_pacing_factor.load(std::memory_order_relaxed);

  // Background GC threads are throttled aggressively when bus is congested
  if (throttle || desc.priority == RedjetStreamPriority::STREAM_GC_BACKGROUND) {
    uint32_t pause_count = 1;
    if (desc.priority == RedjetStreamPriority::STREAM_GC_BACKGROUND) {
      pause_count = throttle ? (8 * factor) : 2;
    } else if (desc.priority == RedjetStreamPriority::STREAM_GC_FOREGROUND) {
      pause_count = throttle ? (2 * factor) : 0;
    }

    if (pause_count > 0) {
      uint64_t start_tsc = sacculus_rdtsc();
      for (uint32_t p = 0; p < pause_count; ++p) {
        RJ_STREAM_PAUSE();
      }
      uint64_t elapsed = sacculus_rdtsc() - start_tsc;
      desc.pauses_injected += pause_count;
      _total_pauses_injected.fetch_add(pause_count, std::memory_order_relaxed);
      _total_throttle_cycles_damped.fetch_add(elapsed, std::memory_order_relaxed);
    }
  }
}

void RedjetStreamThrottle::set_global_congestion_throttle(bool engage, uint32_t pacing_multiplier) noexcept {
  _throttle_engaged.store(engage, std::memory_order_release);
  _global_pacing_factor.store(std::max<uint32_t>(1, pacing_multiplier), std::memory_order_release);
}

void RedjetStreamThrottle::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet Hardware Memory Stream Throttle ===");
  st->print_cr("  Congestion Throttle Engaged: %s (Pacing Factor: %u)",
               _throttle_engaged.load(std::memory_order_relaxed) ? "YES" : "NO",
               _global_pacing_factor.load(std::memory_order_relaxed));
  st->print_cr("  Active Stream Channels: %u / " SIZE_FORMAT,
               _active_streams_count.load(std::memory_order_relaxed), MAX_CONCURRENT_STREAMS);
  st->print_cr("  Total Streams Opened: " UINT64_FORMAT, streams_opened());
  st->print_cr("  Total Streams Closed: " UINT64_FORMAT, streams_closed());
  st->print_cr("  Total Pauses Injected: " UINT64_FORMAT, pauses_injected());
  st->print_cr("  Total Bytes Streamed: " UINT64_FORMAT " (" SIZE_FORMAT " MB)",
               bytes_streamed(), static_cast<size_t>(bytes_streamed() / (1024 * 1024)));
  st->print_cr("  Total Cycles Damped: " UINT64_FORMAT, cycles_damped());
}
