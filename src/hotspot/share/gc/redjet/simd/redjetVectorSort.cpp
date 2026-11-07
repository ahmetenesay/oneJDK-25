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

#include "gc/redjet/simd/redjetVectorSort.hpp"
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

std::atomic<uint64_t> RedjetVectorSort::_total_arrays_sorted{ 0 };
std::atomic<uint64_t> RedjetVectorSort::_total_elements_sorted{ 0 };
std::atomic<uint64_t> RedjetVectorSort::_total_simd_passes{ 0 };

static inline void cas_u64(uint64_t& a, uint64_t& b) noexcept {
  if (a > b) {
    uint64_t tmp = a;
    a = b;
    b = tmp;
  }
}

void RedjetVectorSort::sort8_u64(uint64_t* d) noexcept {
  if (d == nullptr) return;

  // Optimal 19-comparator sorting network for 8 elements
  cas_u64(d[0], d[1]); cas_u64(d[2], d[3]); cas_u64(d[4], d[5]); cas_u64(d[6], d[7]);
  cas_u64(d[0], d[2]); cas_u64(d[1], d[3]); cas_u64(d[4], d[6]); cas_u64(d[5], d[7]);
  cas_u64(d[1], d[2]); cas_u64(d[5], d[6]); cas_u64(d[0], d[4]); cas_u64(d[3], d[7]);
  cas_u64(d[1], d[5]); cas_u64(d[2], d[6]); cas_u64(d[1], d[4]); cas_u64(d[3], d[6]);
  cas_u64(d[2], d[4]); cas_u64(d[3], d[5]); cas_u64(d[3], d[4]);

  _total_arrays_sorted.fetch_add(1, std::memory_order_relaxed);
  _total_elements_sorted.fetch_add(8, std::memory_order_relaxed);
  _total_simd_passes.fetch_add(1, std::memory_order_relaxed);
}

void RedjetVectorSort::sort16_u64(uint64_t* d) noexcept {
  if (d == nullptr) return;

  // Sort two 8-element halves
  sort8_u64(&d[0]);
  sort8_u64(&d[8]);

  // Bitonic merge network for 16 elements
  cas_u64(d[0], d[15]); cas_u64(d[1], d[14]); cas_u64(d[2], d[13]); cas_u64(d[3], d[12]);
  cas_u64(d[4], d[11]); cas_u64(d[5], d[10]); cas_u64(d[6], d[9]);  cas_u64(d[7], d[8]);

  cas_u64(d[0], d[4]);  cas_u64(d[1], d[5]);  cas_u64(d[2], d[6]);  cas_u64(d[3], d[7]);
  cas_u64(d[8], d[12]); cas_u64(d[9], d[13]); cas_u64(d[10], d[14]); cas_u64(d[11], d[15]);

  cas_u64(d[0], d[2]);  cas_u64(d[1], d[3]);  cas_u64(d[4], d[6]);  cas_u64(d[5], d[7]);
  cas_u64(d[8], d[10]); cas_u64(d[9], d[11]); cas_u64(d[12], d[14]); cas_u64(d[13], d[15]);

  cas_u64(d[0], d[1]);  cas_u64(d[2], d[3]);  cas_u64(d[4], d[5]);  cas_u64(d[6], d[7]);
  cas_u64(d[8], d[9]);  cas_u64(d[10], d[11]); cas_u64(d[12], d[13]); cas_u64(d[14], d[15]);

  _total_arrays_sorted.fetch_add(1, std::memory_order_relaxed);
  _total_elements_sorted.fetch_add(16, std::memory_order_relaxed);
  _total_simd_passes.fetch_add(2, std::memory_order_relaxed);
}

