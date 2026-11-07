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

#include "gc/redjet/vfs/redjetVFSNode_RunwayFledgling.hpp"
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

RedjetVFSNode_RunwayFledgling::RedjetVFSNode_RunwayFledgling(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  for (size_t i = 0; i < MAX_FLEDGLING_SLABS; ++i) {
    _slabs[i] = nullptr;
  }
}

RedjetVFSNode_RunwayFledgling::~RedjetVFSNode_RunwayFledgling() {
  hs_ticket_guard_t guard(_fledgling_lock);
  size_t count = _slab_count.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    if (_slabs[i] != nullptr) {
      RedjetSurvivorEntry* cur = _slabs[i]->entry_head;
      while (cur != nullptr) {
        RedjetSurvivorEntry* next = cur->next;
        delete cur;
        cur = next;
      }
      _slabs[i]->entry_head = nullptr;

      if (_slabs[i]->slab_base != nullptr) {
#if defined(_WIN32)
        VirtualFree(_slabs[i]->slab_base, 0, MEM_RELEASE);
#else
        munmap(_slabs[i]->slab_base, RedjetFledglingSlab::FLEDGLING_SLAB_SIZE);
#endif
      }
      delete _slabs[i];
      _slabs[i] = nullptr;
    }
  }
  _slab_count.store(0, std::memory_order_relaxed);
}

RedjetFledglingSlab* RedjetVFSNode_RunwayFledgling::allocate_slab() {
  uint8_t* mem = nullptr;
#if defined(_WIN32)
  mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, RedjetFledglingSlab::FLEDGLING_SLAB_SIZE,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
  mem = static_cast<uint8_t*>(mmap(nullptr, RedjetFledglingSlab::FLEDGLING_SLAB_SIZE,
                                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (mem == MAP_FAILED) mem = nullptr;
#endif

  if (mem == nullptr) {
    return nullptr;
  }

  auto* slab = new (std::nothrow) RedjetFledglingSlab();
  if (slab == nullptr) {
#if defined(_WIN32)
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, RedjetFledglingSlab::FLEDGLING_SLAB_SIZE);
#endif
    return nullptr;
  }

  slab->slab_base = mem;
  slab->free_ptr = mem;
  slab->slab_end = mem + RedjetFledglingSlab::FLEDGLING_SLAB_SIZE;
  slab->live_bytes.store(0, std::memory_order_relaxed);
  slab->survivor_count.store(0, std::memory_order_relaxed);
  slab->entry_head = nullptr;

  size_t idx = _slab_count.load(std::memory_order_relaxed);
  if (idx < MAX_FLEDGLING_SLABS) {
    slab->slab_id = static_cast<uint32_t>(idx);
    _slabs[idx] = slab;
    _slab_count.fetch_add(1, std::memory_order_relaxed);
  }

  add_used(RedjetFledglingSlab::FLEDGLING_SLAB_SIZE);
  return slab;
}

void* RedjetVFSNode_RunwayFledgling::admit_survivor(
    void* original_oop, size_t object_size, bool is_puzzler_optimized, uint32_t zone_hint) {

  if (original_oop == nullptr || object_size == 0) {
    return nullptr;
  }

  size_t aligned_size = (object_size + 7) & ~static_cast<size_t>(7);
  hs_ticket_guard_t guard(_fledgling_lock);

  // Search existing slabs with available capacity
  RedjetFledglingSlab* target_slab = nullptr;
  size_t count = _slab_count.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    if (_slabs[i] != nullptr && _slabs[i]->can_fit(aligned_size)) {
      target_slab = _slabs[i];
      break;
    }
  }

  if (target_slab == nullptr) {
    target_slab = allocate_slab();
    if (target_slab == nullptr) {
      return nullptr; // Quota or OS out of memory
    }
  }

  void* compact_dest = target_slab->append(aligned_size);
  if (compact_dest == nullptr) {
    return nullptr;
  }

  // Copy survivor data into dense fledgling slab
  std::memcpy(compact_dest, original_oop, object_size);

  auto* entry = new (std::nothrow) RedjetSurvivorEntry();
  if (entry != nullptr) {
    entry->oop_address = compact_dest;
    entry->object_size = aligned_size;
    entry->survival_age = 1;
    entry->origin_zone = zone_hint;
    entry->admission_epoch = _fledgling_epoch.load(std::memory_order_relaxed);
    entry->last_access_tsc = sacculus_rdtsc();
    entry->is_optimized = is_puzzler_optimized;
    entry->marked_for_hotfield = false;

    hs_ticket_guard_t slab_guard(target_slab->slab_lock);
    entry->next = target_slab->entry_head;
    target_slab->entry_head = entry;
    target_slab->survivor_count.fetch_add(1, std::memory_order_relaxed);
  }

  _total_admissions.fetch_add(1, std::memory_order_relaxed);
  return compact_dest;
}

