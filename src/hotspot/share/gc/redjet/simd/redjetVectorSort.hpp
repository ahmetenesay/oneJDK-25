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

#ifndef SHARE_GC_REDJET_SIMD_REDJETVECTORSORT_HPP
#define SHARE_GC_REDJET_SIMD_REDJETVECTORSORT_HPP

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
// Redjet In-Register SIMD Bitonic Vector Sort (PLAN.md Section 4.1, 5.2)
// - Sorts 8, 16, 32, and 64 64-bit pointers entirely in AVX-512 / AVX2 registers
// - Replaces std::sort in Microsort for cache-line colocation packing
// - 0 memory round-trips, branchless bitonic compare-and-swap network
// ============================================================================

class RedjetVectorSort {
private:
  // Global telemetry
  static std::atomic<uint64_t> _total_arrays_sorted;
  static std::atomic<uint64_t> _total_elements_sorted;
  static std::atomic<uint64_t> _total_simd_passes;

public:
  // 1. In-Register 8-Element 64-bit Pointer Sort (1 AVX-512 vector)
  RJ_API static void sort8_u64(uint64_t* data) noexcept;

  // 2. In-Register 16-Element 64-bit Pointer Sort (2 AVX-512 vectors)
  RJ_API static void sort16_u64(uint64_t* data) noexcept;

  // 3. In-Register 32-Element 64-bit Pointer Sort (4 AVX-512 vectors)
  RJ_API static void sort32_u64(uint64_t* data) noexcept;

  // 4. In-Register 64-Element 64-bit Pointer Sort (8 AVX-512 vectors)
  RJ_API static void sort64_u64(uint64_t* data) noexcept;

  // 5. General Small Array Sort (dispatches to optimal vector kernel)
  RJ_API static void sort_small_u64(uint64_t* data, size_t count) noexcept;

  // Telemetry
  static uint64_t arrays_sorted()   noexcept { return _total_arrays_sorted.load(std::memory_order_relaxed); }
  static uint64_t elements_sorted() noexcept { return _total_elements_sorted.load(std::memory_order_relaxed); }
  static uint64_t simd_passes()     noexcept { return _total_simd_passes.load(std::memory_order_relaxed); }

  static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_SIMD_REDJETVECTORSORT_HPP
