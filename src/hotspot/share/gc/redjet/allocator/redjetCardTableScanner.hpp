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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETCARDTABLESCANNER_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETCARDTABLESCANNER_HPP

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
// Redjet 512-bit Vectorized Card Table Scanner (PLAN.md Section 3.1, 4.2)
// - Scans 64 card bytes per cycle using AVX-512 _mm512_test_epi8_mask
// - 4x unrolled loop scans 256 cards (128 KB heap) per loop iteration
// - Skips clean heap regions with 0-branch SIMD masks
// - Extracts dirty card indices with hardware trailing zero count (_tzcnt_u64)
// - Non-temporal streaming stores (_mm512_stream_si512) for cache-sparing clearing
// ============================================================================

typedef void (*RedjetDirtyCardCallback)(size_t card_index, void* context);

struct alignas(64) RedjetCardAgeHistogram {
  std::atomic<uint64_t> age_buckets[16];

  void reset() noexcept {
    for (int i = 0; i < 16; ++i) {
      age_buckets[i].store(0, std::memory_order_relaxed);
    }
  }

  void record_age(uint8_t card_val) noexcept {
    uint32_t age = card_val & 0x0F;
    age_buckets[age].fetch_add(1, std::memory_order_relaxed);
  }
};

class RedjetCardTableScanner {
public:
  static constexpr size_t CARDS_PER_VECTOR   = 64;  // 64 bytes = 512 bits
  static constexpr size_t UNROLL_VECTOR_COUNT = 4;   // 4 x 64 = 256 cards per loop
  static constexpr size_t CARD_SHIFT         = 9;   // 512 bytes per card
  static constexpr size_t CARD_SIZE          = 1ULL << CARD_SHIFT;

private:
  uint8_t*               _card_table_base{ nullptr };
  size_t                 _card_table_size{ 0 };
  hs_ticket_lock_t       _scanner_lock;
  RedjetCardAgeHistogram _age_histogram;

  // Telemetry & metrics
  std::atomic<uint64_t>  _total_cards_scanned{ 0 };
  std::atomic<uint64_t>  _total_clean_chunks_skipped{ 0 };
  std::atomic<uint64_t>  _total_dirty_cards_found{ 0 };
  std::atomic<uint64_t>  _simd512_scan_loops{ 0 };
  std::atomic<uint64_t>  _simd256_scan_loops{ 0 };
  std::atomic<uint64_t>  _simd128_scan_loops{ 0 };
  std::atomic<uint64_t>  _stream_clear_bytes{ 0 };

public:
  RedjetCardTableScanner();
  ~RedjetCardTableScanner();

  void init(uint8_t* card_base, size_t card_size);
  void destroy();

  // 1. Vectorized Card Table Scan (AVX-512 unrolled, AVX2, SSE4.2, scalar)
  RJ_API size_t scan_dirty_cards(size_t start_card, size_t count,
                                 RedjetDirtyCardCallback callback, void* ctx);

  // 2. Parallel chunk scanning across Domestic workers
  RJ_API size_t scan_dirty_cards_parallel(size_t start_card, size_t count,
                                          uint32_t num_workers,
                                          RedjetDirtyCardCallback callback, void* ctx);

  // 3. Non-temporal cache-sparing clear (clears dirty marks in bulk without cache pollution)
  RJ_API void clear_card_range_stream(size_t start_card, size_t count);

  // 4. Standard clear card range
  RJ_API void clear_card_range(size_t start_card, size_t count);

  // 5. Card index to heap word translation
  inline uintptr_t card_to_heap_addr(size_t card_idx) const noexcept {
    return card_idx << CARD_SHIFT;
  }

  inline size_t heap_addr_to_card(uintptr_t addr) const noexcept {
    return addr >> CARD_SHIFT;
  }

  // Telemetry accessors
  uint64_t total_cards_scanned()        const noexcept { return _total_cards_scanned.load(std::memory_order_relaxed); }
  uint64_t total_clean_chunks_skipped() const noexcept { return _total_clean_chunks_skipped.load(std::memory_order_relaxed); }
  uint64_t total_dirty_cards_found()    const noexcept { return _total_dirty_cards_found.load(std::memory_order_relaxed); }
  uint64_t simd512_scan_loops()         const noexcept { return _simd512_scan_loops.load(std::memory_order_relaxed); }
  uint64_t simd256_scan_loops()         const noexcept { return _simd256_scan_loops.load(std::memory_order_relaxed); }
  uint64_t simd128_scan_loops()         const noexcept { return _simd128_scan_loops.load(std::memory_order_relaxed); }
  uint64_t stream_clear_bytes()         const noexcept { return _stream_clear_bytes.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETCARDTABLESCANNER_HPP
