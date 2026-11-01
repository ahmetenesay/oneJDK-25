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

#include "gc/redlet/redletMarkBitMap.hpp"
#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef REDLET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#endif

static inline int redlet_ctz64(uint64_t val) {
#if defined(_MSC_VER)
  unsigned long idx = 0;
  if (_BitScanForward64(&idx, val)) {
    return (int)idx;
  }
  return 64;
#else
  return val ? __builtin_ctzll(val) : 64;
#endif
}

static inline int redlet_popcount64(uint64_t val) {
#if defined(_MSC_VER)
  return (int)__popcnt64(val);
#else
  return __builtin_popcountll(val);
#endif
}

RedletMarkBitMap::RedletMarkBitMap() :
  _bottom(nullptr),
  _top(nullptr),
  _map(nullptr),
  _word_count(0),
  _allocated_externally(false) {}

RedletMarkBitMap::~RedletMarkBitMap() {
  if (_map != nullptr && !_allocated_externally) {
#ifndef REDLET_STANDALONE_BUILD
    os::free(_map);
#else
    free(_map);
#endif
    _map = nullptr;
  }
}

bool RedletMarkBitMap::initialize(HeapWord* bottom, HeapWord* top, bm_word_t* storage) {
  _bottom = bottom;
  _top = top;
  size_t heap_bytes = (uintptr_t)top - (uintptr_t)bottom;
  size_t bytes = compute_size_bytes(heap_bytes);
  _word_count = bytes / sizeof(bm_word_t);

  if (storage != nullptr) {
    _map = storage;
    _allocated_externally = true;
  } else {
#ifndef REDLET_STANDALONE_BUILD
    _map = reinterpret_cast<bm_word_t*>(os::malloc(bytes, mtGC));
#else
    _map = reinterpret_cast<bm_word_t*>(malloc(bytes));
#endif
    _allocated_externally = false;
    if (_map == nullptr) {
      return false;
    }
  }

  clear();
  return true;
}

void RedletMarkBitMap::clear() {
  if (_map != nullptr && _word_count > 0) {
    std::memset(_map, 0, _word_count * sizeof(bm_word_t));
  }
}

void RedletMarkBitMap::clear_range(HeapWord* start, HeapWord* end) {
  if (_map == nullptr || start >= end || start >= _top || end <= _bottom) return;
  start = std::max(start, _bottom);
  end   = std::min(end, _top);

  size_t start_bit = addr_to_bit(start);
  size_t end_bit   = addr_to_bit(end);

  size_t start_word = start_bit / BitsPerWord;
  size_t end_word   = end_bit / BitsPerWord;

  if (start_word == end_word) {
    bm_word_t mask = ((bm_word_t)1 << (end_bit - start_bit)) - 1;
    mask <<= (start_bit % BitsPerWord);
    _map[start_word] &= ~mask;
    return;
  }

  size_t first_part = start_bit % BitsPerWord;
  if (first_part != 0) {
    bm_word_t mask = ~(((bm_word_t)1 << first_part) - 1);
    _map[start_word] &= ~mask;
    start_word++;
  }

  if (start_word < end_word) {
    std::memset(&_map[start_word], 0, (end_word - start_word) * sizeof(bm_word_t));
  }

  size_t last_part = end_bit % BitsPerWord;
  if (last_part != 0 && end_word < _word_count) {
    bm_word_t mask = ((bm_word_t)1 << last_part) - 1;
    _map[end_word] &= ~mask;
  }
}

HeapWord* RedletMarkBitMap::get_next_marked_addr(const HeapWord* start, const HeapWord* end) const {
  if (_map == nullptr || start >= end || start >= _top) return const_cast<HeapWord*>(end);
  const HeapWord* cur_start = std::max(start, (const HeapWord*)_bottom);
  const HeapWord* cur_end   = std::min(end, (const HeapWord*)_top);

  size_t cur_bit = addr_to_bit(cur_start);
  size_t end_bit = addr_to_bit(cur_end);
  size_t word_idx = cur_bit / BitsPerWord;
  size_t end_word = (end_bit + BitsPerWord - 1) / BitsPerWord;

  if (word_idx >= _word_count) return const_cast<HeapWord*>(end);

  // Mask out bits before cur_bit in first word
  size_t bit_in_word = cur_bit % BitsPerWord;
  bm_word_t word = _map[word_idx] & (~((bm_word_t)0) << bit_in_word);

  while (true) {
    if (word != 0) {
      int trailing = redlet_ctz64((uint64_t)word);
      size_t found_bit = word_idx * BitsPerWord + trailing;
      if (found_bit >= end_bit) {
        return const_cast<HeapWord*>(end);
      }
      return bit_to_addr(found_bit);
    }
    word_idx++;
    if (word_idx >= end_word || word_idx >= _word_count) {
      break;
    }
    word = _map[word_idx];
  }

  return const_cast<HeapWord*>(end);
}

size_t RedletMarkBitMap::count_marked_in_range(const HeapWord* start, const HeapWord* end) const {
  if (_map == nullptr || start >= end || start >= _top) return 0;
  const HeapWord* cur_start = std::max(start, (const HeapWord*)_bottom);
  const HeapWord* cur_end   = std::min(end, (const HeapWord*)_top);

  size_t start_bit = addr_to_bit(cur_start);
  size_t end_bit   = addr_to_bit(cur_end);

  size_t start_word = start_bit / BitsPerWord;
  size_t end_word   = end_bit / BitsPerWord;

  size_t count = 0;
  if (start_word == end_word) {
    bm_word_t mask = ((bm_word_t)1 << (end_bit - start_bit)) - 1;
    mask <<= (start_bit % BitsPerWord);
    return redlet_popcount64(_map[start_word] & mask);
  }

  size_t first_part = start_bit % BitsPerWord;
  if (first_part != 0) {
    bm_word_t mask = (~((bm_word_t)0)) << first_part;
    count += redlet_popcount64(_map[start_word] & mask);
    start_word++;
  }

  for (size_t i = start_word; i < end_word && i < _word_count; ++i) {
    count += redlet_popcount64(_map[i]);
  }

  size_t last_part = end_bit % BitsPerWord;
  if (last_part != 0 && end_word < _word_count) {
    bm_word_t mask = ((bm_word_t)1 << last_part) - 1;
    count += redlet_popcount64(_map[end_word] & mask);
  }

  return count;
}
