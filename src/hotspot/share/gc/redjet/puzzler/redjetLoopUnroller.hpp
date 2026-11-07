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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETLOOPUNROLLER_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETLOOPUNROLLER_HPP

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
// Redjet Puzzler Loop Unroller & Vector Epilogue Weaver (PLAN.md Section 3.1, 5.1)
// - Step 4 (Reassembly & Weaving) optimization pass for hot loop nests
// - Computes optimal loop unrolling factors (x2, x4, x8, x16) targeting AVX-512 / AVX2
// - Eliminates redundant induction variable increments and back-edge branch overhead
// - Generates clean peeling heads, vector main loops, and scalar remainder epilogues
// ============================================================================

enum class RedjetUnrollFactor : uint8_t {
  UNROLL_NONE = 1,
  UNROLL_2X   = 2,
  UNROLL_4X   = 4,
  UNROLL_8X   = 8,
  UNROLL_16X  = 16
};

struct RedjetLoopMetadata {
  uint32_t           loop_id{ 0 };
  uint32_t           method_id{ 0 };
  size_t             body_instruction_count{ 0 };
  int64_t            constant_trip_count{ -1 }; // -1 if dynamic / unknown
  bool               has_memory_dependencies{ false };
  bool               has_fp_math{ false };
  RedjetUnrollFactor recommended_factor{ RedjetUnrollFactor::UNROLL_NONE };
};

struct RedjetUnrollResult {
  bool               success{ false };
  RedjetUnrollFactor applied_factor{ RedjetUnrollFactor::UNROLL_NONE };
  size_t             original_instructions{ 0 };
  size_t             unrolled_instructions{ 0 };
  size_t             estimated_branch_savings_percent{ 0 };
  uint64_t           processing_duration_tsc{ 0 };
};

class RedjetLoopUnroller {
public:
  static constexpr size_t MAX_LOOP_BODY_INSTRUCTIONS = 256;
  static constexpr size_t MAX_UNROLLED_CAPACITY      = 2048;

private:
  static RedjetLoopUnroller* _instance;
  static std::atomic<bool>   _initialized;

  hs_ticket_lock_t           _unroller_lock;

  // Telemetry
  std::atomic<uint64_t>      _total_loops_analyzed{ 0 };
  std::atomic<uint64_t>      _total_loops_unrolled{ 0 };
  std::atomic<uint64_t>      _total_branches_eliminated{ 0 };

  RedjetUnrollFactor determine_optimal_factor(const RedjetLoopMetadata& meta) const noexcept;

public:
  RedjetLoopUnroller();
  ~RedjetLoopUnroller();

  static void initialize();
  static void destroy();
  static RedjetLoopUnroller* get() noexcept { return _instance; }

  // 1. Loop Analysis & Unroll Recommendation
  RJ_API RedjetUnrollFactor evaluate_loop(const RedjetLoopMetadata& meta) noexcept;

  // 2. Perform Unrolling Transformation
  RJ_API RedjetUnrollResult transform_loop(const RedjetLoopMetadata& meta,
                                           const uint8_t* original_code,
                                           size_t original_bytes,
                                           uint8_t* output_buffer,
                                           size_t output_buffer_capacity);

  // Telemetry queries
  uint64_t loops_analyzed()       const noexcept { return _total_loops_analyzed.load(std::memory_order_relaxed); }
  uint64_t loops_unrolled()       const noexcept { return _total_loops_unrolled.load(std::memory_order_relaxed); }
  uint64_t branches_eliminated()  const noexcept { return _total_branches_eliminated.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETLOOPUNROLLER_HPP
