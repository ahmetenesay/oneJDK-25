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

#include "gc/redjet/vfs/redjetVFSNode_Hotfield.hpp"
#include <new>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
static const size_t TIER_BOUNDS[RedjetVFSNode_Hotfield::NUM_HEIGHT_TIERS][2] = {
    { 0,      64 },
    { 65,     128 },
    { 129,    256 },
    { 257,    512 },
    { 513,    1024 },
    { 1025,   2048 },
    { 2049,   4096 },
    { 4097,   8192 },
    { 8193,   16384 },
    { 16385,  32768 },
    { 32769,  65536 },
    { 65537,  262144 }
};
} // namespace

void* RedjetOOHSlab::allocate_slot() noexcept {
  hs_ticket_guard_t guard(slab_lock);
  if (free_slots.load(std::memory_order_relaxed) == 0) {
    return nullptr;
  }

  size_t words = (total_slots + 63) / 64;
  for (size_t w = 0; w < words; ++w) {
    uint64_t map = allocation_bitmap[w].load(std::memory_order_relaxed);
    if (map != ~static_cast<uint64_t>(0)) {
      // Found word with available slot
      for (int bit = 0; bit < 64; ++bit) {
        size_t slot_idx = w * 64 + bit;
        if (slot_idx >= total_slots) break;

        if ((map & (1ULL << bit)) == 0) {
          allocation_bitmap[w].store(map | (1ULL << bit), std::memory_order_relaxed);
          free_slots.fetch_sub(1, std::memory_order_relaxed);
          return memory_base + (slot_idx * slot_size);
        }
      }
    }
  }

  return nullptr;
}

bool RedjetOOHSlab::free_slot(void* ptr) noexcept {
  if (ptr < memory_base || ptr >= (memory_base + SLAB_PAGE_SIZE)) {
    return false;
  }

  hs_ticket_guard_t guard(slab_lock);
  uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(memory_base);
  size_t slot_idx = offset / slot_size;

  if (slot_idx < total_slots) {
    size_t w = slot_idx / 64;
    int bit = static_cast<int>(slot_idx % 64);
    uint64_t map = allocation_bitmap[w].load(std::memory_order_relaxed);

    if ((map & (1ULL << bit)) != 0) {
      allocation_bitmap[w].store(map & ~(1ULL << bit), std::memory_order_relaxed);
      free_slots.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  return false;
}

RedjetVFSNode_Hotfield::RedjetVFSNode_Hotfield(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes) :
    RedjetVFSNode(name, full_path, parent, quota_bytes) {

  for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
    _tiers[i].min_bytes = TIER_BOUNDS[i][0];
    _tiers[i].max_bytes = TIER_BOUNDS[i][1];
    _tiers[i].head = nullptr;
    _tiers[i].slot_count.store(0, std::memory_order_relaxed);
    _tiers[i].total_bytes.store(0, std::memory_order_relaxed);
  }

  for (size_t c = 0; c < NUM_OOH_CLASSES; ++c) {
    _ooh_classes[c].slot_size = OOH_SIZES[c];
    _ooh_classes[c].slots_per_slab = RedjetOOHSlab::SLAB_PAGE_SIZE / OOH_SIZES[c];
    _ooh_classes[c].active_slab = nullptr;
    _ooh_classes[c].full_slabs_head = nullptr;
    _ooh_classes[c].total_allocated_slots.store(0, std::memory_order_relaxed);
    _ooh_classes[c].total_live_bytes.store(0, std::memory_order_relaxed);
  }
}

RedjetVFSNode_Hotfield::~RedjetVFSNode_Hotfield() {
  for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
    hs_ticket_guard_t guard(_tiers[i].tier_lock);
    RedjetHotfieldSlot* cur = _tiers[i].head;
    while (cur != nullptr) {
      RedjetHotfieldSlot* next = cur->next;
      delete cur;
      cur = next;
    }
    _tiers[i].head = nullptr;
    _tiers[i].slot_count.store(0, std::memory_order_relaxed);
    _tiers[i].total_bytes.store(0, std::memory_order_relaxed);
  }

  for (size_t c = 0; c < NUM_OOH_CLASSES; ++c) {
    hs_ticket_guard_t guard(_ooh_classes[c].class_lock);
    RedjetOOHSlab* slab = _ooh_classes[c].active_slab;
    while (slab != nullptr) {
      RedjetOOHSlab* next = slab->next_slab;
      if (slab->memory_base != nullptr) {
#if defined(_WIN32)
        VirtualFree(slab->memory_base, 0, MEM_RELEASE);
#else
        munmap(slab->memory_base, RedjetOOHSlab::SLAB_PAGE_SIZE);
#endif
      }
      delete slab;
      slab = next;
    }

    slab = _ooh_classes[c].full_slabs_head;
    while (slab != nullptr) {
      RedjetOOHSlab* next = slab->next_slab;
      if (slab->memory_base != nullptr) {
#if defined(_WIN32)
        VirtualFree(slab->memory_base, 0, MEM_RELEASE);
#else
        munmap(slab->memory_base, RedjetOOHSlab::SLAB_PAGE_SIZE);
#endif
      }
      delete slab;
      slab = next;
    }
  }
}

