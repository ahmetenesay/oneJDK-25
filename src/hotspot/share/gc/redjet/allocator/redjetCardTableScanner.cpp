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

#include "gc/redjet/allocator/redjetCardTableScanner.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <immintrin.h>
#include <x86intrin.h>
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetCardTableScanner::RedjetCardTableScanner()
  : _card_table_base(nullptr),
    _card_table_size(0),
    _total_cards_scanned(0),
    _total_clean_chunks_skipped(0),
    _total_dirty_cards_found(0),
    _simd512_scan_loops(0),
    _simd256_scan_loops(0),
    _simd128_scan_loops(0),
    _stream_clear_bytes(0) {
  hs_ticket_lock_init(&_scanner_lock);
  _age_histogram.reset();
}

RedjetCardTableScanner::~RedjetCardTableScanner() {
  destroy();
}

void RedjetCardTableScanner::init(uint8_t* card_base, size_t card_size) {
  hs_ticket_lock_acquire(&_scanner_lock);
  _card_table_base = card_base;
  _card_table_size = card_size;
  hs_ticket_lock_release(&_scanner_lock);
}

void RedjetCardTableScanner::destroy() {
  hs_ticket_lock_acquire(&_scanner_lock);
  _card_table_base = nullptr;
  _card_table_size = 0;
  hs_ticket_lock_release(&_scanner_lock);
}

size_t RedjetCardTableScanner::scan_dirty_cards(size_t start_card, size_t count,
                                               RedjetDirtyCardCallback callback, void* ctx) {
  if (_card_table_base == nullptr || count == 0 || callback == nullptr) return 0;

  size_t end_card = std::min(start_card + count, _card_table_size);
  size_t cur = start_card;
  size_t dirty_count = 0;

  _total_cards_scanned.fetch_add(count, std::memory_order_relaxed);

#if defined(__AVX512F__) && defined(__AVX512BW__)
  // 1. 256-card (4 x 512-bit = 2048-bit) 4x Unrolled Vector Loop
  while (cur + (CARDS_PER_VECTOR * UNROLL_VECTOR_COUNT) <= end_card) {
    _simd512_scan_loops.fetch_add(1, std::memory_order_relaxed);

    const uint8_t* block0 = &_card_table_base[cur];
    const uint8_t* block1 = &_card_table_base[cur + 64];
    const uint8_t* block2 = &_card_table_base[cur + 128];
    const uint8_t* block3 = &_card_table_base[cur + 192];

    __m512i vec0 = _mm512_loadu_si512(reinterpret_cast<const void*>(block0));
    __m512i vec1 = _mm512_loadu_si512(reinterpret_cast<const void*>(block1));
    __m512i vec2 = _mm512_loadu_si512(reinterpret_cast<const void*>(block2));
    __m512i vec3 = _mm512_loadu_si512(reinterpret_cast<const void*>(block3));

    __mmask64 mask0 = _mm512_test_epi8_mask(vec0, vec0);
    __mmask64 mask1 = _mm512_test_epi8_mask(vec1, vec1);
    __mmask64 mask2 = _mm512_test_epi8_mask(vec2, vec2);
    __mmask64 mask3 = _mm512_test_epi8_mask(vec3, vec3);

    if ((mask0 | mask1 | mask2 | mask3) == 0) {
      // All 256 cards (128 KB heap) are completely clean: skip in a single branch check!
      _total_clean_chunks_skipped.fetch_add(4, std::memory_order_relaxed);
      cur += 256;
      continue;
    }

    // Process mask0
    __mmask64 m0 = mask0;
    while (m0 != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward64(&bit_pos, m0);
#else
      unsigned long bit_pos = __builtin_ctzll(m0);
#endif
      size_t card_idx = cur + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      m0 &= (m0 - 1);
    }

    // Process mask1
    __mmask64 m1 = mask1;
    while (m1 != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward64(&bit_pos, m1);
#else
      unsigned long bit_pos = __builtin_ctzll(m1);
#endif
      size_t card_idx = cur + 64 + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      m1 &= (m1 - 1);
    }

    // Process mask2
    __mmask64 m2 = mask2;
    while (m2 != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward64(&bit_pos, m2);
#else
      unsigned long bit_pos = __builtin_ctzll(m2);
#endif
      size_t card_idx = cur + 128 + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      m2 &= (m2 - 1);
    }

    // Process mask3
    __mmask64 m3 = mask3;
    while (m3 != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward64(&bit_pos, m3);
#else
      unsigned long bit_pos = __builtin_ctzll(m3);
#endif
      size_t card_idx = cur + 192 + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      m3 &= (m3 - 1);
    }

    cur += 256;
  }

  // Single 64-card AVX-512 chunks
  while (cur + CARDS_PER_VECTOR <= end_card) {
    _simd512_scan_loops.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* block = &_card_table_base[cur];
    __m512i vec = _mm512_loadu_si512(reinterpret_cast<const void*>(block));
    __mmask64 mask = _mm512_test_epi8_mask(vec, vec);

    if (mask == 0) {
      _total_clean_chunks_skipped.fetch_add(1, std::memory_order_relaxed);
      cur += CARDS_PER_VECTOR;
      continue;
    }

    while (mask != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward64(&bit_pos, mask);
#else
      unsigned long bit_pos = __builtin_ctzll(mask);
#endif
      size_t card_idx = cur + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      mask &= (mask - 1);
    }
    cur += CARDS_PER_VECTOR;
  }
#elif defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
  // AVX2 32-card chunks
  while (cur + 32 <= end_card) {
    _simd256_scan_loops.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* block = &_card_table_base[cur];
    __m256i vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block));
    if (_mm256_testz_si256(vec, vec)) {
      _total_clean_chunks_skipped.fetch_add(1, std::memory_order_relaxed);
      cur += 32;
      continue;
    }

    __m256i zero = _mm256_setzero_si256();
    __m256i cmp  = _mm256_cmpeq_epi8(vec, zero);
    uint32_t mask = ~static_cast<uint32_t>(_mm256_movemask_epi8(cmp));

    while (mask != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward(&bit_pos, mask);
#else
      unsigned long bit_pos = __builtin_ctz(mask);
#endif
      size_t card_idx = cur + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      mask &= (mask - 1);
    }
    cur += 32;
  }
#elif defined(__SSE4_2__) || (defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86)))
  // SSE4.2 16-card chunks
  while (cur + 16 <= end_card) {
    _simd128_scan_loops.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* block = &_card_table_base[cur];
    __m128i vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block));
    if (_mm_testz_si128(vec, vec)) {
      _total_clean_chunks_skipped.fetch_add(1, std::memory_order_relaxed);
      cur += 16;
      continue;
    }

    __m128i zero = _mm_setzero_si128();
    __m128i cmp  = _mm_cmpeq_epi8(vec, zero);
    uint32_t mask = (~static_cast<uint32_t>(_mm_movemask_epi8(cmp))) & 0xFFFF;

    while (mask != 0) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      unsigned long bit_pos = 0;
      _BitScanForward(&bit_pos, mask);
#else
      unsigned long bit_pos = __builtin_ctz(mask);
#endif
      size_t card_idx = cur + bit_pos;
      _age_histogram.record_age(_card_table_base[card_idx]);
      callback(card_idx, ctx);
      dirty_count++;
      mask &= (mask - 1);
    }
    cur += 16;
  }
#endif

  // 64-bit scalar loop for remaining 8-byte chunks
  while (cur + 8 <= end_card) {
    uint64_t val = *reinterpret_cast<const uint64_t*>(&_card_table_base[cur]);
    if (val != 0) {
      for (size_t b = 0; b < 8; ++b) {
        if (_card_table_base[cur + b] != 0) {
          _age_histogram.record_age(_card_table_base[cur + b]);
          callback(cur + b, ctx);
          dirty_count++;
        }
      }
    }
    cur += 8;
  }

  // Byte-by-byte tail
  while (cur < end_card) {
    if (_card_table_base[cur] != 0) {
      _age_histogram.record_age(_card_table_base[cur]);
      callback(cur, ctx);
      dirty_count++;
    }
    cur++;
  }

  _total_dirty_cards_found.fetch_add(dirty_count, std::memory_order_relaxed);
  return dirty_count;
}

