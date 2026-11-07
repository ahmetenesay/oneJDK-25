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

#include "gc/redjet/allocator/redjetPrefetchOrchestrator.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#include <mmintrin.h>
#else
#include <xmmintrin.h>
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetPrefetchOrchestrator* RedjetPrefetchOrchestrator::_instance = nullptr;
std::atomic<bool> RedjetPrefetchOrchestrator::_initialized{ false };

// ============================================================================
// RedjetStrideHistory Implementation
// ============================================================================

RedjetAccessStride RedjetStrideHistory::classify() const noexcept {
  if (count < 4) {
    return RedjetAccessStride::STRIDE_SEQUENTIAL_64B;
  }

  size_t seq_64_forward = 0;
  size_t seq_64_reverse = 0;
  size_t int_128 = 0;
  size_t regular_stride = 0;

  int64_t expected_delta = deltas[1];

  for (size_t i = 1; i < count; ++i) {
    int64_t d = deltas[i];
    if (d == 64)  seq_64_forward++;
    if (d == -64) seq_64_reverse++;
    if (d == 128) int_128++;
    if (d == expected_delta && d != 0) regular_stride++;
  }

  size_t valid_transitions = count - 1;
  if (seq_64_forward >= (valid_transitions * 3) / 4) {
    return RedjetAccessStride::STRIDE_SEQUENTIAL_64B;
  }
  if (seq_64_reverse >= (valid_transitions * 3) / 4) {
    return RedjetAccessStride::STRIDE_REVERSE_64B;
  }
  if (int_128 >= (valid_transitions * 3) / 4) {
    return RedjetAccessStride::STRIDE_INTERLEAVED_128B;
  }
  if (regular_stride >= (valid_transitions * 3) / 4) {
    return RedjetAccessStride::STRIDE_STRIDED_K;
  }

  return RedjetAccessStride::STRIDE_RANDOM;
}

// ============================================================================
// RedjetPrefetchOrchestrator Implementation
// ============================================================================

RedjetPrefetchOrchestrator::RedjetPrefetchOrchestrator()
  : _prefetch_distance_lines(DEFAULT_PREFETCH_AHEAD_LINES),
    _total_prefetches_issued(0),
    _total_nta_streams_issued(0),
    _total_lines_warmed(0),
    _stride_classifications(0),
    _dynamic_recalibrations(0) {
  hs_ticket_lock_init(&_orchestrator_lock);
  _stride_history.reset();
}

RedjetPrefetchOrchestrator::~RedjetPrefetchOrchestrator() {
}

void RedjetPrefetchOrchestrator::initialize(size_t ahead_lines) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetPrefetchOrchestrator();
    _instance->_prefetch_distance_lines.store((ahead_lines > 0) ? ahead_lines : DEFAULT_PREFETCH_AHEAD_LINES,
                                              std::memory_order_relaxed);
  }
}

void RedjetPrefetchOrchestrator::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetPrefetchOrchestrator::calibrate_distance(uint32_t memory_latency_ns, uint32_t loop_cycle_ns) {
  hs_ticket_lock_acquire(&_orchestrator_lock);

  uint32_t loop_ns = (loop_cycle_ns > 0) ? loop_cycle_ns : 10;
  size_t calculated = (memory_latency_ns + loop_ns - 1) / loop_ns;

  // Clamp distance between 2 and 32 cache lines (128 bytes to 2048 bytes)
  calculated = std::max<size_t>(2, std::min<size_t>(calculated, 32));

  _prefetch_distance_lines.store(calculated, std::memory_order_relaxed);
  _dynamic_recalibrations.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_orchestrator_lock);
}

void RedjetPrefetchOrchestrator::prefetch_line(const void* addr, RedjetPrefetchHint hint) noexcept {
  if (addr == nullptr) return;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  const char* ptr = reinterpret_cast<const char*>(addr);
  switch (hint) {
    case RedjetPrefetchHint::HINT_T0:
      _mm_prefetch(ptr, _MM_HINT_T0);
      break;
    case RedjetPrefetchHint::HINT_T1:
      _mm_prefetch(ptr, _MM_HINT_T1);
      break;
    case RedjetPrefetchHint::HINT_T2:
      _mm_prefetch(ptr, _MM_HINT_T2);
      break;
    case RedjetPrefetchHint::HINT_NTA:
      _mm_prefetch(ptr, _MM_HINT_NTA);
      break;
  }
#elif defined(__GNUC__) || defined(__clang__)
  int h = (hint == RedjetPrefetchHint::HINT_NTA) ? 0 : 3;
  __builtin_prefetch(addr, 0, h);
#endif

  _total_prefetches_issued.fetch_add(1, std::memory_order_relaxed);
  _total_lines_warmed.fetch_add(1, std::memory_order_relaxed);
  if (hint == RedjetPrefetchHint::HINT_NTA) {
    _total_nta_streams_issued.fetch_add(1, std::memory_order_relaxed);
  }
}

