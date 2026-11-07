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

#include "gc/redjet/vfs/redjetVFSNode_RunwayEgg.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <new>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

RedjetVFSNode_RunwayEgg::RedjetVFSNode_RunwayEgg(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  for (size_t i = 0; i < MAX_TRACKED_SLABS; ++i) {
    _slabs[i] = nullptr;
  }
  for (size_t i = 0; i < MAX_CACHED_THREADS; ++i) {
    _thread_tlabs[i].current_slab = nullptr;
    _thread_tlabs[i].thread_bump_ptr = nullptr;
    _thread_tlabs[i].thread_end_ptr = nullptr;
    _thread_tlabs[i].thread_allocated_bytes = 0;
  }
}

RedjetVFSNode_RunwayEgg::~RedjetVFSNode_RunwayEgg() {
  hs_ticket_guard_t guard(_slab_lock);
  size_t count = _slab_count.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    if (_slabs[i] != nullptr) {
      if (_slabs[i]->slab_base != nullptr) {
#if defined(_WIN32)
        VirtualFree(_slabs[i]->slab_base, 0, MEM_RELEASE);
#else
        munmap(_slabs[i]->slab_base, RedjetEggSlabHeader::SLAB_CAPACITY_BYTES);
#endif
      }
      delete _slabs[i];
      _slabs[i] = nullptr;
    }
  }
  _slab_count.store(0, std::memory_order_relaxed);
}

