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

#ifndef SHARE_GC_REDJET_SIMD_REDJETSIMDMATRIX_HPP
#define SHARE_GC_REDJET_SIMD_REDJETSIMDMATRIX_HPP

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
// Redjet SIMD Bit-Matrix Acceleration Engine (PLAN.md Section 5.2, 7.4)
// - 512x512 2D Bit-Matrix for High-Speed Object Graph Reachability
// - Vectorized Boolean Matrix Multiplication for Transitive Closure
// - AVX-512 / AVX2 In-Register 64-Byte Transpose
// - Fits entirely within L1-D cache (32 KB footprint) for maximum IPC
// ============================================================================

class RedjetSimdMatrix {
public:
  static constexpr size_t MATRIX_DIM = 512;                    // 512 x 512 bits
  static constexpr size_t ROW_BYTES  = MATRIX_DIM / 8;         // 64 bytes per row (1 AVX-512 vector)
  static constexpr size_t TOTAL_BYTES = MATRIX_DIM * ROW_BYTES; // 32,768 bytes (32 KB = L1-D size)

private:
  alignas(64) uint8_t   _matrix[TOTAL_BYTES];
  hs_ticket_lock_t      _matrix_lock;

  // Telemetry
  std::atomic<uint64_t> _total_closures_computed{ 0 };
  std::atomic<uint64_t> _total_transpositions{ 0 };
  std::atomic<uint64_t> _total_bit_ops{ 0 };

public:
  RedjetSimdMatrix();
  ~RedjetSimdMatrix();

  void clear() noexcept;

  // 1. Bit Operations
  RJ_API void set_edge(size_t row, size_t col) noexcept;
  RJ_API void clear_edge(size_t row, size_t col) noexcept;
  RJ_API bool test_edge(size_t row, size_t col) const noexcept;

  // 2. Vectorized Row-wise Operations (512-bit vector bitwise OR / AND)
  RJ_API void or_row(size_t dest_row, size_t src_row) noexcept;
  RJ_API void and_row(size_t dest_row, size_t src_row) noexcept;

  // 3. Vectorized Transitive Reachability Closure (Warshall / SIMD matrix multiply)
  RJ_API void compute_transitive_closure();

  // 4. Matrix Transpose (512x512 bit-matrix transpose using vector shuffle/unpack)
  RJ_API void transpose_to(RedjetSimdMatrix& target) const;

  // 5. Total Active Reachability Edges
  RJ_API uint64_t count_active_edges() const noexcept;

  // Raw row accessor
  uint8_t* row_ptr(size_t row) noexcept { return &_matrix[row * ROW_BYTES]; }
  const uint8_t* row_ptr(size_t row) const noexcept { return &_matrix[row * ROW_BYTES]; }

  // Telemetry
  uint64_t closures_computed() const noexcept { return _total_closures_computed.load(std::memory_order_relaxed); }
  uint64_t transpositions()    const noexcept { return _total_transpositions.load(std::memory_order_relaxed); }
  uint64_t bit_ops()           const noexcept { return _total_bit_ops.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_SIMD_REDJETSIMDMATRIX_HPP
