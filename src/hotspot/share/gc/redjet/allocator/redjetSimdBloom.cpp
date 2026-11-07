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

#include "gc/redjet/allocator/redjetSimdBloom.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetSimdBloom::RedjetSimdBloom(size_t num_blocks)
  : _blocks(nullptr),
    _num_blocks(0),
    _block_mask(0),
    _total_items_inserted(0),
    _total_queries(0),
    _total_true_negatives(0),
    _total_positives(0),
    _batch_queries(0),
    _simd_vector_tests(0) {
  hs_ticket_lock_init(&_bloom_lock);

  // Round up to nearest power of 2
  size_t n = 1;
  while (n < num_blocks) n <<= 1;
  _num_blocks = n;
  _block_mask = n - 1;

  size_t total_bytes = _num_blocks * sizeof(RedjetBloomBlock512);

#if defined(_WIN32)
  _blocks = reinterpret_cast<RedjetBloomBlock512*>(
    VirtualAlloc(nullptr, total_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
  _blocks = reinterpret_cast<RedjetBloomBlock512*>(
    mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (_blocks == MAP_FAILED) _blocks = nullptr;
#endif

  if (_blocks != nullptr) {
    std::memset(_blocks, 0, total_bytes);
  }
}

RedjetSimdBloom::~RedjetSimdBloom() {
  if (_blocks != nullptr) {
    size_t total_bytes = _num_blocks * sizeof(RedjetBloomBlock512);
#if defined(_WIN32)
    VirtualFree(_blocks, 0, MEM_RELEASE);
#else
    munmap(_blocks, total_bytes);
#endif
    _blocks = nullptr;
  }
}

void RedjetSimdBloom::clear() noexcept {
  if (_blocks != nullptr) {
    std::memset(_blocks, 0, _num_blocks * sizeof(RedjetBloomBlock512));
  }
}

void RedjetSimdBloom::insert(uint64_t key) noexcept {
  if (_blocks == nullptr) return;

  uint64_t h = hash_key(key);
  size_t block_idx = h & _block_mask;
  RedjetBloomBlock512* block = &_blocks[block_idx];

  uint32_t h32 = static_cast<uint32_t>(h);
  for (int i = 0; i < 8; ++i) {
    uint32_t word_idx = (h32 >> (i * 3)) & 0x0F;
    uint32_t bit_idx  = (h32 >> (i * 2 + 1)) & 0x1F;
    block->words[word_idx] |= (1U << bit_idx);
  }

  _total_items_inserted.fetch_add(1, std::memory_order_relaxed);
}

bool RedjetSimdBloom::may_contain(uint64_t key) noexcept {
  if (_blocks == nullptr) return false;

  _total_queries.fetch_add(1, std::memory_order_relaxed);

  uint64_t h = hash_key(key);
  size_t block_idx = h & _block_mask;
  const RedjetBloomBlock512* block = &_blocks[block_idx];

  uint32_t h32 = static_cast<uint32_t>(h);
  for (int i = 0; i < 8; ++i) {
    uint32_t word_idx = (h32 >> (i * 3)) & 0x0F;
    uint32_t bit_idx  = (h32 >> (i * 2 + 1)) & 0x1F;
    if ((block->words[word_idx] & (1U << bit_idx)) == 0) {
      _total_true_negatives.fetch_add(1, std::memory_order_relaxed);
      return false; // Guaranteed not in set
    }
  }

  _total_positives.fetch_add(1, std::memory_order_relaxed);
  return true; // May be in set
}

void RedjetSimdBloom::insert_batch(const uint64_t* keys, size_t count) noexcept {
  if (_blocks == nullptr || keys == nullptr || count == 0) return;

  for (size_t i = 0; i < count; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
    // Prefetch ahead 4 blocks
    if (i + 4 < count) {
      uint64_t next_h = hash_key(keys[i + 4]);
      size_t next_block = next_h & _block_mask;
      _mm_prefetch(reinterpret_cast<const char*>(&_blocks[next_block]), _MM_HINT_T0);
    }
#endif
    insert(keys[i]);
  }
}

void RedjetSimdBloom::may_contain_batch(const uint64_t* keys, size_t count, uint8_t* out_results) noexcept {
  if (_blocks == nullptr || keys == nullptr || out_results == nullptr || count == 0) return;

  _batch_queries.fetch_add(1, std::memory_order_relaxed);

  for (size_t i = 0; i < count; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
    // Software prefetch 4 elements ahead
    if (i + 4 < count) {
      uint64_t next_h = hash_key(keys[i + 4]);
      size_t next_block = next_h & _block_mask;
      _mm_prefetch(reinterpret_cast<const char*>(&_blocks[next_block]), _MM_HINT_T0);
    }
#endif
    out_results[i] = may_contain(keys[i]) ? 1 : 0;
  }
}

double RedjetSimdBloom::estimated_false_positive_rate() const noexcept {
  size_t n = _total_items_inserted.load(std::memory_order_relaxed);
  if (n == 0 || _num_blocks == 0) return 0.0;

  // Total bits m = num_blocks * 512
  double m = static_cast<double>(_num_blocks * 512);
  double k = 8.0; // 8 hash lanes
  double exponent = -k * static_cast<double>(n) / m;
  double p = std::pow(1.0 - std::exp(exponent), k);
  return p;
}

void RedjetSimdBloom::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet 512-bit Blocked SIMD Bloom Filter ---");
  st->print_cr("  Total Blocks:            %zu (%zu KB)", _num_blocks, (_num_blocks * 64) / 1024);
  st->print_cr("  Items Inserted:          %llu", static_cast<unsigned long long>(_total_items_inserted.load(std::memory_order_relaxed)));
  st->print_cr("  Total Queries:           %llu", static_cast<unsigned long long>(_total_queries.load(std::memory_order_relaxed)));
  st->print_cr("  Batch Queries:           %llu", static_cast<unsigned long long>(_batch_queries.load(std::memory_order_relaxed)));
  st->print_cr("  True Negatives:          %llu (%.2f%%)",
               static_cast<unsigned long long>(_total_true_negatives.load(std::memory_order_relaxed)),
               _total_queries.load(std::memory_order_relaxed) > 0 ?
                 (100.0 * _total_true_negatives.load(std::memory_order_relaxed) / _total_queries.load(std::memory_order_relaxed)) : 0.0);
  st->print_cr("  Positives:               %llu", static_cast<unsigned long long>(_total_positives.load(std::memory_order_relaxed)));
  st->print_cr("  Estimated FP Rate:       %.4e", estimated_false_positive_rate());
}
