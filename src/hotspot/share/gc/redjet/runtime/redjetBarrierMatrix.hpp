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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETBARRIERMATRIX_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETBARRIERMATRIX_HPP

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
// Redjet Dynamic Memory Barrier Matrix (PLAN.md Section 3.1, 3.2)
// - Evaluates memory barrier execution across C1, C2, MIRC, and VAC compilers
// - Manages Dynamic Barrier Elision (Egg young-object elision & immutable read elision)
// - Validates TailTraces clasp emission and LRB (Load-Reference Barrier) correctness
// ============================================================================

enum class RedjetBarrierKind : uint8_t {
  BARRIER_PRE_WRITE  = 0, // SATB / reference capture
  BARRIER_POST_WRITE = 1, // TailTraces clasp emission (MOV [r15 + clasp_off], id)
  BARRIER_LOAD_REF   = 2, // Load-Reference Barrier (pointer healing)
  BARRIER_NATIVE_PIN = 3  // Panama / JNI pinning barrier
};

enum class RedjetCompilerTier : uint8_t {
  TIER_C1   = 0,
  TIER_C2   = 1,
  TIER_MIRC = 2,
  TIER_VAC  = 3
};

struct RedjetBarrierMetrics {
  uint64_t total_invocations{ 0 };
  uint64_t total_elisions{ 0 };
  double   elision_rate{ 0.0 };
};

class RedjetBarrierMatrix {
private:
  static RedjetBarrierMatrix* _instance;
  static std::atomic<bool>    _initialized;

  std::atomic<uint64_t>       _invocations[4][4]; // [CompilerTier][BarrierKind]
  std::atomic<uint64_t>       _elisions[4][4];    // [CompilerTier][BarrierKind]
  hs_ticket_lock_t            _matrix_lock;

  // Global telemetry
  std::atomic<uint64_t>       _total_barriers_evaluated{ 0 };
  std::atomic<uint64_t>       _total_egg_elisions{ 0 };
  std::atomic<uint64_t>       _total_read_elisions{ 0 };

public:
  RedjetBarrierMatrix();
  ~RedjetBarrierMatrix();

  static void initialize();
  static void destroy();
  static RedjetBarrierMatrix* get() noexcept { return _instance; }

  // 1. Barrier Elision Evaluation
  // Returns true if write barrier on destination object in specified tier can be safely skipped
  RJ_API bool should_elide_post_write_barrier(uint8_t origin_vfs_tier, bool is_newly_allocated) noexcept;
  RJ_API bool should_elide_load_reference_barrier(bool is_locally_confined, bool is_immutable) noexcept;

  // 2. Barrier Accounting & Telemetry
  RJ_API void record_barrier(RedjetCompilerTier compiler, RedjetBarrierKind barrier, bool was_elided) noexcept;

  // 3. Query Barrier Metrics
  RJ_API RedjetBarrierMetrics get_metrics(RedjetCompilerTier compiler, RedjetBarrierKind barrier) const noexcept;

  // Telemetry
  uint64_t barriers_evaluated() const noexcept { return _total_barriers_evaluated.load(std::memory_order_relaxed); }
  uint64_t egg_elisions()       const noexcept { return _total_egg_elisions.load(std::memory_order_relaxed); }
  uint64_t read_elisions()      const noexcept { return _total_read_elisions.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETBARRIERMATRIX_HPP