size_t RedjetVFSNode_RunwayFledgling::execute_in_slab_compaction() {
  hs_ticket_guard_t guard(_fledgling_lock);
  size_t count = _slab_count.load(std::memory_order_relaxed);
  size_t total_compacted = 0;

  for (size_t i = 0; i < count; ++i) {
    RedjetFledglingSlab* slab = _slabs[i];
    if (slab == nullptr) continue;

    hs_ticket_guard_t slab_guard(slab->slab_lock);
    // Relocate active survivors contiguously from slab_base
    uint8_t* write_ptr = slab->slab_base;
    RedjetSurvivorEntry* cur = slab->entry_head;

    while (cur != nullptr) {
      if (cur->oop_address != nullptr && cur->object_size > 0) {
        if (reinterpret_cast<uint8_t*>(cur->oop_address) != write_ptr) {
          std::memmove(write_ptr, cur->oop_address, cur->object_size);
          cur->oop_address = write_ptr;
          total_compacted += cur->object_size;
        }
        write_ptr += cur->object_size;
      }
      cur = cur->next;
    }

    slab->free_ptr = write_ptr;
    slab->live_bytes.store(static_cast<size_t>(write_ptr - slab->slab_base), std::memory_order_relaxed);
  }

  _total_compactions.fetch_add(1, std::memory_order_relaxed);
  _bytes_compacted.fetch_add(total_compacted, std::memory_order_relaxed);
  return total_compacted;
}

size_t RedjetVFSNode_RunwayFledgling::advance_epoch_and_mark_promotions(
    void** out_promoted_oops, size_t max_out) {

  _fledgling_epoch.fetch_add(1, std::memory_order_relaxed);
  size_t promoted_count = 0;

  hs_ticket_guard_t guard(_fledgling_lock);
  size_t count = _slab_count.load(std::memory_order_relaxed);

  for (size_t i = 0; i < count && promoted_count < max_out; ++i) {
    RedjetFledglingSlab* slab = _slabs[i];
    if (slab == nullptr) continue;

    hs_ticket_guard_t slab_guard(slab->slab_lock);
    RedjetSurvivorEntry* cur = slab->entry_head;

    while (cur != nullptr && promoted_count < max_out) {
      cur->survival_age++;
      if (cur->survival_age >= PROMOTION_AGE_THRESHOLD && !cur->marked_for_hotfield) {
        cur->marked_for_hotfield = true;
        if (out_promoted_oops != nullptr) {
          out_promoted_oops[promoted_count] = cur->oop_address;
        }
        ++promoted_count;
        _total_promotions.fetch_add(1, std::memory_order_relaxed);
      }
      cur = cur->next;
    }
  }

  return promoted_count;
}

void RedjetVFSNode_RunwayFledgling::print_on(outputStream* st) const {
  st->print_cr("Redjet Runway Motherline Fledgling Node [%s]:", full_path());
  st->print_cr("  Quota: %zu KB, Used: %zu KB", quota() / 1024, used() / 1024);
  st->print_cr("  Total Slabs: %zu, Current Epoch: " UINT64_FORMAT, slab_count(), current_epoch());
  st->print_cr("  Total Admissions: " UINT64_FORMAT ", Promotions: " UINT64_FORMAT,
               total_admissions(), total_promotions());
  st->print_cr("  Compactions: " UINT64_FORMAT ", Compacted Bytes: " UINT64_FORMAT,
               total_compactions(), bytes_compacted());
}
