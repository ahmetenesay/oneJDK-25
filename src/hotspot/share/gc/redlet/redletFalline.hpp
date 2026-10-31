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

#ifndef SHARE_GC_REDLET_REDLETFALLINE_HPP
#define SHARE_GC_REDLET_REDLETFALLINE_HPP

#include "gc/redlet/redletEmptyBox.hpp"
#include <atomic>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#ifdef REDLET_STANDALONE_BUILD
#include <cstdint>
#include <cstddef>
#include <thread>
struct outputStream;
#ifndef CHEAPOBJ_FALLBACK_DEFINED
#define CHEAPOBJ_FALLBACK_DEFINED
template <typename T> class CHeapObjDummy {};
#define CHeapObj CHeapObjDummy
#define mtGC int
#endif
#else
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#endif

static inline void sf_falline_spin_pause() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
  __yield();
#else
  __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
}

class RedletFalline : public CHeapObj<mtGC> {
public:
  static constexpr size_t NUM_EMPTYBOX_BINS = 8;
  static constexpr size_t EMPTYBOX_BIN_SIZES[NUM_EMPTYBOX_BINS] = {
    64,
    256,
    1024,
    4096,
    16384,
    65536,
    262144,
    SIZE_MAX
  };

  static inline int find_emptybox_bin(size_t sz) {
    for (int i = 0; i < (int)NUM_EMPTYBOX_BINS - 1; ++i) {
      if (sz <= EMPTYBOX_BIN_SIZES[i]) return i;
    }
    return (int)NUM_EMPTYBOX_BINS - 1;
  }

private:
  RedletEmptyBox*     _bin_heads[NUM_EMPTYBOX_BINS];
  size_t              _bin_counts[NUM_EMPTYBOX_BINS];
  std::atomic<bool>   _bin_locks[NUM_EMPTYBOX_BINS];
  std::atomic<size_t> _emptybox_count;
  std::atomic<bool>   _is_compacting;

  std::atomic<uint64_t> _snatch_hit_count;
  std::atomic<uint64_t> _snatch_miss_count;
  std::atomic<uint64_t> _split_count;
  std::atomic<uint64_t> _coalesced_merge_count;

  inline void lock_bin(int bin) {
    while (_bin_locks[bin].exchange(true, std::memory_order_acquire)) {
      sf_falline_spin_pause();
    }
  }

  inline void unlock_bin(int bin) {
    _bin_locks[bin].store(false, std::memory_order_release);
  }

public:
  RedletFalline();
  ~RedletFalline();

  void register_emptybox(size_t aligned_size, RedletEmptyBox* eb, uint64_t sweep_no = 0);
  RedletEmptyBox* snatch_emptybox(size_t needed_bytes, uint64_t current_sweep = 0);

  size_t emptybox_count() const { return _emptybox_count.load(std::memory_order_relaxed); }
  uint64_t snatch_hits()  const { return _snatch_hit_count.load(std::memory_order_relaxed); }
  uint64_t snatch_misses() const { return _snatch_miss_count.load(std::memory_order_relaxed); }
  uint64_t split_count()   const { return _split_count.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDLET_REDLETFALLINE_HPP
