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

#include "gc/redjet/vfs/redjetVFSNode_Pitstop.hpp"
#include <cstring>
#include <cstdlib>

RedjetVFSNode_Pitstop::RedjetVFSNode_Pitstop(const char* name,
                                             const char* full_path,
                                             RedjetVFSNode* parent,
                                             size_t quota_bytes) :
  RedjetVFSNode(name, full_path, parent, quota_bytes),
  _slots_head(nullptr),
  _slots_tail(nullptr) {
}

RedjetVFSNode_Pitstop::~RedjetVFSNode_Pitstop() {
  _slot_lock.lock();
  RedjetPitstopMemSlot* cur = _slots_head;
  while (cur != nullptr) {
    RedjetPitstopMemSlot* next = cur->next;
    if (cur->shadow_buffer != nullptr) {
      free(cur->shadow_buffer);
      cur->shadow_buffer = nullptr;
    }
    delete cur;
    cur = next;
  }
  _slots_head = nullptr;
  _slots_tail = nullptr;
  _active_slots_count.store(0, std::memory_order_relaxed);
  _used_bytes.store(0, std::memory_order_relaxed);
  _slot_lock.unlock();
}

RedjetPitstopMemSlot* RedjetVFSNode_Pitstop::admit_slot(void* original_slot,
                                                       size_t slot_bytes,
                                                       uint32_t duel_id,
                                                       uint32_t generation,
                                                       uint32_t region_id,
                                                       RedjetPitstopMorphMode mode) {
  if (original_slot == nullptr || slot_bytes == 0) {
    return nullptr;
  }

  auto* entry = new RedjetPitstopMemSlot();
  entry->original_slot_addr = original_slot;
  entry->slot_bytes = slot_bytes;
  entry->duel_id = duel_id;
  entry->generation = generation;
  entry->region_id = region_id;
  entry->morph_mode = mode;
  entry->state.store(static_cast<uint8_t>(RedjetPitstopEntryState::STAGED_FOR_PUZZLER), std::memory_order_release);
  entry->entry_tsc = sacculus_rdtsc();

  size_t total_alloc_bytes = sizeof(RedjetPitstopMemSlot);

  if (mode == RedjetPitstopMorphMode::SHADOW_RCU) {
    size_t alloc_sz = (slot_bytes + 63) & ~63;
    entry->shadow_buffer = malloc(alloc_sz);
    if (entry->shadow_buffer != nullptr) {
      memcpy(entry->shadow_buffer, original_slot, slot_bytes);
      entry->shadow_bytes = alloc_sz;
      total_alloc_bytes += alloc_sz;
    }
  }

  _slot_lock.lock();
  entry->next = nullptr;
  entry->prev = _slots_tail;
  if (_slots_tail != nullptr) {
    _slots_tail->next = entry;
  } else {
    _slots_head = entry;
  }
  _slots_tail = entry;
  _slot_lock.unlock();

  _total_admissions.fetch_add(1, std::memory_order_relaxed);
  _active_slots_count.fetch_add(1, std::memory_order_relaxed);
  add_used(total_alloc_bytes);

  return entry;
}

bool RedjetVFSNode_Pitstop::update_slot_state(void* original_slot, RedjetPitstopEntryState new_state) {
  if (original_slot == nullptr) return false;

  _slot_lock.lock();
  RedjetPitstopMemSlot* cur = _slots_head;
  while (cur != nullptr) {
    if (cur->original_slot_addr == original_slot) {
      cur->state.store(static_cast<uint8_t>(new_state), std::memory_order_release);
      _slot_lock.unlock();
      return true;
    }
    cur = cur->next;
  }
  _slot_lock.unlock();
  return false;
}

