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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETSTRINGDEDUP_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETSTRINGDEDUP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "oops/oopsHierarchy.hpp"
#include "memory/allStatic.hpp"
#include "utilities/ostream.hpp"
#else
typedef void* oop;
class Klass {};
class AllStatic {};
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;

// ============================================================================
// 1. DEDUP TABLE ENTRY & LOCK-FREE LOCAL CACHE
// Caches recently deduplicated strings to avoid full table lookups.
// ============================================================================
struct alignas(64) RedjetDedupCacheEntry {
  uint64_t hash{ 0 };
  size_t   length_bytes{ 0 };
  oop      canonical_string{ nullptr };
  uint64_t last_hit_tsc{ 0 };
};

// ============================================================================
// 2. REDJET STRING DEDUP (PLAN.md Section 8.10.2)
// Concurrent mark-phase string deduplication accelerated by 512-bit AVX-512
// comparison kernels (_mm512_cmpeq_epi8_mask), AVX2, SSE4.2, and scalar fallbacks.
// Includes lock-free candidate ring buffers and vectorized hash computation.
// ============================================================================
class RedjetStringDedup : public AllStatic {
public:
  static constexpr size_t CANDIDATE_QUEUE_SIZE = 16384;
  static constexpr size_t CANDIDATE_QUEUE_MASK = CANDIDATE_QUEUE_SIZE - 1;
  static constexpr size_t CACHE_SIZE           = 4096;
  static constexpr size_t CACHE_MASK           = CACHE_SIZE - 1;

private:
  static std::atomic<bool>     _initialized;
  static std::atomic<uint64_t> _strings_examined;
  static std::atomic<uint64_t> _strings_deduplicated;
  static std::atomic<uint64_t> _bytes_saved;
  static std::atomic<uint64_t> _simd512_comparisons;
  static std::atomic<uint64_t> _simd256_comparisons;
  static std::atomic<uint64_t> _simd128_comparisons;
  static std::atomic<uint64_t> _scalar_comparisons;
  static std::atomic<uint64_t> _mismatches_found;
  static std::atomic<uint64_t> _identical_pointers;
  static std::atomic<uint64_t> _cache_hits;
  static std::atomic<uint64_t> _cache_misses;

  // Lock-free candidate ring buffer
  static oop                   _candidate_queue[CANDIDATE_QUEUE_SIZE];
  static alignas(64) std::atomic<uint64_t> _queue_head;
  static alignas(64) std::atomic<uint64_t> _queue_tail;
  static std::atomic<uint64_t> _queue_overflows;

  // Lock-free dedup acceleration cache
  static RedjetDedupCacheEntry _cache[CACHE_SIZE];

public:
  static void initialize();
  static void shutdown();

  // Candidate selection policy for mark phase
  static bool is_candidate_from_mark(oop java_string);

  // Enqueue candidate for concurrent processing
  static bool enqueue_candidate(oop java_string);

  // Drain queued candidates and deduplicate
  static size_t drain_and_deduplicate(size_t max_items = 2048);

  // 512-bit AVX-512 Vectorized String byte equality comparison kernel
  // Compares 64 bytes per cycle using _mm512_cmpeq_epi8_mask with 4x unrolling (256 bytes per loop)
  // and tiered fallbacks (AVX2, SSE4.2, ARM NEON, scalar).
  static bool compare_bytes_512(const uint8_t* a, const uint8_t* b, size_t length);

  // High-performance Latin1 / UTF-16 character comparison
  static bool compare_string_values(oop string_a, oop string_b);

  // Vectorized 64-bit hash computation for string byte buffers
  static uint64_t compute_hash_512(const uint8_t* data, size_t length);

  // Process deduplication request for a verified candidate string
  static bool deduplicate_string(oop java_string);

  // Telemetry & metrics accessors
  static bool     is_initialized()        noexcept { return _initialized.load(std::memory_order_relaxed); }
  static uint64_t strings_examined()      noexcept { return _strings_examined.load(std::memory_order_relaxed); }
  static uint64_t strings_deduplicated()  noexcept { return _strings_deduplicated.load(std::memory_order_relaxed); }
  static uint64_t bytes_saved()           noexcept { return _bytes_saved.load(std::memory_order_relaxed); }
  static uint64_t simd512_comparisons()   noexcept { return _simd512_comparisons.load(std::memory_order_relaxed); }
  static uint64_t simd256_comparisons()   noexcept { return _simd256_comparisons.load(std::memory_order_relaxed); }
  static uint64_t simd128_comparisons()   noexcept { return _simd128_comparisons.load(std::memory_order_relaxed); }
  static uint64_t scalar_comparisons()    noexcept { return _scalar_comparisons.load(std::memory_order_relaxed); }
  static uint64_t mismatches_found()      noexcept { return _mismatches_found.load(std::memory_order_relaxed); }
  static uint64_t identical_pointers()    noexcept { return _identical_pointers.load(std::memory_order_relaxed); }
  static uint64_t queue_overflows()       noexcept { return _queue_overflows.load(std::memory_order_relaxed); }
  static uint64_t cache_hits()            noexcept { return _cache_hits.load(std::memory_order_relaxed); }
  static uint64_t cache_misses()          noexcept { return _cache_misses.load(std::memory_order_relaxed); }

  static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETSTRINGDEDUP_HPP
