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
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/api/redjetNativeSDK.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "logging/log.hpp"
#else
static bool RedjetGCUseDeadReferenceSignals = true;
#define log_info(...) if (false) printf
#endif

RedjetTailTraces g_redjet_tail_traces;

RedjetTailTraces::RedjetTailTraces() {
  _write_cursor.store(0, std::memory_order_relaxed);
  _read_cursor.store(0, std::memory_order_relaxed);
  _total_signals_received.store(0, std::memory_order_relaxed);
  _total_signals_drained.store(0, std::memory_order_relaxed);
  _total_false_positives_cancelled.store(0, std::memory_order_relaxed);
  _current_tailtraces_epoch.store(1, std::memory_order_relaxed);
  _active_clasp_count.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < RPSS_RING_CAPACITY; ++i) {
    _rpss_ring[i].dead_oop_address.store(0, std::memory_order_relaxed);
    _rpss_ring[i].thread_id = 0;
    _rpss_ring[i].region_id = 0;
    _rpss_ring[i].timestamp_ns = 0;
    _rpss_ring[i].probation_epoch = 0;
    _rpss_ring[i].phase.store(static_cast<uint8_t>(RedjetSignalPhase::PHASE_1_PROBATION), std::memory_order_relaxed);
    _rpss_ring[i].escape_detected.store(false, std::memory_order_relaxed);
  }

  for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
    _clasps[i].thread_id = 0;
    _clasps[i].state.store(static_cast<uint8_t>(RedjetClaspState::CLASP_UNATTACHED), std::memory_order_relaxed);
    _clasps[i].birth_timestamp_ns = 0;
    _clasps[i].detach_timestamp_ns = 0;
    _clasps[i].signals_emitted.store(0, std::memory_order_relaxed);
    _clasps[i].false_positives_filtered.store(0, std::memory_order_relaxed);
  }
}

bool RedjetTailTraces::is_dead_reference_signals_enabled() noexcept {
  return RedjetGCUseDeadReferenceSignals;
}

bool RedjetTailTraces::is_object_escape_bound(void* dead_obj, uint32_t thread_id) const noexcept {
  (void)thread_id;
  if (!dead_obj) return true;

  // 1. Check if pointer is currently held in active Native SDK live_ref registry
  if (g_redjet_native_registry.is_referenced(dead_obj)) {
    return true; // Escaped into native boundary!
  }

  // 2. Validate live header state if available
  auto* hdr = reinterpret_cast<HS_hotspot_live_header_t*>(dead_obj);
  if (hdr->hsMN == HS_LIVE_MAGIC) {
    if (hdr->opt_permitted == 0) {
      return true; // Warmup bypass / protected escape
    }
  }

  return false;
}

void RedjetTailTraces::register_thread_clasp(uint32_t thread_id) {
  if (!is_dead_reference_signals_enabled()) return;

  hs_ticket_guard_t guard(_clasp_lock);
  size_t slot = thread_id % MAX_TRACKED_THREADS;
  _clasps[slot].thread_id = thread_id;
  _clasps[slot].birth_timestamp_ns = sacculus_now_ns();
  _clasps[slot].state.store(static_cast<uint8_t>(RedjetClaspState::CLASP_ACTIVE), std::memory_order_release);
  _active_clasp_count.fetch_add(1, std::memory_order_relaxed);

  hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0, 0, 0, 0, 0, 0, "TailTraces:ClaspAttached");
}

void RedjetTailTraces::detach_thread_clasp(uint32_t thread_id) {
  if (!is_dead_reference_signals_enabled()) return;

  hs_ticket_guard_t guard(_clasp_lock);
  size_t slot = thread_id % MAX_TRACKED_THREADS;
  if (_clasps[slot].thread_id == thread_id) {
    _clasps[slot].detach_timestamp_ns = sacculus_now_ns();
    _clasps[slot].state.store(static_cast<uint8_t>(RedjetClaspState::CLASP_TERMINATED), std::memory_order_release);
    if (_active_clasp_count.load(std::memory_order_relaxed) > 0) {
      _active_clasp_count.fetch_sub(1, std::memory_order_relaxed);
    }
    hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0, 0, 0, 0, 0, 0, "TailTraces:ClaspDetached");
  }
}

bool RedjetTailTraces::is_clasp_active(uint32_t thread_id) const noexcept {
  size_t slot = thread_id % MAX_TRACKED_THREADS;
  if (_clasps[slot].thread_id == thread_id) {
    return _clasps[slot].state.load(std::memory_order_acquire) == static_cast<uint8_t>(RedjetClaspState::CLASP_ACTIVE);
  }
  return false;
}

