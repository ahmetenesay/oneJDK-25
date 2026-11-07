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

#include "gc/redjet/simd/redjetSimdMatrix.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetSimdMatrix::RedjetSimdMatrix()
  : _total_closures_computed(0),
    _total_transpositions(0),
    _total_bit_ops(0) {
  hs_ticket_lock_init(&_matrix_lock);
  clear();
}

RedjetSimdMatrix::~RedjetSimdMatrix() {
}

void RedjetSimdMatrix::clear() noexcept {
  std::memset(_matrix, 0, sizeof(_matrix));
}

void RedjetSimdMatrix::set_edge(size_t row, size_t col) noexcept {
  if (row >= MATRIX_DIM || col >= MATRIX_DIM) return;

  size_t byte_idx = row * ROW_BYTES + (col / 8);
  uint8_t bit_mask = static_cast<uint8_t>(1 << (col % 8));

  _matrix[byte_idx] |= bit_mask;
  _total_bit_ops.fetch_add(1, std::memory_order_relaxed);
}

void RedjetSimdMatrix::clear_edge(size_t row, size_t col) noexcept {
  if (row >= MATRIX_DIM || col >= MATRIX_DIM) return;

  size_t byte_idx = row * ROW_BYTES + (col / 8);
  uint8_t bit_mask = static_cast<uint8_t>(1 << (col % 8));

  _matrix[byte_idx] &= ~bit_mask;
  _total_bit_ops.fetch_add(1, std::memory_order_relaxed);
}

bool RedjetSimdMatrix::test_edge(size_t row, size_t col) const noexcept {
  if (row >= MATRIX_DIM || col >= MATRIX_DIM) return false;

  size_t byte_idx = row * ROW_BYTES + (col / 8);
  uint8_t bit_mask = static_cast<uint8_t>(1 << (col % 8));

  return (_matrix[byte_idx] & bit_mask) != 0;
}

void RedjetSimdMatrix::or_row(size_t dest_row, size_t src_row) noexcept {
  if (dest_row >= MATRIX_DIM || src_row >= MATRIX_DIM) return;

  uint8_t* dest_ptr = &_matrix[dest_row * ROW_BYTES];
  const uint8_t* src_ptr = &_matrix[src_row * ROW_BYTES];

#if defined(__AVX512F__)
  __m512i vdest = _mm512_loadu_si512(reinterpret_cast<const void*>(dest_ptr));
  __m512i vsrc  = _mm512_loadu_si512(reinterpret_cast<const void*>(src_ptr));
  __m512i vres  = _mm512_or_si512(vdest, vsrc);
  _mm512_storeu_si512(reinterpret_cast<void*>(dest_ptr), vres);
#elif defined(__AVX2__)
  __m256i* d = reinterpret_cast<__m256i*>(dest_ptr);
  const __m256i* s = reinterpret_cast<const __m256i*>(src_ptr);
  d[0] = _mm256_or_si256(d[0], s[0]);
  d[1] = _mm256_or_si256(d[1], s[1]);
#else
  uint64_t* d64 = reinterpret_cast<uint64_t*>(dest_ptr);
  const uint64_t* s64 = reinterpret_cast<const uint64_t*>(src_ptr);
  for (int i = 0; i < 8; ++i) {
    d64[i] |= s64[i];
  }
#endif

  _total_bit_ops.fetch_add(ROW_BYTES * 8, std::memory_order_relaxed);
}

void RedjetSimdMatrix::and_row(size_t dest_row, size_t src_row) noexcept {
  if (dest_row >= MATRIX_DIM || src_row >= MATRIX_DIM) return;

  uint8_t* dest_ptr = &_matrix[dest_row * ROW_BYTES];
  const uint8_t* src_ptr = &_matrix[src_row * ROW_BYTES];

#if defined(__AVX512F__)
  __m512i vdest = _mm512_loadu_si512(reinterpret_cast<const void*>(dest_ptr));
  __m512i vsrc  = _mm512_loadu_si512(reinterpret_cast<const void*>(src_ptr));
  __m512i vres  = _mm512_and_si512(vdest, vsrc);
  _mm512_storeu_si512(reinterpret_cast<void*>(dest_ptr), vres);
#elif defined(__AVX2__)
  __m256i* d = reinterpret_cast<__m256i*>(dest_ptr);
  const __m256i* s = reinterpret_cast<const __m256i*>(src_ptr);
  d[0] = _mm256_and_si256(d[0], s[0]);
  d[1] = _mm256_and_si256(d[1], s[1]);
#else
  uint64_t* d64 = reinterpret_cast<uint64_t*>(dest_ptr);
  const uint64_t* s64 = reinterpret_cast<const uint64_t*>(src_ptr);
  for (int i = 0; i < 8; ++i) {
    d64[i] &= s64[i];
  }
#endif

  _total_bit_ops.fetch_add(ROW_BYTES * 8, std::memory_order_relaxed);
}

void RedjetSimdMatrix::compute_transitive_closure() {
  hs_ticket_lock_acquire(&_matrix_lock);

  // Vectorized Warshall's Algorithm
  for (size_t k = 0; k < MATRIX_DIM; ++k) {
    for (size_t i = 0; i < MATRIX_DIM; ++i) {
      if (test_edge(i, k)) {
        or_row(i, k);
      }
    }
  }

  _total_closures_computed.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_matrix_lock);
}

void RedjetSimdMatrix::transpose_to(RedjetSimdMatrix& target) const {
  target.clear();

  for (size_t r = 0; r < MATRIX_DIM; ++r) {
    for (size_t c = 0; c < MATRIX_DIM; ++c) {
      if (test_edge(r, c)) {
        target.set_edge(c, r);
      }
    }
  }

  const_cast<std::atomic<uint64_t>&>(_total_transpositions).fetch_add(1, std::memory_order_relaxed);
}

uint64_t RedjetSimdMatrix::count_active_edges() const noexcept {
  const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(_matrix);
  size_t count64 = TOTAL_BYTES / sizeof(uint64_t);

  uint64_t total = 0;
  for (size_t i = 0; i < count64; ++i) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    total += __popcnt64(ptr64[i]);
#elif defined(__GNUC__) || defined(__clang__)
    total += __builtin_popcountll(ptr64[i]);
#else
    uint64_t x = ptr64[i];
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    total += (x * 0x0101010101010101ULL) >> 56;
#endif
  }

  return total;
}

void RedjetSimdMatrix::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet SIMD Bit-Matrix Acceleration Engine ---");
  st->print_cr("  Matrix Dimension:     %zu x %zu bits", MATRIX_DIM, MATRIX_DIM);
  st->print_cr("  Row Footprint:        %zu bytes (1 AVX-512 register)", ROW_BYTES);
  st->print_cr("  Total Size:           %zu bytes (L1-D cache resident)", TOTAL_BYTES);
  st->print_cr("  Active Edges:         %llu", static_cast<unsigned long long>(count_active_edges()));
  st->print_cr("  Closures Computed:    %llu", static_cast<unsigned long long>(_total_closures_computed.load(std::memory_order_relaxed)));
  st->print_cr("  Transpositions:       %llu", static_cast<unsigned long long>(_total_transpositions.load(std::memory_order_relaxed)));
  st->print_cr("  Bit Ops Executed:     %llu", static_cast<unsigned long long>(_total_bit_ops.load(std::memory_order_relaxed)));
}
