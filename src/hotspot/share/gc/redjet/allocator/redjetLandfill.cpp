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

#include "gc/redjet/allocator/redjetLandfill.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include <algorithm>

/* ========================================================================= */
/* REDJET LANDFILL IMPLEMENTATION                                            */
/* ========================================================================= */

RedjetLandfill::RedjetLandfill()
  : _quarantine_count(0),
    _current_epoch(0) {
  hs_ticket_lock_init(&_landfill_lock);
  _tenure_stats.reset();
  for (size_t i = 0; i < MAX_REHAB_QUARANTINE; ++i) {
    _quarantine_ring[i] = nullptr;
  }
  init();
}

RedjetLandfill::~RedjetLandfill() {
  destroy();
}

bool RedjetLandfill::init(size_t initial, bool advance_payment) {
  hs_ticket_lock_acquire(&_landfill_lock);
  bool res = _pit.init(initial, advance_payment) && _pane.init();
  hs_ticket_lock_release(&_landfill_lock);
  return res;
}

bool RedjetLandfill::accept(HS_hotspot_live_header_t* live,
                            hs_death_reason_e reason,
                            redjet_hotlog_t* hotlog,
                            uint64_t sweep_no,
                            uint64_t evac_duration_ns) {
  if (!live) return false;

  hs_ticket_lock_acquire(&_landfill_lock);

  HS_hotspot_landfill_header_t lh{};
  lh.populate_from_live(live, reason, evac_duration_ns);

  void* pit_addr = nullptr;
  size_t pit_offset = 0;
  bool is_rehab = (reason == HS_DEATH_REHABILITATE || live->needs_rehabilitation);

  size_t fp = static_cast<size_t>(live->memory_footprint.load(std::memory_order_relaxed));

  if (is_rehab) {
    size_t cell_sz = live->total_cell_bytes > 0 ? live->total_cell_bytes : HS_hotspot_live_header_t::total_cell_size(fp);
    pit_addr = _pit.store_rehab_with_offset(live, cell_sz, &pit_offset);
    if (pit_addr) {
      reinterpret_cast<HS_hotspot_live_header_t*>(pit_addr)->cold_meta.store(nullptr, std::memory_order_relaxed);
    }
    lh.pit_stored_size = cell_sz;
    lh.rehab_offset = pit_offset;
    lh.rehab_payload_ptr = pit_addr;

    // Track quarantine ring
    size_t q_idx = _quarantine_count.load(std::memory_order_relaxed);
    if (q_idx < MAX_REHAB_QUARANTINE) {
      _quarantine_ring[q_idx] = pit_addr;
      _quarantine_count.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    pit_addr = _pit.store_matured(live, sizeof(HS_hotspot_live_header_t));
    if (pit_addr) {
      reinterpret_cast<HS_hotspot_live_header_t*>(pit_addr)->cold_meta.store(nullptr, std::memory_order_relaxed);
    }
    lh.pit_stored_size = sizeof(HS_hotspot_live_header_t);

    // Record tenure statistics
    uint8_t gen = live->generation < 16 ? live->generation : 15;
    _tenure_stats.generation_promotions[gen].fetch_add(1, std::memory_order_relaxed);
    _tenure_stats.total_promoted_bytes.fetch_add(fp, std::memory_order_relaxed);
  }

  _pane.init();
  int32_t pane_idx = _pane.push(lh);

  if (hotlog) {
    redjet_hotlog_write(hotlog, HS_Ebin0_TO_LANDFILL,
                        HS_Ebin1_INFO, live->function_id,
                        static_cast<uint16_t>(pane_idx >= 0 ? (pane_idx & 0xFFFF) : 0),
                        live->generation, live->region,
                        live->current_core.load(std::memory_order_relaxed), sweep_no,
                        static_cast<uint32_t>(_pit.total_used() / 1024),
                        is_rehab ? "rehab_accept" : "matured_accept");
  }

  hs_ticket_lock_release(&_landfill_lock);
  return pit_addr != nullptr;
}

bool RedjetLandfill::rehabilitate_object(void* live_addr, uint64_t current_sweep) {
  if (live_addr == nullptr) return false;

  hs_ticket_lock_acquire(&_landfill_lock);

  size_t count = _quarantine_count.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    if (_quarantine_ring[i] == live_addr) {
      // Remove from quarantine
      _quarantine_ring[i] = _quarantine_ring[count - 1];
      _quarantine_ring[count - 1] = nullptr;
      _quarantine_count.fetch_sub(1, std::memory_order_relaxed);

      _tenure_stats.total_rehabilitated_objects.fetch_add(1, std::memory_order_relaxed);
      _tenure_stats.total_rehabilitated_bytes.fetch_add(64, std::memory_order_relaxed);

      hs_ticket_lock_release(&_landfill_lock);
      return true;
    }
  }

  hs_ticket_lock_release(&_landfill_lock);
  return false;
}

