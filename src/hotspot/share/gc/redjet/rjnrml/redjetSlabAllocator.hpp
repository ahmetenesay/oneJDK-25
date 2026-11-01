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

#ifndef SHARE_GC_REDJET_REDJETSLABALLOCATOR_HPP
#define SHARE_GC_REDJET_REDJETSLABALLOCATOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjnrml/redjetEmptyBox.hpp"
#include "gc/redjet/rjnrml/redjetPagefile.hpp"
#include "gc/redjet/rjspcf/redjetSIMD.hpp"
#include "gc/redjet/rjspcf/redjetFalline.hpp"
#include "gc/redjet/rjnrml/redjetIntentCLI.hpp"
#include "gc/redjet/rjspcf/redjetMotherline.hpp"
#include "gc/redjet/rjspcf/redjetRingLog.hpp"

#ifdef REDJET_STANDALONE_BUILD
class HeapWordImpl;
typedef HeapWordImpl* HeapWord;
static constexpr size_t HeapWordSize = sizeof(HeapWord);
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#endif

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

/* ========================================================================= */
/* 1. SLAB FREELIST NODE & 7-TIER SIZE CLASSES                               */
/* ========================================================================= */

struct RedjetSlabNode {
  RedjetSlabNode* next{ nullptr };
};

class RedjetSlabClass {
public:
  enum RecyclePolicy : uint32_t {
    LIFO = 0,
    FIFO = 1
  };

private:
  size_t              _item_size{ 0 };
  RedjetSlabNode*     _free_list_head{ nullptr };
  RedjetSlabNode*     _free_list_tail{ nullptr };
  std::atomic<size_t> _allocated_count{ 0 };
  std::atomic<size_t> _free_count{ 0 };
  hs_ticket_lock_t    _lock;

public:
  RedjetSlabClass() = default;

  void init(size_t sz) noexcept {
    _item_size = sz;
    _free_list_head = nullptr;
    _free_list_tail = nullptr;
    _allocated_count.store(0, std::memory_order_relaxed);
    _free_count.store(0, std::memory_order_relaxed);
  }

  size_t item_size() const noexcept { return _item_size; }
  size_t allocated_count() const noexcept { return _allocated_count.load(std::memory_order_relaxed); }
  size_t free_count() const noexcept { return _free_count.load(std::memory_order_relaxed); }

  void* allocate_from_freelist();
  void  deallocate(void* ptr, uint32_t policy = LIFO);
};

/* ========================================================================= */
/* 2. THREAD-LOCAL ALLOCATION BUFFER (TLAB) CONTEXT                          */
/* ========================================================================= */

struct RedjetTLABBuffer {
  uint8_t* owner_runway_base{ nullptr };
  uint32_t owner_epoch{ 0 };
  uint8_t* base{ nullptr };
  size_t   offset{ 0 };
  size_t   capacity{ 0 };

  void reset() noexcept {
    owner_runway_base = nullptr;
    owner_epoch = 0;
    base = nullptr;
    offset = 0;
    capacity = 0;
  }

  inline uint8_t* allocate(size_t alloc_sz, size_t align = SACFOR_CACHE_LINE) noexcept {
    size_t t_off = (offset + align - 1) & ~(align - 1);
    if (t_off + alloc_sz <= capacity) {
      offset = t_off + alloc_sz;
      return base + t_off;
    }
    return nullptr;
  }

  inline size_t remaining() const noexcept {
    return (capacity > offset) ? (capacity - offset) : 0;
  }

  static RedjetTLABBuffer* current();
};

/* ========================================================================= */
/* 3. CHAINED OS EXPANSION CHUNK                                             */
/* ========================================================================= */

struct RedjetChainedChunk {
  uint8_t*            base{ nullptr };
  size_t              capacity{ 0 };
  std::atomic<size_t> bump_offset{ 0 };
  RedjetChainedChunk* next{ nullptr };
};

/* ========================================================================= */
/* 4. REDJET SLAB ALLOCATOR NEXUS                                            */
/* ========================================================================= */

class RedjetSlabAllocator {
public:
  static constexpr size_t NUM_CLASSES = 7;
  static constexpr size_t CLASS_SIZES[NUM_CLASSES] = {
    64, 256, 1024, 4096, 16384, 65536, 262144
  };
  static constexpr size_t TLAB_REFILL_BYTES = 512 * 1024;
  static constexpr size_t TLAB_MAX_OBJ_SIZE = 65536;
  static constexpr size_t MAX_SLAB_SIZE = 262144;

private:
  RedjetSlabClass       _classes[NUM_CLASSES];
  size_t                _slab_chunk_size_bytes{ 512 * 1024 };
  uint32_t              _freelist_recycle_policy{ 0 }; // 0: LIFO, 1: FIFO
  uint8_t*              _runway_heap_base{ nullptr };
  size_t                _runway_capacity{ 0 };
  std::atomic<size_t>   _runway_bump_offset{ 0 };
  hs_ticket_lock_t      _bump_lock;
  hs_heap_superblock_t* _superblock{ nullptr };
  bool                  _advance_payment{ false };
  bool                  _forbid_dynamic_expansion{ false };
  std::atomic<uint32_t> _cache_color_step{ 0 };
  RedjetFalline*        _falline{ nullptr };

  std::mutex            _chain_lock;
  RedjetChainedChunk*   _chained_head{ nullptr };
  std::atomic<size_t>   _chained_chunk_count{ 0 };
  std::atomic<uint32_t> _runway_epoch{ 1 };

public:
  RedjetSlabAllocator();
  ~RedjetSlabAllocator();

  bool init(size_t handsel_bytes = 1024ULL * 1024 * 1024,
            bool advance_payment = false,
            bool forbid_expansion = false,
            uint32_t freelist_policy = 0,
            size_t slab_chunk_sz = 512 * 1024);
  void destroy();

  void set_falline(RedjetFalline* falline) noexcept { _falline = falline; }
  RedjetFalline* falline() const noexcept { return _falline; }

  int find_size_class(size_t bytes) const noexcept {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
      if (bytes <= CLASS_SIZES[i]) return static_cast<int>(i);
    }
    return -1;
  }

  void* allocate(size_t bytes, size_t align = SACFOR_CACHE_LINE);
  void* allocate_from_chained_chunks(size_t alloc_sz, size_t align = SACFOR_CACHE_LINE);
  void  deallocate(void* ptr, size_t bytes);

  HeapWord* refill_tlab(RedjetTLABBuffer* tlab, size_t min_words, size_t requested_words, size_t* actual_words);
  HeapWord* refill_tlab(size_t min_words, size_t requested_words, size_t* actual_words) {
    return refill_tlab(nullptr, min_words, requested_words, actual_words);
  }

  bool is_in(const void* p) const noexcept;
  size_t rebate_pages(hs_snatch_rebate_mode_e mode, size_t threshold_bytes = 64 * 1024);
  size_t get_used_bytes() const noexcept;
  size_t capacity() const noexcept;
  uint8_t* runway_base() const noexcept { return _runway_heap_base; }
  uint32_t runway_epoch() const noexcept { return _runway_epoch.load(std::memory_order_relaxed); }

  const RedjetSlabClass& slab_class(size_t idx) const { return _classes[idx]; }
  RedjetSlabClass& slab_class(size_t idx) { return _classes[idx]; }
};

#endif // SHARE_GC_REDJET_REDJETSLABALLOCATOR_HPP
