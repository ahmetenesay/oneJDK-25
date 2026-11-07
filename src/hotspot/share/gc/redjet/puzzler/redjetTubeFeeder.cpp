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

#include "gc/redjet/puzzler/redjetTubeFeeder.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetTubeFeeder::RedjetTubeFeeder() {
  _global_stream_throttle.store(false, std::memory_order_relaxed);
  _total_elements_fed.store(0, std::memory_order_relaxed);
  _total_prefetches_issued.store(0, std::memory_order_relaxed);
  _total_throttle_pauses.store(0, std::memory_order_relaxed);
  _total_channels_opened.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < REDJET_MAX_TUBE_CHANNELS; ++i) {
    _channels[i].reset();
  }
}

RedjetTubeFeeder::~RedjetTubeFeeder() {
  destroy();
}

void RedjetTubeFeeder::init() {
  hs_ticket_guard_t guard(_feeder_lock);
  _global_stream_throttle.store(false, std::memory_order_relaxed);
  for (size_t i = 0; i < REDJET_MAX_TUBE_CHANNELS; ++i) {
    _channels[i].reset();
  }
}

void RedjetTubeFeeder::destroy() {
  hs_ticket_guard_t guard(_feeder_lock);
  for (size_t i = 0; i < REDJET_MAX_TUBE_CHANNELS; ++i) {
    _channels[i].reset();
  }
}

// =========================================================================
// 1. Channel Lifecycle
// =========================================================================
uint32_t RedjetTubeFeeder::open_channel(const void* base_ptr, size_t element_count, size_t element_stride) {
  if (base_ptr == nullptr || element_count == 0 || element_stride == 0) {
    return UINT32_MAX;
  }

  hs_ticket_guard_t guard(_feeder_lock);

  for (size_t i = 0; i < REDJET_MAX_TUBE_CHANNELS; ++i) {
    if (!_channels[i].is_active.load(std::memory_order_relaxed)) {
      RedjetTubeChannel& ch = _channels[i];
      ch.stream_base = static_cast<const uint8_t*>(base_ptr);
      ch.total_elements = element_count;
      ch.element_stride = element_stride;
      ch.consumer_index.store(0, std::memory_order_relaxed);
      ch.prefetch_index.store(0, std::memory_order_relaxed);
      ch.stream_paused.store(_global_stream_throttle.load(std::memory_order_relaxed), std::memory_order_relaxed);
      ch.channel_id = static_cast<uint32_t>(i);
      ch.is_active.store(true, std::memory_order_release);

      _total_channels_opened.fetch_add(1, std::memory_order_relaxed);
      return static_cast<uint32_t>(i);
    }
  }

  return UINT32_MAX; // All channels occupied
}

void RedjetTubeFeeder::close_channel(uint32_t channel_id) {
  if (channel_id >= REDJET_MAX_TUBE_CHANNELS) return;
  hs_ticket_guard_t guard(_feeder_lock);
  _channels[channel_id].reset();
}

// =========================================================================
// 2. Hardware Prefetch Stepping (Ahead by 64 Elements)
// Issues _mm_prefetch(..., _MM_HINT_T0) ahead into L1-D cache
// =========================================================================
void RedjetTubeFeeder::advance_prefetch(uint32_t channel_id, size_t consumer_pos) {
  if (channel_id >= REDJET_MAX_TUBE_CHANNELS) return;
  RedjetTubeChannel& ch = _channels[channel_id];
  if (!ch.is_active.load(std::memory_order_relaxed)) return;

  // MLTC bus throttling check
  if (ch.stream_paused.load(std::memory_order_relaxed) ||
      _global_stream_throttle.load(std::memory_order_relaxed)) {
    _total_throttle_pauses.fetch_add(1, std::memory_order_relaxed);
    return; // Bus saturated: yield memory bus to mutator threads
  }

  size_t target_prefetch = consumer_pos + REDJET_TUBE_PREFETCH_DISTANCE;
  if (target_prefetch > ch.total_elements) {
    target_prefetch = ch.total_elements;
  }

  size_t cur_prefetch = ch.prefetch_index.load(std::memory_order_relaxed);
  if (cur_prefetch < consumer_pos) {
    cur_prefetch = consumer_pos;
  }

  const uint8_t* base = ch.stream_base;
  size_t stride = ch.element_stride;
  size_t count = 0;

  while (cur_prefetch < target_prefetch) {
    const void* addr = base + (cur_prefetch * stride);
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0);
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, 3);
#endif
#endif
    cur_prefetch++;
    count++;
  }

  ch.prefetch_index.store(cur_prefetch, std::memory_order_relaxed);
  _total_prefetches_issued.fetch_add(count, std::memory_order_relaxed);
}

const void* RedjetTubeFeeder::consume_elements(uint32_t channel_id, size_t count) {
  if (channel_id >= REDJET_MAX_TUBE_CHANNELS) return nullptr;
  RedjetTubeChannel& ch = _channels[channel_id];
  if (!ch.is_active.load(std::memory_order_relaxed)) return nullptr;

  size_t cur = ch.consumer_index.fetch_add(count, std::memory_order_relaxed);
  if (cur >= ch.total_elements) {
    return nullptr;
  }

  // Advance prefetch pipeline ahead by 64 elements
  advance_prefetch(channel_id, cur + count);
  _total_elements_fed.fetch_add(count, std::memory_order_relaxed);

  return ch.stream_base + (cur * ch.element_stride);
}

// =========================================================================
// 3. MLTC Bus Throttling & Congestion Coupling
// Pauses prefetch stream when bus saturation is detected by MLTC
// =========================================================================
void RedjetTubeFeeder::update_mltc_congestion_state(RedjetBusCongestionLevel congestion_level) {
  bool should_throttle = (congestion_level >= RedjetBusCongestionLevel::HIGH_CONTENTION);
  _global_stream_throttle.store(should_throttle, std::memory_order_release);

  for (size_t i = 0; i < REDJET_MAX_TUBE_CHANNELS; ++i) {
    if (_channels[i].is_active.load(std::memory_order_relaxed)) {
      _channels[i].stream_paused.store(should_throttle, std::memory_order_relaxed);
    }
  }

  if (should_throttle) {
    _total_throttle_pauses.fetch_add(1, std::memory_order_relaxed);
  }
}

bool RedjetTubeFeeder::is_channel_paused(uint32_t channel_id) const noexcept {
  if (channel_id >= REDJET_MAX_TUBE_CHANNELS) return false;
  return _channels[channel_id].stream_paused.load(std::memory_order_relaxed) ||
         _global_stream_throttle.load(std::memory_order_relaxed);
}

void RedjetTubeFeeder::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet Hardware Tube Feeder (Vector Prefetch Conduit):");
  st->print_cr("  Channels Opened:            " UINT64_FORMAT, total_channels_opened());
  st->print_cr("  Total Elements Fed:         " UINT64_FORMAT, total_elements_fed());
  st->print_cr("  Total Prefetches Issued:    " UINT64_FORMAT, total_prefetches_issued());
  st->print_cr("  MLTC Throttle Pauses:       " UINT64_FORMAT, total_throttle_pauses());
  st->print_cr("  Global Throttle Active:     %s", _global_stream_throttle.load(std::memory_order_relaxed) ? "YES" : "NO");
}
