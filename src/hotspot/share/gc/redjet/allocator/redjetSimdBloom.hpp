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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETSIMDBLOOM_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETSIMDBLOOM_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include <cmath>
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
// Redjet 512-bit Blocked SIMD Bloom Filter (PLAN.md Section 3.1, 4.2)
// - Cache-line aligned 64-byte blocks (Split-Block Bloom Filter)
// - Guarantees exactly 1 cache line access per insert/query (0 cross-line penalties)
// - Sub-cycle card table & TailTraces reference membership testing
// - 8 vectorized hash sub-lanes per 64-byte block
// - Batch query/insert with vector prefetching
// ============================================================================

struct alignas(64) RedjetBloomBlock512 {
  uint32_t words[16]; // 16 x 32-bit words = 512 bits = 64 bytes
};

class RedjetSimdBloom {
public:
  static constexpr size_t DEFAULT_NUM_BLOCKS = 16384; // 16K blocks = 1 MB footprint
  static constexpr size_t NUM_HASH_LANES     = 8;

private:
  RedjetBloomBlock512*  _blocks{ nullptr };
  size_t                _num_blocks{ 0 };
  size_t                _block_mask{ 0 };
  hs_ticket_lock_t      _bloom_lock;

  // Telemetry & metrics
  std::atomic<uint64_t> _total_items_inserted{ 0 };
  std::atomic<uint64_t> _total_queries{ 0 };
  std::atomic<uint64_t> _total_true_negatives{ 0 };
  std::atomic<uint64_t> _total_positives{ 0 };
  std::atomic<uint64_t> _batch_queries{ 0 };
  std::atomic<uint64_t> _simd_vector_tests{ 0 };

  static inline uint64_t hash_key(uint64_t key) noexcept {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
  }

public:
  explicit RedjetSimdBloom(size_t num_blocks = DEFAULT_NUM_BLOCKS);
  ~RedjetSimdBloom();

  void clear() noexcept;

  // Single-key insert and query
  RJ_API void insert(uint64_t key) noexcept;
  RJ_API bool may_contain(uint64_t key) noexcept;

  // Batch insert and query with software cache prefetch
  RJ_API void insert_batch(const uint64_t* keys, size_t count) noexcept;
  RJ_API void may_contain_batch(const uint64_t* keys, size_t count, uint8_t* out_results) noexcept;

  // Mathematical false positive probability model: P_fp = (1 - e^(-k*n/m))^k
  double estimated_false_positive_rate() const noexcept;

  // Telemetry accessors
  size_t   num_blocks()           const noexcept { return _num_blocks; }
  uint64_t total_items_inserted() const noexcept { return _total_items_inserted.load(std::memory_order_relaxed); }
  uint64_t total_queries()        const noexcept { return _total_queries.load(std::memory_order_relaxed); }
  uint64_t total_true_negatives() const noexcept { return _total_true_negatives.load(std::memory_order_relaxed); }
  uint64_t total_positives()      const noexcept { return _total_positives.load(std::memory_order_relaxed); }
  uint64_t batch_queries()        const noexcept { return _batch_queries.load(std::memory_order_relaxed); }
  uint64_t simd_vector_tests()    const noexcept { return _simd_vector_tests.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETSIMDBLOOM_HPP
