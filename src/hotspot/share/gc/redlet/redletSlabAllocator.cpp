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

#include "gc/redlet/redletSlabAllocator.hpp"
#include <new>

thread_local RedletTLABBuffer tl_redlet_tlab{};

RedletTLABBuffer* RedletTLABBuffer::current() {
  return &tl_redlet_tlab;
}

/* ========================================================================= */
/* 1. SLAB CLASS FREELIST ALLOCATION & RECYCLING                             */
/* ========================================================================= */

void* RedletSlabClass::allocate_from_freelist() {
  sf_ticket_guard_t g(_lock);
  if (!_free_list_head) return nullptr;

  RedletSlabNode* node = _free_list_head;
  _free_list_head = node->next;
  if (!_free_list_head) {
    _free_list_tail = nullptr;
  }
  _free_count.fetch_sub(1, std::memory_order_relaxed);
  _allocated_count.fetch_add(1, std::memory_order_relaxed);
  return static_cast<void*>(node);
}

void RedletSlabClass::deallocate(void* ptr, uint32_t policy) {
  if (!ptr) return;
  sf_ticket_guard_t g(_lock);
  auto* node = static_cast<RedletSlabNode*>(ptr);
  node->next = nullptr;

  if (policy == FIFO) {
    if (!_free_list_head) {
      _free_list_head = node;
      _free_list_tail = node;
    } else {
      _free_list_tail->next = node;
      _free_list_tail = node;
    }
  } else { // LIFO (Default)
    node->next = _free_list_head;
    _free_list_head = node;
    if (!_free_list_tail) {
      _free_list_tail = node;
    }
  }
  _free_count.fetch_add(1, std::memory_order_relaxed);
  size_t cur_alloc = _allocated_count.load(std::memory_order_relaxed);
  if (cur_alloc > 0) {
    _allocated_count.fetch_sub(1, std::memory_order_relaxed);
  }
}

/* ========================================================================= */
/* 2. SLAB ALLOCATOR INITIALIZATION & LIFECYCLE                              */
/* ========================================================================= */

RedletSlabAllocator::RedletSlabAllocator() {
  for (size_t i = 0; i < NUM_CLASSES; ++i) {
    _classes[i].init(CLASS_SIZES[i]);
  }
}

RedletSlabAllocator::~RedletSlabAllocator() {
  destroy();
}

bool RedletSlabAllocator::init(size_t handsel_bytes,
                               bool advance_payment,
                               bool forbid_expansion,
                               uint32_t freelist_policy,
                               size_t slab_chunk_sz) {
  _runway_capacity = handsel_bytes;
  _advance_payment = advance_payment;
  _forbid_dynamic_expansion = forbid_expansion;
  _freelist_recycle_policy = freelist_policy;
  _slab_chunk_size_bytes = (slab_chunk_sz >= 64 * 1024) ? slab_chunk_sz : (512 * 1024);

  for (size_t i = 0; i < NUM_CLASSES; ++i) {
    _classes[i].init(CLASS_SIZES[i]);
  }

  while (_runway_capacity >= 64ULL * 1024 * 1024) {
    _runway_heap_base = static_cast<uint8_t*>(sf_os_reserve_memory(_runway_capacity));
    if (_runway_heap_base) {
      if (sf_os_commit_memory(_runway_heap_base, _runway_capacity)) {
        break;
      }
      sf_os_release_memory(_runway_heap_base, _runway_capacity);
      _runway_heap_base = nullptr;
    }
    _runway_capacity /= 2;
  }
  if (!_runway_heap_base) return false;

  _superblock = reinterpret_cast<sf_heap_superblock_t*>(_runway_heap_base);
  new (_superblock) sf_heap_superblock_t();
  _superblock->magic = SF_HEAP_MAGIC;
  _superblock->version = 1;
  _superblock->total_reserved_bytes = _runway_capacity;
  _superblock->total_committed_bytes = _runway_capacity;
  _superblock->page_size = 4096;
  _superblock->alignment = SACFOR_CACHE_LINE;

  size_t header_sz = sizeof(sf_heap_superblock_t);
  header_sz = (header_sz + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);

  _superblock->slabs_offset = header_sz;
  _superblock->slabs_capacity = _runway_capacity / 4;
  _superblock->bump_arena_offset = header_sz;
  _superblock->bump_arena_capacity = _runway_capacity * 3 / 4;
  _superblock->los_offset = _runway_capacity * 3 / 4;
  _superblock->los_capacity = _runway_capacity / 4;

  _runway_bump_offset.store(header_sz, std::memory_order_relaxed);
  _runway_epoch.fetch_add(1, std::memory_order_release);
  RedletTLABBuffer::current()->reset();

  if (_advance_payment) {
    sf_pretouch_memory(_runway_heap_base, _runway_capacity);
  }
  return true;
}