void RedjetTailTraces::emit_dead_reference_signal(uint32_t thread_id, void* dead_obj, uint32_t region_id) {
  if (!is_dead_reference_signals_enabled() || dead_obj == nullptr) return;

  // False-Positive Protection: Check escape-bound before staging
  if (is_object_escape_bound(dead_obj, thread_id)) {
    size_t slot = thread_id % MAX_TRACKED_THREADS;
    _clasps[slot].false_positives_filtered.fetch_add(1, std::memory_order_relaxed);
    _total_false_positives_cancelled.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  uint64_t cur_epoch = _current_tailtraces_epoch.load(std::memory_order_relaxed);
  uint64_t slot_idx = _write_cursor.fetch_add(1, std::memory_order_acq_rel);
  RedjetClaspSignal& sig = _rpss_ring[slot_idx % RPSS_RING_CAPACITY];

  sig.thread_id = thread_id;
  sig.region_id = region_id;
  sig.timestamp_ns = sacculus_now_ns();
  sig.probation_epoch = cur_epoch;
  sig.escape_detected.store(false, std::memory_order_relaxed);
  sig.phase.store(static_cast<uint8_t>(RedjetSignalPhase::PHASE_1_PROBATION), std::memory_order_release);
  sig.dead_oop_address.store(reinterpret_cast<uint64_t>(dead_obj), std::memory_order_release);

  size_t c_slot = thread_id % MAX_TRACKED_THREADS;
  _clasps[c_slot].signals_emitted.fetch_add(1, std::memory_order_relaxed);
  _total_signals_received.fetch_add(1, std::memory_order_relaxed);
}

size_t RedjetTailTraces::advance_epoch_and_process_probation(uint64_t new_epoch) {
  _current_tailtraces_epoch.store(new_epoch, std::memory_order_release);

  uint64_t read_idx = _read_cursor.load(std::memory_order_acquire);
  uint64_t write_idx = _write_cursor.load(std::memory_order_acquire);
  size_t promoted_to_condemned = 0;

  for (uint64_t i = read_idx; i < write_idx; ++i) {
    RedjetClaspSignal& sig = _rpss_ring[i % RPSS_RING_CAPACITY];
    uint8_t p = sig.phase.load(std::memory_order_relaxed);

    if (p == static_cast<uint8_t>(RedjetSignalPhase::PHASE_1_PROBATION)) {
      if (sig.escape_detected.load(std::memory_order_acquire)) {
        sig.phase.store(static_cast<uint8_t>(RedjetSignalPhase::PHASE_CANCELLED), std::memory_order_release);
        sig.dead_oop_address.store(0, std::memory_order_release);
        _total_false_positives_cancelled.fetch_add(1, std::memory_order_relaxed);
      } else if (new_epoch >= sig.probation_epoch + DEFAULT_PROBATION_EPOCH_DELAY) {
        // Two-phase epoch delay elapsed without escape: promote to Phase 2 Condemnation
        sig.phase.store(static_cast<uint8_t>(RedjetSignalPhase::PHASE_2_CONDEMNATION), std::memory_order_release);
        promoted_to_condemned++;
      }
    }
  }
  return promoted_to_condemned;
}

bool RedjetTailTraces::cancel_signal_on_escape(void* obj_ptr) {
  if (!obj_ptr) return false;
  uint64_t target_addr = reinterpret_cast<uint64_t>(obj_ptr);

  uint64_t read_idx = _read_cursor.load(std::memory_order_acquire);
  uint64_t write_idx = _write_cursor.load(std::memory_order_acquire);

  for (uint64_t i = read_idx; i < write_idx; ++i) {
    RedjetClaspSignal& sig = _rpss_ring[i % RPSS_RING_CAPACITY];
    if (sig.dead_oop_address.load(std::memory_order_relaxed) == target_addr) {
      sig.escape_detected.store(true, std::memory_order_release);
      sig.phase.store(static_cast<uint8_t>(RedjetSignalPhase::PHASE_CANCELLED), std::memory_order_release);
      sig.dead_oop_address.store(0, std::memory_order_release);
      _total_false_positives_cancelled.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

size_t RedjetTailTraces::drain_signals(void (*reclaim_callback)(void* obj, uint32_t region_id)) {
  if (reclaim_callback == nullptr) return 0;

  uint64_t read_idx = _read_cursor.load(std::memory_order_acquire);
  uint64_t write_idx = _write_cursor.load(std::memory_order_acquire);
  size_t count = 0;

  while (read_idx < write_idx) {
    RedjetClaspSignal& sig = _rpss_ring[read_idx % RPSS_RING_CAPACITY];
    uint8_t p = sig.phase.load(std::memory_order_acquire);

    if (p == static_cast<uint8_t>(RedjetSignalPhase::PHASE_2_CONDEMNATION)) {
      uint64_t oop_addr = sig.dead_oop_address.exchange(0, std::memory_order_acq_rel);
      if (oop_addr != 0) {
        sig.phase.store(static_cast<uint8_t>(RedjetSignalPhase::PHASE_COMMITTED), std::memory_order_release);
        reclaim_callback(reinterpret_cast<void*>(oop_addr), sig.region_id);
        count++;
      }
      read_idx++;
    } else if (p == static_cast<uint8_t>(RedjetSignalPhase::PHASE_CANCELLED)) {
      sig.dead_oop_address.store(0, std::memory_order_relaxed);
      read_idx++;
    } else {
      // Unconfirmed Phase 1 probation signal: cannot drain past this slot yet
      break;
    }
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
