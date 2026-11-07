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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETPREFETCHORCHESTRATOR_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETPREFETCHORCHESTRATOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// Redjet Prefetch Orchestrator (PLAN.md Section 4.1, 5.4)
// - Dynamic Prefetch Distance Auto-Tuning (K = Latency / ALU Loop Time)
// - Hardware Stride Detector (Forward, Reverse, Interleaved, Non-Temporal)
// - Online Stride Learning with 16-access sliding window history
// - Emits multi-level prefetch hints (_MM_HINT_T0, T1, T2, NTA)
// - Zero L1-D cache miss runway object iteration
// ============================================================================

enum class RedjetPrefetchHint : uint8_t {
  HINT_T0  = 0, // Prefetch to all cache levels (L1, L2, L3)
  HINT_T1  = 1, // Prefetch to L2 and L3
  HINT_T2  = 2, // Prefetch to L3 only
  HINT_NTA = 3  // Non-Temporal: minimize cache pollution (streaming bypass)
};

enum class RedjetAccessStride : uint8_t {
  STRIDE_SEQUENTIAL_64B   = 0,
  STRIDE_INTERLEAVED_128B = 1,
  STRIDE_REVERSE_64B      = 2,
  STRIDE_STRIDED_K        = 3,
  STRIDE_RANDOM           = 4
};

struct alignas(64) RedjetStrideHistory {
  static constexpr size_t HISTORY_CAPACITY = 16;

  uintptr_t addresses[HISTORY_CAPACITY]{ 0 };
  int64_t   deltas[HISTORY_CAPACITY]{ 0 };
  size_t    head{ 0 };
  size_t    count{ 0 };

  void reset() noexcept {
    for (size_t i = 0; i < HISTORY_CAPACITY; ++i) {
      addresses[i] = 0;
      deltas[i] = 0;
    }
    head = 0;
    count = 0;
  }

  void record(uintptr_t addr) noexcept {
    if (count > 0) {
      size_t prev_idx = (head + HISTORY_CAPACITY - 1) % HISTORY_CAPACITY;
      deltas[head] = static_cast<int64_t>(addr) - static_cast<int64_t>(addresses[prev_idx]);
    } else {
      deltas[head] = 0;
    }
    addresses[head] = addr;
    head = (head + 1) % HISTORY_CAPACITY;
    if (count < HISTORY_CAPACITY) count++;
  }

  RedjetAccessStride classify() const noexcept;
};

class RedjetPrefetchOrchestrator {
public:
  static constexpr size_t DEFAULT_PREFETCH_AHEAD_LINES = 8; // 8 cache lines ahead (512 bytes)

private:
  static RedjetPrefetchOrchestrator* _instance;
  static std::atomic<bool>           _initialized;

  std::atomic<size_t>                _prefetch_distance_lines{ DEFAULT_PREFETCH_AHEAD_LINES };
  hs_ticket_lock_t                   _orchestrator_lock;
  RedjetStrideHistory                _stride_history;

  // Telemetry & metrics
  std::atomic<uint64_t>              _total_prefetches_issued{ 0 };
  std::atomic<uint64_t>              _total_nta_streams_issued{ 0 };
  std::atomic<uint64_t>              _total_lines_warmed{ 0 };
  std::atomic<uint64_t>              _stride_classifications{ 0 };
  std::atomic<uint64_t>              _dynamic_recalibrations{ 0 };

public:
  RedjetPrefetchOrchestrator();
  ~RedjetPrefetchOrchestrator();

  static void initialize(size_t ahead_lines = DEFAULT_PREFETCH_AHEAD_LINES);
  static void destroy();
  static RedjetPrefetchOrchestrator* get() noexcept { return _instance; }

  // Dynamic Auto-Tuner: K = ceil(Latency / LoopTime)
  void calibrate_distance(uint32_t memory_latency_ns, uint32_t loop_cycle_ns);

  // Single-line prefetch hint
  RJ_API void prefetch_line(const void* addr, RedjetPrefetchHint hint = RedjetPrefetchHint::HINT_T0) noexcept;

  // Unrolled 4-line prefetch ahead
  RJ_API void prefetch_4lines_ahead(const void* addr, RedjetPrefetchHint hint = RedjetPrefetchHint::HINT_T0) noexcept;

  // Multi-line range prefetching with stride detection
  RJ_API void prefetch_range(const void* base, size_t bytes,
                             RedjetPrefetchHint hint = RedjetPrefetchHint::HINT_T0,
                             RedjetAccessStride stride = RedjetAccessStride::STRIDE_SEQUENTIAL_64B) noexcept;

  // Record access for online stride classification
  RJ_API void record_access_and_classify(const void* addr) noexcept;

  // Distance accessor
  size_t prefetch_distance_lines() const noexcept {
    return _prefetch_distance_lines.load(std::memory_order_relaxed);
  }

  // Telemetry accessors
  uint64_t total_prefetches_issued()   const noexcept { return _total_prefetches_issued.load(std::memory_order_relaxed); }
  uint64_t total_nta_streams_issued()  const noexcept { return _total_nta_streams_issued.load(std::memory_order_relaxed); }
  uint64_t total_lines_warmed()        const noexcept { return _total_lines_warmed.load(std::memory_order_relaxed); }
  uint64_t stride_classifications()    const noexcept { return _stride_classifications.load(std::memory_order_relaxed); }
  uint64_t dynamic_recalibrations()    const noexcept { return _dynamic_recalibrations.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETPREFETCHORCHESTRATOR_HPP
