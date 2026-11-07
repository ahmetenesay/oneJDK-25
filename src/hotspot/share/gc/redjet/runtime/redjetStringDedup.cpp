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

#include "gc/redjet/runtime/redjetStringDedup.hpp"
#include <cstring>
#include <chrono>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <immintrin.h>
#include <intrin.h>
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <x86intrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/stringdedup/stringDedupTable.hpp"
#include "logging/log.hpp"
#include "oops/access.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

std::atomic<bool>     RedjetStringDedup::_initialized{ false };
std::atomic<uint64_t> RedjetStringDedup::_strings_examined{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_strings_deduplicated{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_bytes_saved{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_simd512_comparisons{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_simd256_comparisons{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_simd128_comparisons{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_scalar_comparisons{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_mismatches_found{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_identical_pointers{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_cache_hits{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_cache_misses{ 0 };

oop                   RedjetStringDedup::_candidate_queue[CANDIDATE_QUEUE_SIZE];
alignas(64) std::atomic<uint64_t> RedjetStringDedup::_queue_head{ 0 };
alignas(64) std::atomic<uint64_t> RedjetStringDedup::_queue_tail{ 0 };
std::atomic<uint64_t> RedjetStringDedup::_queue_overflows{ 0 };

RedjetDedupCacheEntry RedjetStringDedup::_cache[CACHE_SIZE];

static inline uint64_t rj_string_read_tsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
  return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t val;
  asm volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#else
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

void RedjetStringDedup::initialize() {
  _queue_head.store(0, std::memory_order_relaxed);
  _queue_tail.store(0, std::memory_order_relaxed);
  _queue_overflows.store(0, std::memory_order_relaxed);
  for (size_t i = 0; i < CANDIDATE_QUEUE_SIZE; ++i) {
    _candidate_queue[i] = nullptr;
  }
  for (size_t i = 0; i < CACHE_SIZE; ++i) {
    _cache[i].hash = 0;
    _cache[i].length_bytes = 0;
    _cache[i].canonical_string = nullptr;
    _cache[i].last_hit_tsc = 0;
  }
  _initialized.store(true, std::memory_order_release);
}

void RedjetStringDedup::shutdown() {
  _initialized.store(false, std::memory_order_release);
}

bool RedjetStringDedup::is_candidate_from_mark(oop java_string) {
  if (java_string == nullptr) return false;

#ifndef REDJET_STANDALONE_BUILD
  if (!StringDedup::is_enabled()) return false;
  if (!java_lang_String::is_instance(java_string)) return false;

  // Check if string has a valid non-empty byte buffer
  typeArrayOop value = java_lang_String::value_no_keepalive(java_string);
  if (value == nullptr || value->length() == 0) {
    return false;
  }

  _strings_examined.fetch_add(1, std::memory_order_relaxed);
  return true;
#else
  _strings_examined.fetch_add(1, std::memory_order_relaxed);
  return true;
#endif
}

bool RedjetStringDedup::enqueue_candidate(oop java_string) {
  if (java_string == nullptr || !_initialized.load(std::memory_order_relaxed)) {
    return false;
  }

  uint64_t t = _queue_tail.load(std::memory_order_relaxed);
  uint64_t h = _queue_head.load(std::memory_order_acquire);

  if (t - h >= CANDIDATE_QUEUE_SIZE) {
    _queue_overflows.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  _candidate_queue[t & CANDIDATE_QUEUE_MASK] = java_string;
  _queue_tail.store(t + 1, std::memory_order_release);
  return true;
}

size_t RedjetStringDedup::drain_and_deduplicate(size_t max_items) {
  if (!_initialized.load(std::memory_order_relaxed)) return 0;

  size_t processed = 0;
  while (processed < max_items) {
    uint64_t h = _queue_head.load(std::memory_order_relaxed);
    uint64_t t = _queue_tail.load(std::memory_order_acquire);
    if (h >= t) break;

    oop candidate = _candidate_queue[h & CANDIDATE_QUEUE_MASK];
    _candidate_queue[h & CANDIDATE_QUEUE_MASK] = nullptr;
    _queue_head.store(h + 1, std::memory_order_release);

    if (candidate != nullptr) {
      deduplicate_string(candidate);
      processed++;
    }
  }
  return processed;
}

bool RedjetStringDedup::compare_bytes_512(const uint8_t* a, const uint8_t* b, size_t length) {
  if (a == b) {
    _identical_pointers.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (a == nullptr || b == nullptr) {
    _mismatches_found.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  size_t offset = 0;

#if (defined(__AVX512F__) && defined(__AVX512BW__)) || (defined(_MSC_VER) && defined(__AVX512BW__))
  // 512-bit vector loop: 4x unrolled (256 bytes per iteration)
  _simd512_comparisons.fetch_add(1, std::memory_order_relaxed);

  while (offset + 256 <= length) {
    __m512i va0 = _mm512_loadu_si512(reinterpret_cast<const void*>(a + offset));
    __m512i vb0 = _mm512_loadu_si512(reinterpret_cast<const void*>(b + offset));
    __mmask64 m0 = _mm512_cmpeq_epi8_mask(va0, vb0);

    __m512i va1 = _mm512_loadu_si512(reinterpret_cast<const void*>(a + offset + 64));
    __m512i vb1 = _mm512_loadu_si512(reinterpret_cast<const void*>(b + offset + 64));
    __mmask64 m1 = _mm512_cmpeq_epi8_mask(va1, vb1);

    __m512i va2 = _mm512_loadu_si512(reinterpret_cast<const void*>(a + offset + 128));
    __m512i vb2 = _mm512_loadu_si512(reinterpret_cast<const void*>(b + offset + 128));
    __mmask64 m2 = _mm512_cmpeq_epi8_mask(va2, vb2);

    __m512i va3 = _mm512_loadu_si512(reinterpret_cast<const void*>(a + offset + 192));
    __m512i vb3 = _mm512_loadu_si512(reinterpret_cast<const void*>(b + offset + 192));
    __mmask64 m3 = _mm512_cmpeq_epi8_mask(va3, vb3);

    if ((m0 & m1 & m2 & m3) != 0xFFFFFFFFFFFFFFFFULL) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset += 256;
  }

  // Single 64-byte chunks
  while (offset + 64 <= length) {
    __m512i va = _mm512_loadu_si512(reinterpret_cast<const void*>(a + offset));
    __m512i vb = _mm512_loadu_si512(reinterpret_cast<const void*>(b + offset));
    __mmask64 mask = _mm512_cmpeq_epi8_mask(va, vb);
    if (mask != 0xFFFFFFFFFFFFFFFFULL) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset += 64;
  }
#elif defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
  _simd256_comparisons.fetch_add(1, std::memory_order_relaxed);
  // 256-bit AVX2 fallback: 32 bytes per cycle
  while (offset + 32 <= length) {
    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + offset));
    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + offset));
    __m256i vcmp = _mm256_cmpeq_epi8(va, vb);
    int mask = _mm256_movemask_epi8(vcmp);
    if (mask != -1) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset += 32;
  }
#elif defined(__SSE4_2__) || (defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86)))
  _simd128_comparisons.fetch_add(1, std::memory_order_relaxed);
  // 128-bit SSE4.2 fallback: 16 bytes per cycle
  while (offset + 16 <= length) {
    __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + offset));
    __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + offset));
    __m128i vcmp = _mm_cmpeq_epi8(va, vb);
    int mask = _mm_movemask_epi8(vcmp);
    if (mask != 0xFFFF) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset += 16;
  }
#elif defined(__aarch64__)
  _simd128_comparisons.fetch_add(1, std::memory_order_relaxed);
  // ARM NEON 128-bit fallback
  while (offset + 16 <= length) {
    uint8x16_t va = vld1q_u8(a + offset);
    uint8x16_t vb = vld1q_u8(b + offset);
    uint8x16_t vcmp = vceqq_u8(va, vb);
    uint64x2_t v64 = vreinterpretq_u64_u8(vcmp);
    if (vgetq_lane_u64(v64, 0) != 0xFFFFFFFFFFFFFFFFULL ||
        vgetq_lane_u64(v64, 1) != 0xFFFFFFFFFFFFFFFFULL) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset += 16;
  }
#endif

  _scalar_comparisons.fetch_add(1, std::memory_order_relaxed);

  // 64-bit scalar loop for remaining 8-byte words
  while (offset + 8 <= length) {
    uint64_t qa = *reinterpret_cast<const uint64_t*>(a + offset);
    uint64_t qb = *reinterpret_cast<const uint64_t*>(b + offset);
    if (qa != qb) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset += 8;
  }

  // Byte-by-byte tail
  while (offset < length) {
    if (a[offset] != b[offset]) {
      _mismatches_found.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    offset++;
  }

  return true;
}

uint64_t RedjetStringDedup::compute_hash_512(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) return 0;

  static constexpr uint64_t PRIME1 = 11400714785074694791ULL;
  static constexpr uint64_t PRIME2 = 14029467366897019727ULL;
  static constexpr uint64_t PRIME3 = 1609587929392839161ULL;
  static constexpr uint64_t PRIME4 = 9650029242287828579ULL;
  static constexpr uint64_t PRIME5 = 2870177924486656056ULL;

  uint64_t hash = length * PRIME5;
  size_t offset = 0;

  while (offset + 32 <= length) {
    uint64_t k1 = *reinterpret_cast<const uint64_t*>(data + offset);
    uint64_t k2 = *reinterpret_cast<const uint64_t*>(data + offset + 8);
    uint64_t k3 = *reinterpret_cast<const uint64_t*>(data + offset + 16);
    uint64_t k4 = *reinterpret_cast<const uint64_t*>(data + offset + 24);

    k1 *= PRIME1; k1 = (k1 << 31) | (k1 >> 33); k1 *= PRIME2; hash ^= k1; hash = (hash << 27) | (hash >> 37); hash = hash * 5 + PRIME1;
    k2 *= PRIME2; k2 = (k2 << 31) | (k2 >> 33); k2 *= PRIME3; hash ^= k2; hash = (hash << 27) | (hash >> 37); hash = hash * 5 + PRIME2;
    k3 *= PRIME3; k3 = (k3 << 31) | (k3 >> 33); k3 *= PRIME4; hash ^= k3; hash = (hash << 27) | (hash >> 37); hash = hash * 5 + PRIME3;
    k4 *= PRIME4; k4 = (k4 << 31) | (k4 >> 33); k4 *= PRIME1; hash ^= k4; hash = (hash << 27) | (hash >> 37); hash = hash * 5 + PRIME4;

    offset += 32;
  }

  while (offset + 8 <= length) {
    uint64_t k = *reinterpret_cast<const uint64_t*>(data + offset);
    k *= PRIME2; k = (k << 31) | (k >> 33); k *= PRIME1;
    hash ^= k;
    hash = (hash << 27) | (hash >> 37);
    hash = hash * 5 + PRIME4;
    offset += 8;
  }

  while (offset < length) {
    hash ^= static_cast<uint64_t>(data[offset]) * PRIME5;
    hash = (hash << 11) | (hash >> 53);
    hash *= PRIME1;
    offset++;
  }

  hash ^= (hash >> 33);
  hash *= PRIME2;
  hash ^= (hash >> 29);
  hash *= PRIME3;
  hash ^= (hash >> 32);

  return hash;
}

bool RedjetStringDedup::compare_string_values(oop string_a, oop string_b) {
  if (string_a == string_b) return true;
  if (string_a == nullptr || string_b == nullptr) return false;

#ifndef REDJET_STANDALONE_BUILD
  typeArrayOop val_a = java_lang_String::value_no_keepalive(string_a);
  typeArrayOop val_b = java_lang_String::value_no_keepalive(string_b);

  if (val_a == val_b) return true;
  if (val_a == nullptr || val_b == nullptr) return false;

  int len_a = val_a->length();
  int len_b = val_b->length();
  if (len_a != len_b) return false;

  const uint8_t* ptr_a = reinterpret_cast<const uint8_t*>(val_a->byte_at_addr(0));
  const uint8_t* ptr_b = reinterpret_cast<const uint8_t*>(val_b->byte_at_addr(0));

  return compare_bytes_512(ptr_a, ptr_b, static_cast<size_t>(len_a));
#else
  return true;
#endif
}

bool RedjetStringDedup::deduplicate_string(oop java_string) {
  if (java_string == nullptr) return false;
  _strings_examined.fetch_add(1, std::memory_order_relaxed);

#ifndef REDJET_STANDALONE_BUILD
  if (!StringDedup::is_enabled()) return false;

  typeArrayOop val = java_lang_String::value_no_keepalive(java_string);
  if (val == nullptr) return false;

  size_t length_bytes = val->length() * sizeof(jbyte);
  if (length_bytes == 0) return false;

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(val->byte_at_addr(0));
  uint64_t hash = compute_hash_512(bytes, length_bytes);

  // Check local cache
  size_t cache_idx = hash & CACHE_MASK;
  RedjetDedupCacheEntry& entry = _cache[cache_idx];

  if (entry.hash == hash && entry.length_bytes == length_bytes && entry.canonical_string != nullptr) {
    if (compare_string_values(entry.canonical_string, java_string)) {
      _cache_hits.fetch_add(1, std::memory_order_relaxed);
      entry.last_hit_tsc = rj_string_read_tsc();
      // Enqueue to HotSpot StringDedupTable for canonical pointer replacement
      StringDedup::Requests requests;
      requests.add(java_string);

      _strings_deduplicated.fetch_add(1, std::memory_order_relaxed);
      _bytes_saved.fetch_add(length_bytes, std::memory_order_relaxed);
      return true;
    }
  }

  _cache_misses.fetch_add(1, std::memory_order_relaxed);

  // Update cache entry with current string as canonical candidate
  entry.hash = hash;
  entry.length_bytes = length_bytes;
  entry.canonical_string = java_string;
  entry.last_hit_tsc = rj_string_read_tsc();

  // Enqueue to HotSpot StringDedupTable for concurrent global table lookup
  StringDedup::Requests requests;
  requests.add(java_string);

  return false;
#else
  return false;
#endif
}

void RedjetStringDedup::print_on(outputStream* st) {
  if (st == nullptr) return;
  st->print_cr("--- Redjet String Deduplication (512-bit SIMD & Hash Cache) ---");
  st->print_cr("  Strings Examined:        %llu", (unsigned long long)_strings_examined.load(std::memory_order_relaxed));
  st->print_cr("  Strings Deduplicated:    %llu", (unsigned long long)_strings_deduplicated.load(std::memory_order_relaxed));
  st->print_cr("  Bytes Reclaimed/Saved:   %llu bytes (%llu KB)",
               (unsigned long long)_bytes_saved.load(std::memory_order_relaxed),
               (unsigned long long)(_bytes_saved.load(std::memory_order_relaxed) / 1024));
  st->print_cr("  512-bit AVX-512 Checks:  %llu", (unsigned long long)_simd512_comparisons.load(std::memory_order_relaxed));
  st->print_cr("  256-bit AVX2 Checks:     %llu", (unsigned long long)_simd256_comparisons.load(std::memory_order_relaxed));
  st->print_cr("  128-bit SSE/NEON Checks: %llu", (unsigned long long)_simd128_comparisons.load(std::memory_order_relaxed));
  st->print_cr("  Scalar Checks:           %llu", (unsigned long long)_scalar_comparisons.load(std::memory_order_relaxed));
  st->print_cr("  Mismatches Found:        %llu", (unsigned long long)_mismatches_found.load(std::memory_order_relaxed));
  st->print_cr("  Identical Pointer Hits:  %llu", (unsigned long long)_identical_pointers.load(std::memory_order_relaxed));
  st->print_cr("  Candidate Queue Drops:   %llu", (unsigned long long)_queue_overflows.load(std::memory_order_relaxed));
  st->print_cr("  Dedup Cache Hits:        %llu", (unsigned long long)_cache_hits.load(std::memory_order_relaxed));
  st->print_cr("  Dedup Cache Misses:      %llu", (unsigned long long)_cache_misses.load(std::memory_order_relaxed));
}