bool RedjetVFSNode_Pitstop::record_duel_result(void* original_slot,
                                              uint16_t winning_simd_idx,
                                              uint64_t baseline_cyc,
                                              uint64_t candidate_cyc) {
  if (original_slot == nullptr) return false;

  _slot_lock.lock();
  RedjetPitstopMemSlot* cur = _slots_head;
  while (cur != nullptr) {
    if (cur->original_slot_addr == original_slot) {
      cur->baseline_cycles = baseline_cyc;
      cur->candidate_cycles = candidate_cyc;
      cur->winning_simd_idx = winning_simd_idx;

      uint64_t gained = (baseline_cyc > candidate_cyc) ? (baseline_cyc - candidate_cyc) : 0;
      cur->cycles_gained = gained;
      cur->state.store(static_cast<uint8_t>(RedjetPitstopEntryState::READY_TO_RETURN), std::memory_order_release);

      _total_duels_completed.fetch_add(1, std::memory_order_relaxed);
      if (gained > 0) {
        _total_duels_won.fetch_add(1, std::memory_order_relaxed);
        _total_cycles_saved.fetch_add(gained, std::memory_order_relaxed);
      }
      _slot_lock.unlock();
      return true;
    }
    cur = cur->next;
  }
  _slot_lock.unlock();
  return false;
}

bool RedjetVFSNode_Pitstop::return_slot_to_origin(void* original_slot,
                                                  RedjetRunway* runway,
                                                  RedjetVFSNode_Hotfield* hotfield) {
  (void)runway;
  (void)hotfield;
  if (original_slot == nullptr) return false;

  _slot_lock.lock();
  RedjetPitstopMemSlot* cur = _slots_head;
  while (cur != nullptr) {
    if (cur->original_slot_addr == original_slot) {
      // If shadow copy was used and winner achieved speedup, commit morphed data back
      if (cur->shadow_buffer != nullptr && cur->cycles_gained > 0) {
        memcpy(cur->original_slot_addr, cur->shadow_buffer, cur->slot_bytes);
      }

      // Unlink
      if (cur->prev != nullptr) {
        cur->prev->next = cur->next;
      } else {
        _slots_head = cur->next;
      }
      if (cur->next != nullptr) {
        cur->next->prev = cur->prev;
      } else {
        _slots_tail = cur->prev;
      }
      break;
    }
    cur = cur->next;
  }
  _slot_lock.unlock();

  if (cur == nullptr) return false;

  size_t reclaimed = sizeof(RedjetPitstopMemSlot) + cur->shadow_bytes;
  if (cur->shadow_buffer != nullptr) {
    free(cur->shadow_buffer);
    cur->shadow_buffer = nullptr;
  }
  delete cur;

  _total_returns_dispatched.fetch_add(1, std::memory_order_relaxed);
  _active_slots_count.fetch_sub(1, std::memory_order_relaxed);
  sub_used(reclaimed);

  return true;
}

bool RedjetVFSNode_Pitstop::abort_and_release(void* original_slot) {
  if (original_slot == nullptr) return false;

  _slot_lock.lock();
  RedjetPitstopMemSlot* cur = _slots_head;
  while (cur != nullptr) {
    if (cur->original_slot_addr == original_slot) {
      if (cur->prev != nullptr) {
        cur->prev->next = cur->next;
      } else {
        _slots_head = cur->next;
      }
      if (cur->next != nullptr) {
        cur->next->prev = cur->prev;
      } else {
        _slots_tail = cur->prev;
      }
      break;
    }
    cur = cur->next;
  }
  _slot_lock.unlock();

  if (cur == nullptr) return false;

  size_t reclaimed = sizeof(RedjetPitstopMemSlot) + cur->shadow_bytes;
  if (cur->shadow_buffer != nullptr) {
    free(cur->shadow_buffer);
    cur->shadow_buffer = nullptr;
  }
  delete cur;

  _total_aborts.fetch_add(1, std::memory_order_relaxed);
  _active_slots_count.fetch_sub(1, std::memory_order_relaxed);
  sub_used(reclaimed);

  return true;
}

RedjetPitstopEntryState RedjetVFSNode_Pitstop::get_slot_state(const void* original_slot) const noexcept {
  if (original_slot == nullptr) return RedjetPitstopEntryState::ABORTED;

  const_cast<hs_ticket_lock_t&>(_slot_lock).lock();
  const RedjetPitstopMemSlot* cur = _slots_head;
  while (cur != nullptr) {
    if (cur->original_slot_addr == original_slot) {
      RedjetPitstopEntryState s = static_cast<RedjetPitstopEntryState>(cur->state.load(std::memory_order_acquire));
      const_cast<hs_ticket_lock_t&>(_slot_lock).unlock();
      return s;
    }
    cur = cur->next;
  }
  const_cast<hs_ticket_lock_t&>(_slot_lock).unlock();
  return RedjetPitstopEntryState::ABORTED;
}