RedjetEggSlabHeader* RedjetVFSNode_RunwayEgg::allocate_new_slab(
    RedjetEggZone zone, RedjetEggGeneration gen, uint32_t tid) {

  // 1. Check free slab pool first
  {
    hs_ticket_guard_t free_guard(_free_lock);
    if (_free_slab_head != nullptr) {
      RedjetEggSlabHeader* recycled = _free_slab_head;
      _free_slab_head = recycled->next_slab;
      recycled->next_slab = nullptr;
      recycled->prev_slab = nullptr;
      recycled->zone = zone;
      recycled->generation = gen;
      recycled->owner_thread_id = tid;
      recycled->allocation_epoch = static_cast<uint32_t>(_current_epoch.load(std::memory_order_relaxed));
      recycled->reset();
      return recycled;
    }
  }

  // 2. Allocate 64KB page from OS aligned to 64 bytes
  uint8_t* mem = nullptr;
#if defined(_WIN32)
  mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, RedjetEggSlabHeader::SLAB_CAPACITY_BYTES,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
  mem = static_cast<uint8_t*>(mmap(nullptr, RedjetEggSlabHeader::SLAB_CAPACITY_BYTES,
                                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (mem == MAP_FAILED) mem = nullptr;
#endif

  if (mem == nullptr) {
    return nullptr;
  }

  auto* slab = new (std::nothrow) RedjetEggSlabHeader();
  if (slab == nullptr) {
#if defined(_WIN32)
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, RedjetEggSlabHeader::SLAB_CAPACITY_BYTES);
#endif
    return nullptr;
  }

  slab->slab_base = mem;
  slab->bump_top.store(mem, std::memory_order_relaxed);
  slab->slab_end = mem + RedjetEggSlabHeader::SLAB_CAPACITY_BYTES;
  slab->zone = zone;
  slab->generation = gen;
  slab->owner_thread_id = tid;
  slab->allocation_epoch = static_cast<uint32_t>(_current_epoch.load(std::memory_order_relaxed));
  slab->allocated_bytes.store(0, std::memory_order_relaxed);
  slab->reclaimed_bytes.store(0, std::memory_order_relaxed);

  {
    hs_ticket_guard_t slab_guard(_slab_lock);
    size_t idx = _slab_count.load(std::memory_order_relaxed);
    if (idx < MAX_TRACKED_SLABS) {
      slab->slab_id = static_cast<uint32_t>(idx);
      _slabs[idx] = slab;
      _slab_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  add_used(RedjetEggSlabHeader::SLAB_CAPACITY_BYTES);
  return slab;
}

void RedjetVFSNode_RunwayEgg::free_slab_internal(RedjetEggSlabHeader* slab) {
  if (slab == nullptr) return;
  slab->reset();
  hs_ticket_guard_t free_guard(_free_lock);
  slab->next_slab = _free_slab_head;
  _free_slab_head = slab;
  _total_slabs_recycled.fetch_add(1, std::memory_order_relaxed);
}

void* RedjetVFSNode_RunwayEgg::allocate_fast(
    size_t bytes, uint32_t thread_id, RedjetEggZone zone, RedjetEggGeneration gen) {

  if (bytes == 0 || bytes > RedjetEggSlabHeader::SLAB_CAPACITY_BYTES) {
    return nullptr;
  }

  size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);
  uint32_t slot = thread_id % MAX_CACHED_THREADS;
  RedjetThreadEggTLAB& tlab = _thread_tlabs[slot];

  // Fast path: thread-local bump pointer
  if (tlab.thread_bump_ptr != nullptr && tlab.thread_bump_ptr + aligned <= tlab.thread_end_ptr) {
    void* result = tlab.thread_bump_ptr;
    tlab.thread_bump_ptr += aligned;
    tlab.thread_allocated_bytes += aligned;
    _total_allocations.fetch_add(1, std::memory_order_relaxed);
    _total_bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
    return result;
  }

  // Slow path: refill thread buffer from slab or allocate new slab
  return refill_and_allocate(bytes, thread_id, zone, gen);
}

void* RedjetVFSNode_RunwayEgg::refill_and_allocate(
    size_t bytes, uint32_t thread_id, RedjetEggZone zone, RedjetEggGeneration gen) {

  size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);
  uint32_t slot = thread_id % MAX_CACHED_THREADS;
  RedjetThreadEggTLAB& tlab = _thread_tlabs[slot];

  // Try allocating from current slab first if present
  if (tlab.current_slab != nullptr) {
    void* obj = tlab.current_slab->allocate(aligned);
    if (obj != nullptr) {
      _total_allocations.fetch_add(1, std::memory_order_relaxed);
      _total_bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
      return obj;
    }
  }

  // Current slab is full; request a fresh 64KB slab
  RedjetEggSlabHeader* new_slab = allocate_new_slab(zone, gen, thread_id);
  if (new_slab == nullptr) {
    return nullptr; // Out of memory or quota exceeded
  }

  tlab.current_slab = new_slab;
  tlab.thread_bump_ptr = new_slab->slab_base;
  tlab.thread_end_ptr = new_slab->slab_end;

  void* result = tlab.thread_bump_ptr;
  tlab.thread_bump_ptr += aligned;
  tlab.thread_allocated_bytes += aligned;

  new_slab->bump_top.store(tlab.thread_bump_ptr, std::memory_order_relaxed);
  new_slab->allocated_bytes.store(aligned, std::memory_order_relaxed);

  _total_allocations.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
  return result;
}

size_t RedjetVFSNode_RunwayEgg::advance_epoch_and_recycle() {
  uint64_t old_epoch = _current_epoch.fetch_add(1, std::memory_order_relaxed);
  size_t recycled_count = 0;

  hs_ticket_guard_t guard(_slab_lock);
  size_t count = _slab_count.load(std::memory_order_relaxed);

  for (size_t i = 0; i < count; ++i) {
    RedjetEggSlabHeader* slab = _slabs[i];
    if (slab != nullptr) {
      // If slab belongs to past epoch and has zero surviving references
      if (slab->allocation_epoch < old_epoch &&
          slab->reclaimed_bytes.load(std::memory_order_relaxed) >= slab->allocated_bytes.load(std::memory_order_relaxed)) {
        free_slab_internal(slab);
        ++recycled_count;
      }
    }
  }

  return recycled_count;
}

void RedjetVFSNode_RunwayEgg::print_on(outputStream* st) const {
  st->print_cr("Redjet Runway Motherline Egg Node [%s]:", full_path());
  st->print_cr("  Quota: %zu KB, Used: %zu KB", quota() / 1024, used() / 1024);
  st->print_cr("  Total Slabs: %zu, Current Epoch: " UINT64_FORMAT, total_slabs(), current_epoch());
  st->print_cr("  Total Allocations: " UINT64_FORMAT ", Total Bytes: " UINT64_FORMAT,
               total_allocations(), total_bytes_allocated());
  st->print_cr("  Total Slabs Recycled: " UINT64_FORMAT ", Promotions: " UINT64_FORMAT,
               total_slabs_recycled(), total_promotions());
}
