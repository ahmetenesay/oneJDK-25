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

#ifndef SHARE_GC_REDJET_REDJETFALLINE_HPP
#define SHARE_GC_REDJET_REDJETFALLINE_HPP

#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <atomic>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#ifdef REDJET_STANDALONE_BUILD
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

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

static inline void hs_falline_spin_pause() noexcept {
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

/* ========================================================================= */
/* 1. MORGUE ENTRY (CONDEMNED OBJECT PRE-TRASHING STAGING)                   */
/* ========================================================================= */

struct alignas(64) RedjetMorgueNode {
  void*               obj_ptr{ nullptr };
  size_t              footprint_bytes{ 0 };
  uint32_t            function_id{ 0 };
  uint64_t            condemned_epoch{ 0 };
  uint64_t            condemned_timestamp_ns{ 0 };
  std::atomic<bool>   is_resurrected{ false };
  RedjetMorgueNode*   next{ nullptr };
  RedjetMorgueNode*   prev{ nullptr };
};

/* ========================================================================= */
/* 2. REDJET FALLINE NEXUS (EMPTYBOX BINS + MORGUE STAGING + RESURRECTION)   */
/* ========================================================================= */

class RedjetFalline : public CHeapObj<mtGC> {
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
  // EmptyBox Bins
  RedjetEmptyBox*     _bin_heads[NUM_EMPTYBOX_BINS];
  size_t              _bin_counts[NUM_EMPTYBOX_BINS];
  std::atomic<bool>   _bin_locks[NUM_EMPTYBOX_BINS];
  std::atomic<size_t> _emptybox_count;
  std::atomic<bool>   _is_compacting;

  // Snatch & Split Statistics
  std::atomic<uint64_t> _snatch_hit_count;
  std::atomic<uint64_t> _snatch_miss_count;
  std::atomic<uint64_t> _split_count;
  std::atomic<uint64_t> _coalesced_merge_count;

  // Morgue Subsystem
  RedjetMorgueNode*     _morgue_head{ nullptr };
  RedjetMorgueNode*     _morgue_tail{ nullptr };
  std::atomic<size_t>   _morgue_count{ 0 };
  std::atomic<size_t>   _morgue_bytes{ 0 };
  hs_ticket_lock_t      _morgue_lock;

  // Resurrection Subsystem
  std::atomic<uint64_t> _resurrection_requests{ 0 };
  std::atomic<uint64_t> _resurrections_succeeded{ 0 };
  std::atomic<uint64_t> _resurrections_rejected{ 0 };
  std::atomic<uint64_t> _morgue_expired_swept{ 0 };

  inline void lock_bin(int bin) {
    while (_bin_locks[bin].exchange(true, std::memory_order_acquire)) {
      hs_falline_spin_pause();
    }
  }

  inline void unlock_bin(int bin) {
    _bin_locks[bin].store(false, std::memory_order_release);
  }

public:
  RedjetFalline();
  ~RedjetFalline();

  // EmptyBox Management
  RJ_API void register_emptybox(size_t aligned_size, RedjetEmptyBox* eb, uint64_t sweep_no = 0);
  RJ_API RedjetEmptyBox* snatch_emptybox(size_t needed_bytes, uint64_t current_sweep = 0);

  // Morgue Lifecycle
  RJ_API bool stage_in_morgue(void* obj_ptr, size_t footprint, uint32_t function_id, uint64_t current_epoch);
  RJ_API bool is_in_morgue(const void* obj_ptr) const noexcept;

  // Epoch Delay & Sweeping: Evicts expired morgue entries past min_epoch_delay into EmptyBox bins
  RJ_API size_t advance_morgue_epoch(uint64_t current_epoch, uint64_t min_epoch_delay = 2);

  // Resurrection Check: Rescues an object staged in the morgue
  RJ_API bool attempt_resurrection(void* obj_ptr);

  // Statistics
  size_t emptybox_count() const { return _emptybox_count.load(std::memory_order_relaxed); }
  uint64_t snatch_hits()  const { return _snatch_hit_count.load(std::memory_order_relaxed); }
  uint64_t snatch_misses() const { return _snatch_miss_count.load(std::memory_order_relaxed); }
  uint64_t split_count()   const { return _split_count.load(std::memory_order_relaxed); }

  size_t morgue_count() const { return _morgue_count.load(std::memory_order_relaxed); }
  size_t morgue_bytes() const { return _morgue_bytes.load(std::memory_order_relaxed); }
  uint64_t resurrections() const { return _resurrections_succeeded.load(std::memory_order_relaxed); }
  uint64_t resurrection_rejects() const { return _resurrections_rejected.load(std::memory_order_relaxed); }
  uint64_t expired_swept() const { return _morgue_expired_swept.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_REDJETFALLINE_HPP