size_t RedjetVFSNode_Hotfield::find_tier_for_size(size_t bytes) const noexcept {
  for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
    if (bytes <= _tiers[i].max_bytes) {
      return i;
    }
  }
  return NUM_HEIGHT_TIERS - 1;
}

size_t RedjetVFSNode_Hotfield::find_ooh_class_for_size(size_t bytes) const noexcept {
  for (size_t i = 0; i < NUM_OOH_CLASSES; ++i) {
    if (bytes <= OOH_SIZES[i]) {
      return i;
    }
  }
  return NUM_OOH_CLASSES - 1;
}

RedjetOOHSlab* RedjetVFSNode_Hotfield::allocate_ooh_slab_page(size_t class_idx) {
  uint8_t* mem = nullptr;
#if defined(_WIN32)
  mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, RedjetOOHSlab::SLAB_PAGE_SIZE,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
  mem = static_cast<uint8_t*>(mmap(nullptr, RedjetOOHSlab::SLAB_PAGE_SIZE,
                                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (mem == MAP_FAILED) mem = nullptr;
#endif

  if (mem == nullptr) return nullptr;

  auto* slab = new (std::nothrow) RedjetOOHSlab();
  if (slab == nullptr) {
#if defined(_WIN32)
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, RedjetOOHSlab::SLAB_PAGE_SIZE);
#endif
    return nullptr;
  }

  slab->memory_base = mem;
  slab->slot_size = OOH_SIZES[class_idx];
  slab->total_slots = RedjetOOHSlab::SLAB_PAGE_SIZE / slab->slot_size;
  slab->free_slots.store(slab->total_slots, std::memory_order_relaxed);
  slab->class_index = static_cast<uint32_t>(class_idx);
  slab->next_slab = nullptr;
  slab->prev_slab = nullptr;

  for (size_t w = 0; w < RedjetOOHSlab::BITMAP_WORDS; ++w) {
    slab->allocation_bitmap[w].store(0, std::memory_order_relaxed);
  }

  add_used(RedjetOOHSlab::SLAB_PAGE_SIZE);
  return slab;
}

void* RedjetVFSNode_Hotfield::allocate_ooh(size_t requested_bytes, size_t* out_actual_size) {
  if (requested_bytes == 0 || requested_bytes > OOH_SIZES[NUM_OOH_CLASSES - 1]) {
    return nullptr;
  }

  size_t c_idx = find_ooh_class_for_size(requested_bytes);
  RedjetOOHClass& cls = _ooh_classes[c_idx];

  if (out_actual_size != nullptr) {
    *out_actual_size = cls.slot_size;
  }

  hs_ticket_guard_t guard(cls.class_lock);

  // 1. Try active slab
  if (cls.active_slab != nullptr) {
    void* slot = cls.active_slab->allocate_slot();
    if (slot != nullptr) {
      cls.total_allocated_slots.fetch_add(1, std::memory_order_relaxed);
      cls.total_live_bytes.fetch_add(cls.slot_size, std::memory_order_relaxed);
      _total_ooh_allocations.fetch_add(1, std::memory_order_relaxed);
      return slot;
    }

    // Active slab full: move to full list
    RedjetOOHSlab* full = cls.active_slab;
    cls.active_slab = nullptr;
    full->next_slab = cls.full_slabs_head;
    cls.full_slabs_head = full;
  }

  // 2. Allocate fresh slab page
  RedjetOOHSlab* new_slab = allocate_ooh_slab_page(c_idx);
  if (new_slab == nullptr) {
    return nullptr;
  }

  cls.active_slab = new_slab;
  void* slot = new_slab->allocate_slot();
  if (slot != nullptr) {
    cls.total_allocated_slots.fetch_add(1, std::memory_order_relaxed);
    cls.total_live_bytes.fetch_add(cls.slot_size, std::memory_order_relaxed);
    _total_ooh_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  return slot;
}

bool RedjetVFSNode_Hotfield::free_ooh(void* ptr, size_t footprint_bytes) {
  if (ptr == nullptr || footprint_bytes == 0 || footprint_bytes > OOH_SIZES[NUM_OOH_CLASSES - 1]) return false;

  size_t c_idx = find_ooh_class_for_size(footprint_bytes);
  RedjetOOHClass& cls = _ooh_classes[c_idx];

  hs_ticket_guard_t guard(cls.class_lock);

  // Check active slab
  if (cls.active_slab != nullptr && cls.active_slab->free_slot(ptr)) {
    cls.total_allocated_slots.fetch_sub(1, std::memory_order_relaxed);
    cls.total_live_bytes.fetch_sub(cls.slot_size, std::memory_order_relaxed);
    _total_ooh_frees.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Check full slabs
  RedjetOOHSlab* prev = nullptr;
  RedjetOOHSlab* cur = cls.full_slabs_head;
  while (cur != nullptr) {
    if (cur->free_slot(ptr)) {
      cls.total_allocated_slots.fetch_sub(1, std::memory_order_relaxed);
      cls.total_live_bytes.fetch_sub(cls.slot_size, std::memory_order_relaxed);
      _total_ooh_frees.fetch_add(1, std::memory_order_relaxed);

      // Slabs in full list with newly available slots become active if active is null
      if (cls.active_slab == nullptr) {
        if (prev != nullptr) {
          prev->next_slab = cur->next_slab;
        } else {
          cls.full_slabs_head = cur->next_slab;
        }
        cur->next_slab = nullptr;
        cls.active_slab = cur;
      }
      return true;
    }
    prev = cur;
    cur = cur->next_slab;
  }

  return false;
}

bool RedjetVFSNode_Hotfield::insert_object(void* obj_ptr, size_t footprint, uint32_t initial_heat) {
  if (!obj_ptr || footprint == 0) return false;

  size_t tier_idx = find_tier_for_size(footprint);
  RedjetHotfieldHeightTier& tier = _tiers[tier_idx];

  auto* slot = new (std::nothrow) RedjetHotfieldSlot();
  if (!slot) return false;

  slot->obj_ptr = obj_ptr;
  slot->footprint_bytes = footprint;
  slot->height_tier = static_cast<uint32_t>(tier_idx);
  slot->access_heat = initial_heat;
  slot->last_access_tsc = sacculus_rdtsc();

  hs_ticket_guard_t guard(tier.tier_lock);
  RedjetHotfieldSlot* prev = nullptr;
  RedjetHotfieldSlot* cur = tier.head;
  while (cur != nullptr && cur->access_heat >= initial_heat) {
    prev = cur;
    cur = cur->next;
  }

  if (prev == nullptr) {
    slot->next = tier.head;
    tier.head = slot;
  } else {
    slot->next = prev->next;
    prev->next = slot;
  }

  tier.slot_count.fetch_add(1, std::memory_order_relaxed);
  tier.total_bytes.fetch_add(footprint, std::memory_order_relaxed);
  add_used(footprint);

  _total_inserts.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool RedjetVFSNode_Hotfield::remove_object(void* obj_ptr, size_t footprint) {
  if (!obj_ptr) return false;

  size_t tier_idx = find_tier_for_size(footprint);
  RedjetHotfieldHeightTier& tier = _tiers[tier_idx];

  hs_ticket_guard_t guard(tier.tier_lock);
  RedjetHotfieldSlot* prev = nullptr;
  RedjetHotfieldSlot* cur = tier.head;

  while (cur != nullptr) {
    if (cur->obj_ptr == obj_ptr) {
      if (prev == nullptr) {
        tier.head = cur->next;
      } else {
        prev->next = cur->next;
      }

      tier.slot_count.fetch_sub(1, std::memory_order_relaxed);
      tier.total_bytes.fetch_sub(cur->footprint_bytes, std::memory_order_relaxed);
      sub_used(cur->footprint_bytes);

      delete cur;
      _total_removals.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    prev = cur;
    cur = cur->next;
  }

  return false;
}

void RedjetVFSNode_Hotfield::touch_object(void* obj_ptr, size_t footprint) noexcept {
  if (!obj_ptr) return;

  size_t tier_idx = find_tier_for_size(footprint);
  RedjetHotfieldHeightTier& tier = _tiers[tier_idx];

  hs_ticket_guard_t guard(tier.tier_lock);
  RedjetHotfieldSlot* cur = tier.head;
  while (cur != nullptr) {
    if (cur->obj_ptr == obj_ptr) {
      cur->access_heat++;
      cur->last_access_tsc = sacculus_rdtsc();
      _total_heat_touches.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    cur = cur->next;
  }
}

void RedjetVFSNode_Hotfield::rebalance_and_sort_tiers() {
  for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
    RedjetHotfieldHeightTier& tier = _tiers[i];
    hs_ticket_guard_t guard(tier.tier_lock);

    if (tier.head == nullptr || tier.head->next == nullptr) {
      continue;
    }

    // Merge-sort on access_heat descending
    RedjetHotfieldSlot* sorted = nullptr;
    RedjetHotfieldSlot* cur = tier.head;

    while (cur != nullptr) {
      RedjetHotfieldSlot* next = cur->next;

      if (sorted == nullptr || sorted->access_heat < cur->access_heat) {
        cur->next = sorted;
        sorted = cur;
      } else {
        RedjetHotfieldSlot* s_cur = sorted;
        while (s_cur->next != nullptr && s_cur->next->access_heat >= cur->access_heat) {
          s_cur = s_cur->next;
        }
        cur->next = s_cur->next;
        s_cur->next = cur;
      }
      cur = next;
    }
    tier.head = sorted;
  }
}

void* RedjetVFSNode_Hotfield::find_best_fit_slot(size_t needed_bytes, size_t* out_actual_size) {
  return allocate_ooh(needed_bytes, out_actual_size);
}

void RedjetVFSNode_Hotfield::get_tier_stats(size_t tier_idx, size_t* out_slots, size_t* out_bytes) const noexcept {
  if (tier_idx >= NUM_HEIGHT_TIERS) {
    if (out_slots) *out_slots = 0;
    if (out_bytes) *out_bytes = 0;
    return;
  }
  if (out_slots) *out_slots = _tiers[tier_idx].slot_count.load(std::memory_order_relaxed);
  if (out_bytes) *out_bytes = _tiers[tier_idx].total_bytes.load(std::memory_order_relaxed);
}

void RedjetVFSNode_Hotfield::get_ooh_stats(size_t class_idx, size_t* out_slots, size_t* out_bytes) const noexcept {
  if (class_idx >= NUM_OOH_CLASSES) {
    if (out_slots) *out_slots = 0;
    if (out_bytes) *out_bytes = 0;
    return;
  }
  if (out_slots) *out_slots = _ooh_classes[class_idx].total_allocated_slots.load(std::memory_order_relaxed);
  if (out_bytes) *out_bytes = _ooh_classes[class_idx].total_live_bytes.load(std::memory_order_relaxed);
}

void RedjetVFSNode_Hotfield::print_on(outputStream* st) const {
  st->print_cr("Redjet Hotfield Node [%s]:", full_path());
  st->print_cr("  Quota: %zu KB, Used: %zu KB", quota() / 1024, used() / 1024);
  st->print_cr("  OOH Allocations: " UINT64_FORMAT ", OOH Frees: " UINT64_FORMAT,
               total_ooh_allocations(), total_ooh_frees());
  st->print_cr("  Height Tier Inserts: " UINT64_FORMAT ", Removals: " UINT64_FORMAT,
               total_inserts(), total_removals());
}
