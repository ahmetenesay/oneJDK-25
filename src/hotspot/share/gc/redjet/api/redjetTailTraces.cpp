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

#include "gc/redjet/api/redjetTailTraces.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

RedjetTailTraces g_redjet_tail_traces;

RedjetTailTraces::RedjetTailTraces() {
  _write_cursor.store(0, std::memory_order_relaxed);
  _read_cursor.store(0, std::memory_order_relaxed);
  _total_signals_received.store(0, std::memory_order_relaxed);
  _total_signals_drained.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < RPSS_RING_CAPACITY; ++i) {
    _rpss_ring[i].dead_oop_address.store(0, std::memory_order_relaxed);
    _rpss_ring[i].thread_id = 0;
    _rpss_ring[i].region_id = 0;
    _rpss_ring[i].timestamp_ns = 0;
  }
}

void RedjetTailTraces::register_thread_clasp(uint32_t thread_id) {
  // Clasp attachment marker on thread birth
  hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0, 0, 0, 0, 0, 0, "TailTraces:ClaspAttached");
}

void RedjetTailTraces::emit_dead_reference_signal(uint32_t thread_id, void* dead_obj, uint32_t region_id) {
  if (dead_obj == nullptr) return;

  uint64_t slot_idx = _write_cursor.fetch_add(1, std::memory_order_acq_rel);
  RedjetClaspSignal& sig = _rpss_ring[slot_idx % RPSS_RING_CAPACITY];

  sig.thread_id = thread_id;
  sig.region_id = region_id;
  sig.timestamp_ns = sacculus_now_ns();
  sig.dead_oop_address.store(reinterpret_cast<uint64_t>(dead_obj), std::memory_order_release);

  _total_signals_received.fetch_add(1, std::memory_order_relaxed);
}

size_t RedjetTailTraces::drain_signals(void (*reclaim_callback)(void* obj, uint32_t region_id)) {
  if (reclaim_callback == nullptr) return 0;

  uint64_t read_idx = _read_cursor.load(std::memory_order_acquire);
  uint64_t write_idx = _write_cursor.load(std::memory_order_acquire);
  size_t count = 0;

  while (read_idx < write_idx) {
    RedjetClaspSignal& sig = _rpss_ring[read_idx % RPSS_RING_CAPACITY];
    uint64_t oop_addr = sig.dead_oop_address.exchange(0, std::memory_order_acq_rel);
    if (oop_addr != 0) {
      reclaim_callback(reinterpret_cast<void*>(oop_addr), sig.region_id);
      count++;
    }
    read_idx++;
  }

  _read_cursor.store(read_idx, std::memory_order_release);
  _total_signals_drained.fetch_add(count, std::memory_order_relaxed);
  return count;
}

uint64_t RedjetTailTraces::pending_signals() const {
  uint64_t w = _write_cursor.load(std::memory_order_relaxed);
  uint64_t r = _read_cursor.load(std::memory_order_relaxed);
  return w > r ? (w - r) : 0;
}
