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

#include "gc/redjet/vfs/redjetVFSNode_Falline.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <algorithm>

RedjetVFSNode_Falline::RedjetVFSNode_Falline(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  for (size_t i = 0; i < MORGUE_CAPACITY; ++i) {
    _morgue_ring[i].dead_oop.store(nullptr, std::memory_order_relaxed);
    _morgue_ring[i].object_size = 0;
    _morgue_ring[i].originating_thread_id = 0;
    _morgue_ring[i].region_id = 0;
    _morgue_ring[i].quarantine_timestamp_tsc = 0;
    _morgue_ring[i].admission_epoch = 0;
    _morgue_ring[i].state.store(static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_EMPTY), std::memory_order_relaxed);
    _morgue_ring[i].resurrected.store(false, std::memory_order_relaxed);
  }
}

RedjetVFSNode_Falline::~RedjetVFSNode_Falline() {
  purge_morgue();
}

bool RedjetVFSNode_Falline::quarantine_object(
    void* oop, size_t object_size, uint32_t thread_id, uint32_t region_id) {

  if (oop == nullptr || object_size == 0) {
    return false;
  }

  uint64_t head = _head_cursor.load(std::memory_order_relaxed);
  uint64_t tail = _tail_cursor.load(std::memory_order_acquire);

  if ((head - tail) >= MORGUE_CAPACITY) {
    _quarantine_drops_on_full.fetch_add(1, std::memory_order_relaxed);
    return false; // Morgue ring saturated
  }

  uint64_t slot_idx = _head_cursor.fetch_add(1, std::memory_order_relaxed) & MORGUE_MASK;
  RedjetFallineMorgueSlot& slot = _morgue_ring[slot_idx];

  slot.dead_oop.store(oop, std::memory_order_relaxed);
  slot.object_size = object_size;
  slot.originating_thread_id = thread_id;
  slot.region_id = region_id;
  slot.quarantine_timestamp_tsc = sacculus_rdtsc();
  slot.admission_epoch = _falline_epoch.load(std::memory_order_relaxed);
  slot.resurrected.store(false, std::memory_order_relaxed);
  slot.state.store(static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_PROBATION), std::memory_order_release);

  add_used(object_size);
  _total_quarantined.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool RedjetVFSNode_Falline::attempt_rehabilitation(void* oop) {
  if (oop == nullptr) return false;

  uint64_t tail = _tail_cursor.load(std::memory_order_relaxed);
  uint64_t head = _head_cursor.load(std::memory_order_relaxed);

  for (uint64_t idx = tail; idx < head; ++idx) {
    RedjetFallineMorgueSlot& slot = _morgue_ring[idx & MORGUE_MASK];
    if (slot.dead_oop.load(std::memory_order_relaxed) == oop) {
      uint8_t cur_state = slot.state.load(std::memory_order_relaxed);
      if (cur_state == static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_PROBATION)) {
        if (slot.state.compare_exchange_strong(cur_state,
                                               static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_REHABILITATED),
                                               std::memory_order_acq_rel)) {
          slot.resurrected.store(true, std::memory_order_relaxed);
          sub_used(slot.object_size);
          _total_rehabilitated.fetch_add(1, std::memory_order_relaxed);
          return true; // Successfully rescued from morgue
        }
      }
    }
  }

  return false;
}

size_t RedjetVFSNode_Falline::advance_quarantine_epoch() {
  uint64_t new_epoch = _falline_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t newly_condemned = 0;

  uint64_t tail = _tail_cursor.load(std::memory_order_relaxed);
  uint64_t head = _head_cursor.load(std::memory_order_relaxed);

  for (uint64_t idx = tail; idx < head; ++idx) {
    RedjetFallineMorgueSlot& slot = _morgue_ring[idx & MORGUE_MASK];
    uint8_t cur_state = slot.state.load(std::memory_order_relaxed);

    if (cur_state == static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_PROBATION)) {
      if ((new_epoch - slot.admission_epoch) >= RETENTION_WINDOW_EPOCHS) {
        slot.state.store(static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_CONDEMNED), std::memory_order_release);
        ++newly_condemned;
        _total_condemned.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  return newly_condemned;
}

size_t RedjetVFSNode_Falline::drain_condemned_to_landfill(
    void** out_oops, size_t max_oops, size_t* out_drained_bytes) {

  size_t drained_count = 0;
  size_t bytes_acc = 0;

  uint64_t tail = _tail_cursor.load(std::memory_order_relaxed);
  uint64_t head = _head_cursor.load(std::memory_order_relaxed);

  while (tail < head && drained_count < max_oops) {
    RedjetFallineMorgueSlot& slot = _morgue_ring[tail & MORGUE_MASK];
    uint8_t cur_state = slot.state.load(std::memory_order_acquire);

    if (cur_state == static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_CONDEMNED)) {
      void* oop = slot.dead_oop.load(std::memory_order_relaxed);
      if (out_oops != nullptr && oop != nullptr) {
        out_oops[drained_count] = oop;
      }
      bytes_acc += slot.object_size;
      sub_used(slot.object_size);
      slot.state.store(static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_SWEPT), std::memory_order_release);
      slot.dead_oop.store(nullptr, std::memory_order_relaxed);
      ++drained_count;
      ++tail;
    } else if (cur_state == static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_REHABILITATED) ||
               cur_state == static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_SWEPT)) {
      slot.dead_oop.store(nullptr, std::memory_order_relaxed);
      slot.state.store(static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_EMPTY), std::memory_order_release);
      ++tail;
    } else {
      break; // Still in probation window
    }
  }

  _tail_cursor.store(tail, std::memory_order_release);
  _total_drained_bytes.fetch_add(bytes_acc, std::memory_order_relaxed);

  if (out_drained_bytes != nullptr) {
    *out_drained_bytes = bytes_acc;
  }
  return drained_count;
}

void RedjetVFSNode_Falline::purge_morgue() {
  for (size_t i = 0; i < MORGUE_CAPACITY; ++i) {
    _morgue_ring[i].dead_oop.store(nullptr, std::memory_order_relaxed);
    _morgue_ring[i].state.store(static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_EMPTY), std::memory_order_relaxed);
  }
  _head_cursor.store(0, std::memory_order_relaxed);
  _tail_cursor.store(0, std::memory_order_relaxed);
}

size_t RedjetVFSNode_Falline::active_quarantine_count() const noexcept {
  uint64_t head = _head_cursor.load(std::memory_order_relaxed);
  uint64_t tail = _tail_cursor.load(std::memory_order_relaxed);
  return (head > tail) ? static_cast<size_t>(head - tail) : 0;
}

void RedjetVFSNode_Falline::print_on(outputStream* st) const {
  st->print_cr("Redjet Runway Falline Pretrashing Node [%s]:", full_path());
  st->print_cr("  Quota: %zu KB, Used: %zu KB, Active Quarantined: %zu",
               quota() / 1024, used() / 1024, active_quarantine_count());
  st->print_cr("  Total Quarantined: " UINT64_FORMAT ", Condemned: " UINT64_FORMAT,
               total_quarantined(), total_condemned());
  st->print_cr("  Total Rehabilitated: " UINT64_FORMAT ", Drained Bytes: " UINT64_FORMAT,
               total_rehabilitated(), total_drained_bytes());
  st->print_cr("  Drops on Full: " UINT64_FORMAT, drops_on_full());
}
