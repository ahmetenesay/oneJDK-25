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

#ifndef SHARE_GC_REDJET_REDJETMARKBITMAP_HPP
#define SHARE_GC_REDJET_REDJETMARKBITMAP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "memory/memRegion.hpp"
#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"
#else
typedef void* HeapWord;
typedef void* oop;
class MemRegion {
  HeapWord* _start;
  HeapWord* _end;
public:
  MemRegion() : _start(nullptr), _end(nullptr) {}
  MemRegion(HeapWord* s, HeapWord* e) : _start(s), _end(e) {}
  HeapWord* start() const { return _start; }
  HeapWord* end() const { return _end; }
  size_t byte_size() const { return (char*)_end - (char*)_start; }
  bool contains(const void* p) const { return p >= _start && p < _end; }
};
#endif

// RedjetMarkBitMap: SIMD-accelerated, lock-free parallel mark bitmap for RedjetGC
class RedjetMarkBitMap {
public:
  typedef uintptr_t bm_word_t;
  static constexpr size_t BitsPerWord = sizeof(bm_word_t) * 8;
  static constexpr size_t Shift = 3; // 8 bytes per bit (MinObjAlignment)

private:
  HeapWord*   _bottom;
  HeapWord*   _top;
  bm_word_t*  _map;
  size_t      _word_count;
  bool        _allocated_externally;

  inline size_t addr_to_bit(const HeapWord* addr) const {
    return ((uintptr_t)addr - (uintptr_t)_bottom) >> Shift;
  }

  inline HeapWord* bit_to_addr(size_t bit) const {
    return (HeapWord*)((uintptr_t)_bottom + (bit << Shift));
  }

public:
  RedjetMarkBitMap();
  ~RedjetMarkBitMap();

  static size_t compute_size_bytes(size_t heap_bytes) {
    size_t bits = heap_bytes >> Shift;
    size_t words = (bits + BitsPerWord - 1) / BitsPerWord;
    return words * sizeof(bm_word_t);
  }

  bool initialize(HeapWord* bottom, HeapWord* top, bm_word_t* storage = nullptr);

  inline bool is_marked(const HeapWord* addr) const {
    if (addr < _bottom || addr >= _top || _map == nullptr) return false;
    size_t bit = addr_to_bit(addr);
    size_t word_idx = bit / BitsPerWord;
    bm_word_t mask = (bm_word_t)1 << (bit % BitsPerWord);
    return (reinterpret_cast<const std::atomic<bm_word_t>*>(&_map[word_idx])->load(std::memory_order_relaxed) & mask) != 0;
  }

  inline bool is_marked(oop obj) const {
    return is_marked(reinterpret_cast<const HeapWord*>(obj));
  }

  // Atomic mark. Returns true if bit was successfully set (transitioned 0 -> 1).
  inline bool par_mark(const HeapWord* addr) {
    if (addr < _bottom || addr >= _top || _map == nullptr) return false;
    size_t bit = addr_to_bit(addr);
    size_t word_idx = bit / BitsPerWord;
    bm_word_t mask = (bm_word_t)1 << (bit % BitsPerWord);

    std::atomic<bm_word_t>* target = reinterpret_cast<std::atomic<bm_word_t>*>(&_map[word_idx]);
    bm_word_t old_val = target->load(std::memory_order_relaxed);
    while ((old_val & mask) == 0) {
      if (target->compare_exchange_weak(old_val, old_val | mask, std::memory_order_relaxed, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  inline bool par_mark(oop obj) {
    return par_mark(reinterpret_cast<const HeapWord*>(obj));
  }

  void clear();
  void clear_range(HeapWord* start, HeapWord* end);

  // SIMD/Hardware accelerated scan routines
  HeapWord* get_next_marked_addr(const HeapWord* start, const HeapWord* end) const;
  size_t count_marked_in_range(const HeapWord* start, const HeapWord* end) const;

  HeapWord* bottom() const { return _bottom; }
  HeapWord* top() const { return _top; }
  size_t word_count() const { return _word_count; }
  bm_word_t* raw_words() { return _map; }
};

#endif // SHARE_GC_REDJET_REDJETMARKBITMAP_HPP
