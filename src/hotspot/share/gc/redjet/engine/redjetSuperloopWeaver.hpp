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

#ifndef SHARE_GC_REDJET_ENGINE_REDJETSUPERLOOPWEAVER_HPP
#define SHARE_GC_REDJET_ENGINE_REDJETSUPERLOOPWEAVER_HPP

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
// Redjet Superloop Weaver Engine (PLAN.md Section 5.3, 6.2, 7.2)
// - Hardware Execution Port Saturation Analyzer (Intel Golden Cove & AMD Zen 4/5)
// - ILP Interleaved Unrolling (4x, 8x, 16x) with Vector Register Rotation
// - PILF (Puzzler Interleaved Loop Fusion) Back-Edge Generation
// - Peeling / Main Vector Body / Scalar Residual Epilogue Generation
// - Bit-Exact IEEE 754 Safety Gate & TSC-Delta Benchmark Verification
// ============================================================================

enum class RedjetLoopKind : uint8_t {
  CONTIGUOUS_ARRAY_COPY = 0,
  VECTOR_MATH_REDUCTION = 1,
  OBJECT_FIELD_SCAN     = 2,
  POINTER_CHASING       = 3,
  SPARSE_INDEXED_ACCESS = 4
};

struct RedjetLoopCharacteristics {
  RedjetLoopKind kind{ RedjetLoopKind::CONTIGUOUS_ARRAY_COPY };
  size_t         element_size_bytes{ 8 };
  size_t         trip_count{ 0 };
  bool           has_memory_aliasing{ false };
  bool           has_floating_point{ false };
  uint8_t        alu_ops_per_iteration{ 1 };
  uint8_t        load_ops_per_iteration{ 1 };
  uint8_t        store_ops_per_iteration{ 1 };
};

struct RedjetSuperloopPlan {
  uint8_t  optimal_unroll_factor{ 8 }; // 4x, 8x, or 16x
  uint8_t  vector_width_bytes{ 64 };   // 64B (AVX-512) or 32B (AVX2)
  size_t   peeling_elements{ 0 };      // Align to 64-byte cache line
  size_t   vector_body_iterations{ 0 };// Main vectorized unrolled loop
  size_t   residual_elements{ 0 };     // Scalar tail loop
  uint32_t estimated_ipc_boost_pct{ 0 };
  bool     can_fuse_adjacent_loops{ false };
};

class RedjetSuperloopWeaver {
public:
  static constexpr size_t MAX_UNROLL_BUFFER_BYTES = 65536;

private:
  hs_ticket_lock_t      _weaver_lock;

  // Telemetry
  std::atomic<uint64_t> _total_loops_analyzed{ 0 };
  std::atomic<uint64_t> _total_superloops_woven{ 0 };
  std::atomic<uint64_t> _total_pilf_fusions{ 0 };
  std::atomic<uint64_t> _total_vector_rotations{ 0 };
  std::atomic<uint64_t> _total_safety_traps_aborted{ 0 };

public:
  RedjetSuperloopWeaver();
  ~RedjetSuperloopWeaver();

  // 1. Loop Profile & Microarchitectural Port Analysis
  RJ_API RedjetSuperloopPlan analyze_and_plan(const RedjetLoopCharacteristics& chars) noexcept;

  // 2. Machine Code Superloop Weaving
  // Emits unrolled, port-saturated x86-64 machine code into target_buffer
  RJ_API size_t weave_superloop_x86_64(const RedjetSuperloopPlan& plan,
                                       const RedjetLoopCharacteristics& chars,
                                       uint8_t* target_buffer,
                                       size_t buffer_capacity);

  // 3. Bit-Exact Floating Point & Memory Invariant Safety Gate
  RJ_API bool verify_ieee754_exactness(const uint8_t* original_code, size_t orig_len,
                                       const uint8_t* woven_code, size_t woven_len) noexcept;

  // Telemetry
  uint64_t loops_analyzed()       const noexcept { return _total_loops_analyzed.load(std::memory_order_relaxed); }
  uint64_t superloops_woven()     const noexcept { return _total_superloops_woven.load(std::memory_order_relaxed); }
  uint64_t pilf_fusions()         const noexcept { return _total_pilf_fusions.load(std::memory_order_relaxed); }
  uint64_t vector_rotations()     const noexcept { return _total_vector_rotations.load(std::memory_order_relaxed); }
  uint64_t safety_aborts()        const noexcept { return _total_safety_traps_aborted.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ENGINE_REDJETSUPERLOOPWEAVER_HPP