size_t RedjetCardTableScanner::scan_dirty_cards_parallel(size_t start_card, size_t count,
                                                        uint32_t num_workers,
                                                        RedjetDirtyCardCallback callback, void* ctx) {
  if (num_workers <= 1 || count < 1024) {
    return scan_dirty_cards(start_card, count, callback, ctx);
  }

  size_t total_dirty = 0;
  size_t chunk_size = (count + num_workers - 1) / num_workers;
  // Align chunk size to 64 cards to prevent cache line sharing
  chunk_size = (chunk_size + 63) & ~63ULL;

  for (uint32_t w = 0; w < num_workers; ++w) {
    size_t chunk_start = start_card + w * chunk_size;
    if (chunk_start >= start_card + count) break;
    size_t chunk_count = std::min(chunk_size, (start_card + count) - chunk_start);
    total_dirty += scan_dirty_cards(chunk_start, chunk_count, callback, ctx);
  }

  return total_dirty;
}

void RedjetCardTableScanner::clear_card_range_stream(size_t start_card, size_t count) {
  if (_card_table_base == nullptr || count == 0) return;

  size_t end_card = std::min(start_card + count, _card_table_size);
  size_t cur = start_card;

#if defined(__AVX512F__)
  __m512i zero512 = _mm512_setzero_si512();

  // 4x unrolled non-temporal streaming store loop: 256 cards (128 KB) per loop
  while (cur + 256 <= end_card) {
    _mm512_stream_si512(reinterpret_cast<void*>(&_card_table_base[cur]), zero512);
    _mm512_stream_si512(reinterpret_cast<void*>(&_card_table_base[cur + 64]), zero512);
    _mm512_stream_si512(reinterpret_cast<void*>(&_card_table_base[cur + 128]), zero512);
    _mm512_stream_si512(reinterpret_cast<void*>(&_card_table_base[cur + 192]), zero512);
    cur += 256;
  }

  while (cur + 64 <= end_card) {
    _mm512_stream_si512(reinterpret_cast<void*>(&_card_table_base[cur]), zero512);
    cur += 64;
  }
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
  _mm_sfence();
#elif defined(__x86_64__) || defined(__i386__)
  _mm_sfence();
#endif
#elif defined(__AVX2__)
  __m256i zero256 = _mm256_setzero_si256();
  while (cur + 32 <= end_card) {
    _mm256_stream_si256(reinterpret_cast<__m256i*>(&_card_table_base[cur]), zero256);
    cur += 32;
  }
  _mm_sfence();
#endif

  if (cur < end_card) {
    std::memset(&_card_table_base[cur], 0, end_card - cur);
  }

  _stream_clear_bytes.fetch_add(count, std::memory_order_relaxed);
}

void RedjetCardTableScanner::clear_card_range(size_t start_card, size_t count) {
  if (_card_table_base == nullptr || count == 0) return;

  size_t end_card = std::min(start_card + count, _card_table_size);
  std::memset(&_card_table_base[start_card], 0, end_card - start_card);
}

void RedjetCardTableScanner::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet 512-bit Vectorized Card Table Scanner ---");
  st->print_cr("  Card Table Base:         %p", _card_table_base);
  st->print_cr("  Card Table Size (cards): %zu (%zu MB heap capacity)",
               _card_table_size, (_card_table_size * CARD_SIZE) / (1024 * 1024));
  st->print_cr("  Cards Scanned:           %llu", (unsigned long long)_total_cards_scanned.load(std::memory_order_relaxed));
  st->print_cr("  Clean Chunks Skipped:    %llu", (unsigned long long)_total_clean_chunks_skipped.load(std::memory_order_relaxed));
  st->print_cr("  Dirty Cards Found:       %llu", (unsigned long long)_total_dirty_cards_found.load(std::memory_order_relaxed));
  st->print_cr("  AVX-512 Scan Loops:      %llu", (unsigned long long)_simd512_scan_loops.load(std::memory_order_relaxed));
  st->print_cr("  AVX2 Scan Loops:         %llu", (unsigned long long)_simd256_scan_loops.load(std::memory_order_relaxed));
  st->print_cr("  SSE4.2 Scan Loops:       %llu", (unsigned long long)_simd128_scan_loops.load(std::memory_order_relaxed));
  st->print_cr("  Stream Cleared Cards:    %llu", (unsigned long long)_stream_clear_bytes.load(std::memory_order_relaxed));

  st->print_cr("  Card Mutation Age Distribution (0..15):");
  for (int i = 0; i < 16; ++i) {
    st->print_cr("    Age [%02d]: %llu", i, (unsigned long long)_age_histogram.age_buckets[i].load(std::memory_order_relaxed));
  }
}
