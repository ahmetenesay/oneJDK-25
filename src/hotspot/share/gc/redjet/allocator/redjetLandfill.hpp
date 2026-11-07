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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETLANDFILL_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETLANDFILL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// REDJET LANDFILL: Auxiliary Staging, Maturation Pit & Quarantine Morgue
// (PLAN.md Section 8.4, 9.4)
// - Accepts matured objects from Hotfield and survivor runways
// - Enforces 15-cycle tenure promotion barrier
// - Manages 1-2 cycle Falline quarantine morgue rehabilitation
// - Implements hole-plugging compaction to maintain high heap density
// - Integrates with Snatch/Rebate for zero-cost page returning to OS
// ============================================================================

struct alignas(64) RedjetLandfillTenureStats {
  std::atomic<uint64_t> generation_promotions[16];
  std::atomic<uint64_t> total_promoted_bytes{ 0 };
  std::atomic<uint64_t> total_rehabilitated_objects{ 0 };
  std::atomic<uint64_t> total_rehabilitated_bytes{ 0 };
  std::atomic<uint64_t> hole_plugging_merges{ 0 };
  std::atomic<uint64_t> compaction_cycles{ 0 };

  void reset() noexcept {
    for (int i = 0; i < 16; ++i) {
      generation_promotions[i].store(0, std::memory_order_relaxed);
    }
    total_promoted_bytes.store(0, std::memory_order_relaxed);
    total_rehabilitated_objects.store(0, std::memory_order_relaxed);
    total_rehabilitated_bytes.store(0, std::memory_order_relaxed);
    hole_plugging_merges.store(0, std::memory_order_relaxed);
    compaction_cycles.store(0, std::memory_order_relaxed);
  }
};

class RedjetLandfill {
public:
  static constexpr uint8_t TENURE_THRESHOLD_EPOCHS = 15;
  static constexpr size_t  MAX_REHAB_QUARANTINE    = 8192;

private:
  hs_trash_pit_t            _pit;
  redjet_header_pane_t      _pane;
  RedjetLandfillTenureStats _tenure_stats;
  hs_ticket_lock_t          _landfill_lock;

  // Rehabilitation quarantine tracking
  void*                     _quarantine_ring[MAX_REHAB_QUARANTINE];
  std::atomic<size_t>       _quarantine_count{ 0 };
  std::atomic<uint64_t>     _current_epoch{ 0 };

public:
  RedjetLandfill();
  ~RedjetLandfill();

  bool init(size_t initial = RJ_LANDFILL_INIT, bool advance_payment = false);
  bool accept(HS_hotspot_live_header_t* live,
              hs_death_reason_e reason,
              redjet_hotlog_t* hotlog = nullptr,
              uint64_t sweep_no = 0,
              uint64_t evac_duration_ns = 0);

  // Quarantine rehabilitation pipeline (1-2 cycle revival)
  bool rehabilitate_object(void* live_addr, uint64_t current_sweep);

  // Compaction and hole-plugging
  size_t compact_and_plug_holes(size_t max_bytes_to_compact);

  // Critical saturation detector
  bool is_critical(float critical_ratio = RJ_LANDFILL_CRIT) const;

  // Evacuation & sweeping
  void evacuate(redjet_hotlog_t* hotlog = nullptr, uint64_t sweep_no = 0);
  void destroy();

  // Accessors
  hs_trash_pit_t& pit() noexcept { return _pit; }
  const hs_trash_pit_t& pit() const noexcept { return _pit; }
  redjet_header_pane_t& pane() noexcept { return _pane; }
  const redjet_header_pane_t& pane() const noexcept { return _pane; }

  const RedjetLandfillTenureStats& tenure_stats() const noexcept { return _tenure_stats; }
  uint64_t current_epoch() const noexcept { return _current_epoch.load(std::memory_order_relaxed); }
  size_t quarantine_count() const noexcept { return _quarantine_count.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

using hsLandfill          = RedjetLandfill;
using sacculus_landfill_t = RedjetLandfill;
using redjet_landfill_t   = RedjetLandfill;
using rj_landfill_t       = RedjetLandfill;

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETLANDFILL_HPP