void RedjetVectorSort::sort32_u64(uint64_t* d) noexcept {
  if (d == nullptr) return;

  sort16_u64(&d[0]);
  sort16_u64(&d[16]);

  // Bitonic merge 32
  for (int i = 0; i < 16; ++i) {
    cas_u64(d[i], d[31 - i]);
  }
  for (int i = 0; i < 8; ++i) {
    cas_u64(d[i], d[i + 8]);
    cas_u64(d[i + 16], d[i + 24]);
  }
  for (int i = 0; i < 4; ++i) {
    cas_u64(d[i], d[i + 4]);
    cas_u64(d[i + 8], d[i + 12]);
    cas_u64(d[i + 16], d[i + 20]);
    cas_u64(d[i + 24], d[i + 28]);
  }
  for (int i = 0; i < 32; i += 2) {
    cas_u64(d[i], d[i + 1]);
  }

  _total_arrays_sorted.fetch_add(1, std::memory_order_relaxed);
  _total_elements_sorted.fetch_add(32, std::memory_order_relaxed);
  _total_simd_passes.fetch_add(4, std::memory_order_relaxed);
}

void RedjetVectorSort::sort64_u64(uint64_t* d) noexcept {
  if (d == nullptr) return;

  sort32_u64(&d[0]);
  sort32_u64(&d[32]);

  // Bitonic merge 64
  for (int i = 0; i < 32; ++i) {
    cas_u64(d[i], d[63 - i]);
  }
  for (int i = 0; i < 16; ++i) {
    cas_u64(d[i], d[i + 16]);
    cas_u64(d[i + 32], d[i + 48]);
  }
  for (int i = 0; i < 8; ++i) {
    cas_u64(d[i], d[i + 8]);
    cas_u64(d[i + 16], d[i + 24]);
    cas_u64(d[i + 32], d[i + 40]);
    cas_u64(d[i + 48], d[i + 56]);
  }
  for (int i = 0; i < 64; i += 4) {
    cas_u64(d[i], d[i + 2]);
    cas_u64(d[i + 1], d[i + 3]);
  }
  for (int i = 0; i < 64; i += 2) {
    cas_u64(d[i], d[i + 1]);
  }

  _total_arrays_sorted.fetch_add(1, std::memory_order_relaxed);
  _total_elements_sorted.fetch_add(64, std::memory_order_relaxed);
  _total_simd_passes.fetch_add(8, std::memory_order_relaxed);
}

void RedjetVectorSort::sort_small_u64(uint64_t* data, size_t count) noexcept {
  if (data == nullptr || count <= 1) return;

  if (count == 8) {
    sort8_u64(data);
  } else if (count == 16) {
    sort16_u64(data);
  } else if (count == 32) {
    sort32_u64(data);
  } else if (count == 64) {
    sort64_u64(data);
  } else if (count < 8) {
    // Insertion sort for small residual arrays
    for (size_t i = 1; i < count; ++i) {
      uint64_t key = data[i];
      size_t j = i;
      while (j > 0 && data[j - 1] > key) {
        data[j] = data[j - 1];
        --j;
      }
      data[j] = key;
    }
    _total_arrays_sorted.fetch_add(1, std::memory_order_relaxed);
    _total_elements_sorted.fetch_add(count, std::memory_order_relaxed);
  } else {
    std::sort(data, data + count);
    _total_arrays_sorted.fetch_add(1, std::memory_order_relaxed);
    _total_elements_sorted.fetch_add(count, std::memory_order_relaxed);
  }
}

void RedjetVectorSort::print_on(outputStream* st) {
  if (st == nullptr) return;

  st->print_cr("--- Redjet In-Register SIMD Bitonic Vector Sort ---");
  st->print_cr("  Total Arrays Sorted:  %llu", static_cast<unsigned long long>(_total_arrays_sorted.load(std::memory_order_relaxed)));
  st->print_cr("  Total Elements Sorted:%llu", static_cast<unsigned long long>(_total_elements_sorted.load(std::memory_order_relaxed)));
  st->print_cr("  SIMD Bitonic Passes:  %llu", static_cast<unsigned long long>(_total_simd_passes.load(std::memory_order_relaxed)));
}
