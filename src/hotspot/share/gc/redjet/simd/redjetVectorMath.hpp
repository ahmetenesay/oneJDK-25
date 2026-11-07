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

#ifndef SHARE_GC_REDJET_SIMD_REDJETVECTORMATH_HPP
#define SHARE_GC_REDJET_SIMD_REDJETVECTORMATH_HPP

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
// Multi-Architecture SIMD & Tensor Vector Math Library (PLAN.md Section 5.2, 7.4)
// - AVX-512, AVX2, ARM NEON / SVE, and Scalar Fallback
// - 512-bit Vectorized Memory Equality & Byte-Pattern Matcher
// - Vectorized Horizontal Reductions (Sum, Min, Max, Bitwise Popcount)
// - 512-bit High-Speed GC Hash & Checksum Pipeline
// - Fused Multiply-Accumulate (FMA) Vector Dot Products & Matrix Kernels
// ============================================================================

enum class RedjetSimdLevel : uint8_t {
  SCALAR_FALLBACK = 0,
  SSE4_2          = 1,
  AVX2            = 2,
  AVX512          = 3,
  ARM_NEON        = 4,
  ARM_SVE         = 5
};

class RedjetVectorMath {
private:
  static RedjetSimdLevel _active_level;
  static bool            _initialized;

  // Global SIMD execution telemetry
  static std::atomic<uint64_t> _total_vector_ops;
  static std::atomic<uint64_t> _total_bytes_processed;
  static std::atomic<uint64_t> _total_fma_elements;
  static std::atomic<uint64_t> _total_popcnt_vectors;

  static void detect_cpu_simd_capabilities();

public:
  static void init();
  static RedjetSimdLevel active_level() noexcept { return _active_level; }

  // 1. Vectorized Memory Equality (up to 64 bytes per cycle)
  RJ_API static bool mem_equal_512(const void* src1, const void* src2, size_t bytes) noexcept;

  // 2. Vectorized 64-bit Integer Horizontal Reductions
  RJ_API static uint64_t vector_sum_u64(const uint64_t* data, size_t count) noexcept;
  RJ_API static uint64_t vector_min_u64(const uint64_t* data, size_t count) noexcept;
  RJ_API static uint64_t vector_max_u64(const uint64_t* data, size_t count) noexcept;

  // 3. Vectorized Bitwise Population Count (count set bits)
  RJ_API static uint64_t vector_popcount_u64(const uint64_t* data, size_t count) noexcept;

  // 4. Vectorized 512-Bit Fast Hash (Object Classification & RPSS)
  RJ_API static uint64_t vector_hash_512(const void* data, size_t bytes, uint64_t seed) noexcept;

  // 5. Fused Multiply-Accumulate (FMA) Dot Product (Single Precision Float)
  RJ_API static float vector_dot_product_f32(const float* a, const float* b, size_t count) noexcept;

  // 6. Vectorized Linear Scale & Add (Y = alpha * X + Y)
  RJ_API static void vector_axpy_f32(float alpha, const float* x, float* y, size_t count) noexcept;

  // Telemetry accessors
  static uint64_t vector_ops()      noexcept { return _total_vector_ops.load(std::memory_order_relaxed); }
  static uint64_t bytes_processed() noexcept { return _total_bytes_processed.load(std::memory_order_relaxed); }
  static uint64_t fma_elements()    noexcept { return _total_fma_elements.load(std::memory_order_relaxed); }
  static uint64_t popcnt_vectors()  noexcept { return _total_popcnt_vectors.load(std::memory_order_relaxed); }

  static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_SIMD_REDJETVECTORMATH_HPP