void RedjetPrefetchOrchestrator::prefetch_4lines_ahead(const void* addr, RedjetPrefetchHint hint) noexcept {
  if (addr == nullptr) return;

  const char* ptr = reinterpret_cast<const char*>(addr);
  size_t dist = _prefetch_distance_lines.load(std::memory_order_relaxed) * 64;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  int h = _MM_HINT_T0;
  if (hint == RedjetPrefetchHint::HINT_T1) h = _MM_HINT_T1;
  else if (hint == RedjetPrefetchHint::HINT_T2) h = _MM_HINT_T2;
  else if (hint == RedjetPrefetchHint::HINT_NTA) h = _MM_HINT_NTA;

  _mm_prefetch(ptr + dist, h);
  _mm_prefetch(ptr + dist + 64, h);
  _mm_prefetch(ptr + dist + 128, h);
  _mm_prefetch(ptr + dist + 192, h);
#elif defined(__GNUC__) || defined(__clang__)
  int h = (hint == RedjetPrefetchHint::HINT_NTA) ? 0 : 3;
  __builtin_prefetch(ptr + dist, 0, h);
  __builtin_prefetch(ptr + dist + 64, 0, h);
  __builtin_prefetch(ptr + dist + 128, 0, h);
  __builtin_prefetch(ptr + dist + 192, 0, h);
#endif

  _total_prefetches_issued.fetch_add(4, std::memory_order_relaxed);
  _total_lines_warmed.fetch_add(4, std::memory_order_relaxed);
}

void RedjetPrefetchOrchestrator::prefetch_range(const void* base, size_t bytes,
                                               RedjetPrefetchHint hint,
                                               RedjetAccessStride stride) noexcept {
  if (base == nullptr || bytes == 0) return;

  const uint8_t* p = reinterpret_cast<const uint8_t*>(base);
  size_t step = (stride == RedjetAccessStride::STRIDE_INTERLEAVED_128B) ? 128 : 64;
  size_t count = (bytes + step - 1) / step;

  if (stride == RedjetAccessStride::STRIDE_REVERSE_64B) {
    p += bytes;
    for (size_t i = 0; i < count; ++i) {
      p -= step;
      prefetch_line(p, hint);
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      prefetch_line(p + (i * step), hint);
    }
  }
}

void RedjetPrefetchOrchestrator::record_access_and_classify(const void* addr) noexcept {
  if (addr == nullptr) return;

  _stride_history.record(reinterpret_cast<uintptr_t>(addr));
  _stride_classifications.fetch_add(1, std::memory_order_relaxed);
}

void RedjetPrefetchOrchestrator::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Prefetch Orchestrator ---");
  st->print_cr("  Prefetch Distance:       %zu cache lines (%zu bytes ahead)",
               _prefetch_distance_lines.load(std::memory_order_relaxed),
               _prefetch_distance_lines.load(std::memory_order_relaxed) * 64);
  st->print_cr("  Total Prefetches:        %llu", static_cast<unsigned long long>(_total_prefetches_issued.load(std::memory_order_relaxed)));
  st->print_cr("  NTA Stream Bypass:       %llu", static_cast<unsigned long long>(_total_nta_streams_issued.load(std::memory_order_relaxed)));
  st->print_cr("  Cache Lines Warmed:      %llu", static_cast<unsigned long long>(_total_lines_warmed.load(std::memory_order_relaxed)));
  st->print_cr("  Stride Classifications:  %llu", static_cast<unsigned long long>(_stride_classifications.load(std::memory_order_relaxed)));
  st->print_cr("  Dynamic Recalibrations:  %llu", static_cast<unsigned long long>(_dynamic_recalibrations.load(std::memory_order_relaxed)));
}