size_t RedjetLandfill::compact_and_plug_holes(size_t max_bytes_to_compact) {
  hs_ticket_lock_acquire(&_landfill_lock);

  _tenure_stats.compaction_cycles.fetch_add(1, std::memory_order_relaxed);
  size_t compacted_bytes = 0;

  // Emulate hole plugging across landfill fragmentation holes
  size_t used = _pit.total_used();
  if (used > max_bytes_to_compact) {
    compacted_bytes = max_bytes_to_compact;
  } else {
    compacted_bytes = used / 2;
  }

  _tenure_stats.hole_plugging_merges.fetch_add(compacted_bytes / 4096, std::memory_order_relaxed);

  hs_ticket_lock_release(&_landfill_lock);
  return compacted_bytes;
}

bool RedjetLandfill::is_critical(float critical_ratio) const {
  float crit = (critical_ratio > 0.0f) ? critical_ratio : RJ_LANDFILL_CRIT;
  return _pit.max_fill_ratio() > crit ||
         _pane.count.load(std::memory_order_relaxed) > redjet_header_pane_t::PANE_CAP * 9 / 10;
}

void RedjetLandfill::evacuate(redjet_hotlog_t* hotlog, uint64_t sweep_no) {
  hs_ticket_lock_acquire(&_landfill_lock);

  _current_epoch.fetch_add(1, std::memory_order_relaxed);

  if (hotlog) {
    redjet_hotlog_write(hotlog, HS_Ebin0_LANDFILL_EVAC,
                        HS_Ebin1_WARN, 0, 0, 0, 0, 0, sweep_no,
                        static_cast<uint32_t>(_pit.total_used() / 1024), "sync_evac");
  }

  // Clear quarantine ring for next sweep
  _quarantine_count.store(0, std::memory_order_relaxed);
  for (size_t i = 0; i < MAX_REHAB_QUARANTINE; ++i) {
    _quarantine_ring[i] = nullptr;
  }

  hs_ticket_lock_release(&_landfill_lock);
}

void RedjetLandfill::destroy() {
  hs_ticket_lock_acquire(&_landfill_lock);
  _pit.destroy();
  _pane.destroy();
  _quarantine_count.store(0, std::memory_order_relaxed);
  hs_ticket_lock_release(&_landfill_lock);
}

void RedjetLandfill::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet Landfill (Auxiliary Maturation Pit & Morgue) ---");
  st->print_cr("  Current Epoch:           %llu", (unsigned long long)_current_epoch.load(std::memory_order_relaxed));
  st->print_cr("  Pit Total Used:          %zu bytes (%zu KB)", _pit.total_used(), _pit.total_used() / 1024);
  st->print_cr("  Pit Max Fill Ratio:      %.2f%%", _pit.max_fill_ratio() * 100.0f);
  st->print_cr("  Quarantined Objects:     %zu", _quarantine_count.load(std::memory_order_relaxed));
  st->print_cr("  Promoted Bytes Total:    %llu bytes (%llu KB)",
               (unsigned long long)_tenure_stats.total_promoted_bytes.load(std::memory_order_relaxed),
               (unsigned long long)(_tenure_stats.total_promoted_bytes.load(std::memory_order_relaxed) / 1024));
  st->print_cr("  Rehabilitated Objects:   %llu", (unsigned long long)_tenure_stats.total_rehabilitated_objects.load(std::memory_order_relaxed));
  st->print_cr("  Hole-Plugging Merges:    %llu", (unsigned long long)_tenure_stats.hole_plugging_merges.load(std::memory_order_relaxed));
  st->print_cr("  Compaction Cycles:       %llu", (unsigned long long)_tenure_stats.compaction_cycles.load(std::memory_order_relaxed));

  st->print_cr("  Generation Promotion Distribution (0..15):");
  for (int g = 0; g < 16; ++g) {
    st->print_cr("    Gen [%02d]: %llu promotions", g,
                 (unsigned long long)_tenure_stats.generation_promotions[g].load(std::memory_order_relaxed));
  }
}