void RedletSlabAllocator::destroy() {
  _runway_epoch.fetch_add(1, std::memory_order_release);
  RedletTLABBuffer::current()->reset();

  {
    std::lock_guard<std::mutex> g(_chain_lock);
    RedletChainedChunk* ch = _chained_head;
    while (ch) {
      RedletChainedChunk* nxt = ch->next;
      if (ch->base) {
        sf_os_release_memory(ch->base, ch->capacity);
      }
      delete ch;
      ch = nxt;
    }
    _chained_head = nullptr;
    _chained_chunk_count.store(0, std::memory_order_relaxed);
  }

  if (_runway_heap_base) {
    sf_os_release_memory(_runway_heap_base, _runway_capacity);
    _runway_heap_base = nullptr;
    _superblock = nullptr;
  }
  _runway_capacity = 0;
  _runway_bump_offset.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 3. CHAINED OS CHUNK ALLOCATION                                            */
/* ========================================================================= */

void* RedletSlabAllocator::allocate_from_chained_chunks(size_t alloc_sz, size_t align) {
  if (_forbid_dynamic_expansion) {
    return nullptr;
  }

  std::lock_guard<std::mutex> g(_chain_lock);
  RedletChainedChunk* cur = _chained_head;
  while (cur) {
    size_t off = cur->bump_offset.load(std::memory_order_relaxed);
    size_t aligned_off = (off + align - 1) & ~(align - 1);
    if (aligned_off + alloc_sz <= cur->capacity) {
      cur->bump_offset.store(aligned_off + alloc_sz, std::memory_order_relaxed);
      if (_superblock) {
        _superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
      }
      return cur->base + aligned_off;
    }
    cur = cur->next;
  }

  size_t chunk_cap = alloc_sz * 2;
  size_t min_cap = (_slab_chunk_size_bytes >= 64 * 1024) ? _slab_chunk_size_bytes : (64ULL * 1024 * 1024);
  if (chunk_cap < min_cap) chunk_cap = min_cap;
  chunk_cap = (chunk_cap + 4095ULL) & ~4095ULL;

  uint8_t* new_base = static_cast<uint8_t*>(sf_os_reserve_memory(chunk_cap));
  if (!new_base) return nullptr;
  if (!sf_os_commit_memory(new_base, chunk_cap)) {
    sf_os_release_memory(new_base, chunk_cap);
    return nullptr;
  }
  if (_advance_payment) {
    sf_pretouch_memory(new_base, chunk_cap);
  }

  auto* chunk = new (std::nothrow) RedletChainedChunk();
  if (!chunk) {
    sf_os_release_memory(new_base, chunk_cap);
    return nullptr;
  }
  chunk->base = new_base;
  chunk->capacity = chunk_cap;
  chunk->bump_offset.store(alloc_sz, std::memory_order_relaxed);
  chunk->next = _chained_head;
  _chained_head = chunk;
  _chained_chunk_count.fetch_add(1, std::memory_order_relaxed);

  if (_superblock) {
    _superblock->total_reserved_bytes += chunk_cap;
    _superblock->total_committed_bytes += chunk_cap;
    _superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
    _superblock->next_chained_block = new_base;
  }

  sf_hotlog_emit(SF_Ebin0_ALLOC_CHUNK_CHAIN, SF_Ebin1_INFO, 0,
                 static_cast<uint16_t>(_chained_chunk_count.load(std::memory_order_relaxed)),
                 0, 0, 0, 0, static_cast<uint32_t>(chunk_cap / 1024), "os_chunk_chained");
  return new_base;
}

/* ========================================================================= */
/* 4. TLAB REFILL & SIMD ZEROING                                             */
/* ========================================================================= */

HeapWord* RedletSlabAllocator::refill_tlab(RedletTLABBuffer* tlab,
                                          size_t min_words,
                                          size_t requested_words,
                                          size_t* actual_words) {
  if (requested_words == 0) {
    if (actual_words != nullptr) *actual_words = 0;
    return nullptr;
  }

  size_t alloc_bytes = requested_words * HeapWordSize;
  size_t min_bytes = (min_words > 0) ? (min_words * HeapWordSize) : alloc_bytes;
  uint32_t cur_epoch = _runway_epoch.load(std::memory_order_acquire);

  // Falline Integration: Register genuine leftover from previous TLAB slice ONLY IF
  // an existing thread-local buffer with active allocations (offset > 0) is being refilled.
  if (tlab != nullptr && _falline != nullptr &&
      tlab->owner_runway_base == _runway_heap_base &&
      tlab->owner_epoch == cur_epoch &&
      tlab->base != nullptr &&
      tlab->offset > 0) {
    size_t rem_offset = (tlab->offset + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    if (rem_offset < tlab->capacity) {
      size_t leftover = tlab->capacity - rem_offset;
      if (leftover >= sizeof(RedletEmptyBox)) {
        uint8_t* rem_ptr = tlab->base + rem_offset;
        auto* eb = reinterpret_cast<RedletEmptyBox*>(rem_ptr);
        _falline->register_emptybox(leftover, eb);
      }
    }
  }

  uint8_t* new_slice = nullptr;
  size_t granted_bytes = alloc_bytes;

  // Primary: Runway bump allocation under ticket lock with cache coloring
  if (_runway_heap_base != nullptr) {
    sf_ticket_guard_t guard(_bump_lock);
    size_t prev_end = _runway_bump_offset.load(std::memory_order_relaxed);
    size_t color_pad = (_cache_color_step.fetch_add(1, std::memory_order_relaxed) & 7) * SACFOR_CACHE_LINE;
    size_t cur = prev_end + color_pad;
    size_t aligned_cur = (cur + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);

    if (aligned_cur + alloc_bytes <= _runway_capacity) {
      granted_bytes = alloc_bytes;
      size_t gap = aligned_cur - prev_end;
      if (gap >= sizeof(RedletEmptyBox) && _falline != nullptr) {
        sf_os_commit_memory(_runway_heap_base + prev_end, gap);
        auto* eb = reinterpret_cast<RedletEmptyBox*>(_runway_heap_base + prev_end);
        _falline->register_emptybox(gap, eb);
      }
      sf_os_commit_memory(_runway_heap_base + aligned_cur, granted_bytes);
      _runway_bump_offset.store(aligned_cur + granted_bytes, std::memory_order_relaxed);
      if (_superblock != nullptr) {
        _superblock->allocated_bytes.fetch_add(granted_bytes, std::memory_order_relaxed);
      }
      new_slice = _runway_heap_base + aligned_cur;
    } else if (aligned_cur + min_bytes <= _runway_capacity) {
      // Elastic clamping: grant available remaining space in runway
      size_t available = _runway_capacity - aligned_cur;
      granted_bytes = (available / HeapWordSize) * HeapWordSize;
      size_t gap = aligned_cur - prev_end;
      if (gap >= sizeof(RedletEmptyBox) && _falline != nullptr) {
        sf_os_commit_memory(_runway_heap_base + prev_end, gap);
        auto* eb = reinterpret_cast<RedletEmptyBox*>(_runway_heap_base + prev_end);
        _falline->register_emptybox(gap, eb);
      }
      sf_os_commit_memory(_runway_heap_base + aligned_cur, granted_bytes);
      _runway_bump_offset.store(aligned_cur + granted_bytes, std::memory_order_relaxed);
      if (_superblock != nullptr) {
        _superblock->allocated_bytes.fetch_add(granted_bytes, std::memory_order_relaxed);
      }
      new_slice = _runway_heap_base + aligned_cur;
    }
  }

  // Secondary: Chained chunks if initial runway exhausted and dynamic expansion allowed
  if (new_slice == nullptr && !_forbid_dynamic_expansion) {
    new_slice = static_cast<uint8_t*>(allocate_from_chained_chunks(alloc_bytes, SACFOR_CACHE_LINE));
    if (new_slice != nullptr) {
      granted_bytes = alloc_bytes;
    } else if (min_bytes < alloc_bytes) {
      new_slice = static_cast<uint8_t*>(allocate_from_chained_chunks(min_bytes, SACFOR_CACHE_LINE));
      if (new_slice != nullptr) {
        granted_bytes = min_bytes;
      }
    }
  }

  if (new_slice == nullptr) {
    if (actual_words != nullptr) *actual_words = 0;
    return nullptr;
  }

  // SIMD-accelerated zeroing on TLAB refill
  sf_critical_clear_memory(new_slice, granted_bytes);

  // Update thread-local buffer tracking if caller provided an internal TLAB context
  if (tlab != nullptr) {
    tlab->owner_runway_base = _runway_heap_base;
    tlab->owner_epoch = cur_epoch;
    tlab->base = new_slice;
    tlab->capacity = granted_bytes;
    tlab->offset = 0;
  }

  if (actual_words != nullptr) {
    *actual_words = granted_bytes / HeapWordSize;
  }

  return reinterpret_cast<HeapWord*>(new_slice);
}

/* ========================================================================= */
/* 5. FAST-PATH & GENERAL ALLOCATION                                         */
/* ========================================================================= */

void* RedletSlabAllocator::allocate(size_t bytes, size_t align) {
  if (bytes == 0) return nullptr;

  // A. Freelist allocation for discrete size classes
  int sc = find_size_class(bytes);
  if (sc >= 0) {
    void* recycled = _classes[sc].allocate_from_freelist();
    if (recycled != nullptr) {
      if (_superblock != nullptr) {
        _superblock->allocated_bytes.fetch_add(CLASS_SIZES[sc], std::memory_order_relaxed);
      }
      sf_critical_clear_memory(recycled, CLASS_SIZES[sc]);
      sf_hotlog_emit(SF_Ebin0_ALLOC_FREELIST, SF_Ebin1_TRACE, 0, static_cast<uint16_t>(sc),
                     0, 0, 0, 0, static_cast<uint32_t>(CLASS_SIZES[sc] / 1024), "slab_freelist_hit");
      return recycled;
    }
    bytes = CLASS_SIZES[sc];
  }

  size_t alloc_sz = (bytes + align - 1) & ~(align - 1);

  // B. Fast Lock-Free Thread-Local Allocation Buffer (TLAB) bump path
  uint32_t cur_epoch = _runway_epoch.load(std::memory_order_acquire);
  RedletTLABBuffer* tlab = RedletTLABBuffer::current();
  if (!_forbid_dynamic_expansion && alloc_sz <= TLAB_MAX_OBJ_SIZE && _runway_heap_base != nullptr) {
    if (tlab->owner_runway_base == _runway_heap_base &&
        tlab->owner_epoch == cur_epoch &&
        tlab->base != nullptr) {
      uint8_t* ptr = tlab->allocate(alloc_sz, align);
      if (ptr != nullptr) {
        if (_superblock != nullptr) {
          _superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
        }
        return ptr;
      }
    }

    // Refill TLAB slice from bump arena
    size_t refill_sz = (_slab_chunk_size_bytes >= 64 * 1024) ? _slab_chunk_size_bytes : TLAB_REFILL_BYTES;
    if (alloc_sz > refill_sz) {
      refill_sz = alloc_sz;
    }
    size_t req_words = (refill_sz + HeapWordSize - 1) / HeapWordSize;
    size_t min_words = (alloc_sz + HeapWordSize - 1) / HeapWordSize;
    size_t actual_words = 0;
    HeapWord* slice = refill_tlab(tlab, min_words, req_words, &actual_words);
    if (slice != nullptr) {
      uint8_t* ptr = tlab->allocate(alloc_sz, align);
      if (ptr != nullptr) {
        if (_superblock != nullptr) {
          _superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
        }
        return ptr;
      }
    }
  }

  // C. Global bump allocation under bump_lock with cache-coloring offset
  if (_runway_heap_base != nullptr) {
    sf_ticket_guard_t guard(_bump_lock);
    size_t prev_end = _runway_bump_offset.load(std::memory_order_relaxed);
    size_t color_pad = (_cache_color_step.fetch_add(1, std::memory_order_relaxed) & 7) * SACFOR_CACHE_LINE;
    size_t cur = prev_end + color_pad;
    size_t aligned_cur = (cur + align - 1) & ~(align - 1);

    if (aligned_cur + alloc_sz <= _runway_capacity) {
      size_t gap = aligned_cur - prev_end;
      if (gap >= sizeof(RedletEmptyBox) && _falline != nullptr) {
        sf_os_commit_memory(_runway_heap_base + prev_end, gap);
        auto* eb = reinterpret_cast<RedletEmptyBox*>(_runway_heap_base + prev_end);
        _falline->register_emptybox(gap, eb);
      }
      sf_os_commit_memory(_runway_heap_base + aligned_cur, alloc_sz);
      _runway_bump_offset.store(aligned_cur + alloc_sz, std::memory_order_relaxed);
      if (_superblock != nullptr) {
        _superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
      }
      uint8_t* res = _runway_heap_base + aligned_cur;
      sf_critical_clear_memory(res, alloc_sz);
      return res;
    }
  }

  if (_forbid_dynamic_expansion) {
    return nullptr;
  }

  void* p = allocate_from_chained_chunks(alloc_sz, align);
  if (p != nullptr) {
    sf_critical_clear_memory(p, alloc_sz);
  }
  return p;
}

void RedletSlabAllocator::deallocate(void* ptr, size_t bytes) {
  if (!ptr || !_runway_heap_base) return;

  int sc = find_size_class(bytes);
  if (sc >= 0) {
    _classes[sc].deallocate(ptr, _freelist_recycle_policy);
    if (_superblock) {
      _superblock->allocated_bytes.fetch_sub(CLASS_SIZES[sc], std::memory_order_relaxed);
    }
  } else {
    if (_superblock) {
      _superblock->allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
  }
}

bool RedletSlabAllocator::is_in(const void* p) const noexcept {
  if (p == nullptr) return false;
  const uint8_t* ptr = static_cast<const uint8_t*>(p);
  if (_runway_heap_base != nullptr &&
      ptr >= _runway_heap_base &&
      ptr < _runway_heap_base + _runway_capacity) {
    return true;
  }
  RedletChainedChunk* cur = _chained_head;
  while (cur != nullptr) {
    if (cur->base != nullptr &&
        ptr >= cur->base &&
        ptr < cur->base + cur->capacity) {
      return true;
    }
    cur = cur->next;
  }
  return false;
}

size_t RedletSlabAllocator::rebate_pages(sf_snatch_rebate_mode_e mode, size_t threshold_bytes) {
  if (_forbid_dynamic_expansion || mode == sf_snatch_rebate_mode_e::LAZY || !_runway_heap_base) {
    return 0;
  }

  sf_ticket_guard_t guard(_bump_lock);
  size_t cur_used = _runway_bump_offset.load(std::memory_order_relaxed);
  size_t page_start = (cur_used + 4095ULL) & ~4095ULL;
  if (_runway_capacity > page_start) {
    size_t unused = _runway_capacity - page_start;
    if (unused >= threshold_bytes) {
      size_t page_aligned = unused & ~4095ULL;
      if (page_aligned > 0) {
        uint8_t* target = _runway_heap_base + page_start;
        sf_os_decommit_memory(target, page_aligned);
        if (_superblock) {
          _superblock->rebated_bytes.fetch_add(page_aligned, std::memory_order_relaxed);
          _superblock->rebate_cycle_count.fetch_add(1, std::memory_order_relaxed);
        }
        sf_hotlog_emit(SF_Ebin0_ALLOC_REBATE, SF_Ebin1_INFO, 0,
                       static_cast<uint16_t>(mode), 0, 0, 0, 0,
                       static_cast<uint32_t>(page_aligned / 1024), "runway_pages_rebated");
        return page_aligned;
      }
    }
  }
  return 0;
}

size_t RedletSlabAllocator::get_used_bytes() const noexcept {
  size_t off = _runway_bump_offset.load(std::memory_order_relaxed);
  size_t hdr = sizeof(sf_heap_superblock_t);
  hdr = (hdr + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
  size_t used = (off > hdr) ? (off - hdr) : 0;
  RedletChainedChunk* cur = _chained_head;
  while (cur != nullptr) {
    used += cur->bump_offset.load(std::memory_order_relaxed);
    cur = cur->next;
  }
  return used;
}

size_t RedletSlabAllocator::capacity() const noexcept {
  size_t cap = _runway_capacity;
  RedletChainedChunk* cur = _chained_head;
  while (cur != nullptr) {
    cap += cur->capacity;
    cur = cur->next;
  }
  return cap;
}
