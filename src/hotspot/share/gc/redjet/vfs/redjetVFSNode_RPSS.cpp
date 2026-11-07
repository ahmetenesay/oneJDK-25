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

#include "gc/redjet/vfs/redjetVFSNode_RPSS.hpp"
#include <algorithm>

RedjetVFSNode_RPSS::RedjetVFSNode_RPSS(const char* name,
                                       const char* full_path,
                                       RedjetVFSNode* parent,
                                       size_t quota_bytes) :
  RedjetVFSNode(name, full_path, parent, quota_bytes) {
  purge();
}

RedjetVFSNode_RPSS::~RedjetVFSNode_RPSS() {
  purge();
}

bool RedjetVFSNode_RPSS::enqueue_dead_ref(uintptr_t dead_oop, uint32_t thread_id, uint32_t region_id, size_t object_size) {
  if (dead_oop == 0) {
    return false;
  }

  uint64_t head = _head.load(std::memory_order_relaxed);
  do {
    uint64_t tail = _tail.load(std::memory_order_acquire);
    if (head - tail >= RING_CAPACITY) {
      _total_drops_on_full.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  } while (!_head.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

  size_t idx = static_cast<size_t>(head & RING_MASK);

  RedjetRPSSHarbingerEntry& entry = _ring[idx];
  entry.dead_oop.store(dead_oop, std::memory_order_relaxed);
  entry.thread_id = thread_id;
  entry.region_id = region_id;
  entry.object_size = object_size;
  entry.enqueue_timestamp_ns = sacculus_rdtsc();
  entry.probation_epoch = _current_epoch.load(std::memory_order_relaxed);
  entry.resurrected.store(false, std::memory_order_relaxed);
  entry.state.store(static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_PROBATION), std::memory_order_release);

  _total_enqueued.fetch_add(1, std::memory_order_relaxed);
  add_used(sizeof(RedjetRPSSHarbingerEntry) + object_size);
  return true;
}

bool RedjetVFSNode_RPSS::notify_object_escaped(uintptr_t oop_addr) {
  if (oop_addr == 0) {
    return false;
  }

  uint64_t tail = _tail.load(std::memory_order_acquire);
  uint64_t head = _head.load(std::memory_order_acquire);

  for (uint64_t cur = tail; cur < head; ++cur) {
    size_t idx = static_cast<size_t>(cur & RING_MASK);
    RedjetRPSSHarbingerEntry& entry = _ring[idx];

    if (entry.dead_oop.load(std::memory_order_relaxed) == oop_addr) {
      uint8_t expected = static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_PROBATION);
      if (entry.state.compare_exchange_strong(expected, static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_CANCELLED),
                                              std::memory_order_acq_rel)) {
        entry.resurrected.store(true, std::memory_order_relaxed);
        _total_cancelled.fetch_add(1, std::memory_order_relaxed);
        sub_used(sizeof(RedjetRPSSHarbingerEntry) + entry.object_size);
        return true;
      }
    }
  }

  return false;
}

void RedjetVFSNode_RPSS::advance_epoch() {
  uint64_t new_epoch = _current_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;

  uint64_t tail = _tail.load(std::memory_order_acquire);
  uint64_t head = _head.load(std::memory_order_acquire);

  for (uint64_t cur = tail; cur < head; ++cur) {
    size_t idx = static_cast<size_t>(cur & RING_MASK);
    RedjetRPSSHarbingerEntry& entry = _ring[idx];

    if (entry.state.load(std::memory_order_acquire) == static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_PROBATION)) {
      if (new_epoch - entry.probation_epoch >= PROBATION_DELAY_EPOCHS) {
        if (!entry.resurrected.load(std::memory_order_relaxed)) {
          entry.state.store(static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_CONDEMNED), std::memory_order_release);
        }
      }
    }
  }
}

RedjetRPSSDrainStats RedjetVFSNode_RPSS::drain_condemned_batch(uintptr_t* out_dead_oops, size_t max_count) {
  RedjetRPSSDrainStats stats;
  if (out_dead_oops == nullptr || max_count == 0) {
    return stats;
  }

  uint64_t tail = _tail.load(std::memory_order_acquire);
  uint64_t head = _head.load(std::memory_order_acquire);

  while (tail < head && stats.objects_condemned < max_count) {
    size_t idx = static_cast<size_t>(tail & RING_MASK);
    RedjetRPSSHarbingerEntry& entry = _ring[idx];
    uint8_t state = entry.state.load(std::memory_order_acquire);

    if (state == static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_CONDEMNED)) {
      out_dead_oops[stats.objects_condemned++] = entry.dead_oop.load(std::memory_order_relaxed);
      stats.reclaimed_bytes += entry.object_size;
      stats.signals_drained++;

      entry.state.store(static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_RECLAIMED), std::memory_order_release);
      sub_used(sizeof(RedjetRPSSHarbingerEntry) + entry.object_size);
      _total_committed.fetch_add(1, std::memory_order_relaxed);
      _total_reclaimed_bytes.fetch_add(entry.object_size, std::memory_order_relaxed);
      tail++;
    } else if (state == static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_CANCELLED) ||
               state == static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SIGNAL_RECLAIMED)) {
      stats.false_positives_cancelled++;
      stats.signals_drained++;
      tail++;
    } else {
      // Unsettled probation slot encountered at head of queue
      break;
    }
  }

  _tail.store(tail, std::memory_order_release);
  return stats;
}

size_t RedjetVFSNode_RPSS::active_signals() const noexcept {
  uint64_t head = _head.load(std::memory_order_relaxed);
  uint64_t tail = _tail.load(std::memory_order_relaxed);
  return (head >= tail) ? static_cast<size_t>(head - tail) : 0;
}

void RedjetVFSNode_RPSS::purge() {
  for (size_t i = 0; i < RING_CAPACITY; ++i) {
    _ring[i].dead_oop.store(0, std::memory_order_relaxed);
    _ring[i].thread_id = 0;
    _ring[i].region_id = 0;
    _ring[i].object_size = 0;
    _ring[i].enqueue_timestamp_ns = 0;
    _ring[i].probation_epoch = 0;
    _ring[i].state.store(static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SLOT_EMPTY), std::memory_order_relaxed);
    _ring[i].resurrected.store(false, std::memory_order_relaxed);
  }
  _head.store(0, std::memory_order_relaxed);
  _tail.store(0, std::memory_order_relaxed);
  _used_bytes.store(0, std::memory_order_relaxed);
}
