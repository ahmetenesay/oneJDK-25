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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "gc/redjet/rjnrml/redjetFlags.hpp"
#include "gc/redjet/rjnrml/redjetGC.hpp"
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/redjetHardware.hpp"
#include "gc/redjet/rjnrml/redjetHeap.hpp"
#include "gc/redjet/rjspcf/redjetMotherline.hpp"
#include "gc/redjet/rjnrml/redjetOSAffinity.hpp"
#include "gc/redjet/rjnrml/redjetPagefile.hpp"
#include "gc/redjet/rjspcf/redjetPitstop.hpp"
#include "gc/redjet/rjnrml/redjetPointCoordinator.hpp"
#include "gc/redjet/rjspcf/redjetPuzzleArena.hpp"
#include "gc/redjet/rjnrml/redjetRunway.hpp"
#include "gc/redjet/rjnrml/redjetThreadLocalData.hpp"
#include "gc/redjet/rjnrml/redjet_globals.hpp"
#include "gc/redjet/rjspcf/self_advance/redjetCoach.hpp"
#include "gc/redjet/rjspcf/self_advance/redjetSelfAdvanceHooks.hpp"
#include "logging/log.hpp"
#include "memory/universe.hpp"
#include "runtime/globals.hpp"
#include "runtime/javaThread.hpp"

extern "C" {
  alignas(64) volatile uint32_t g_redjet_parkpoint_active_flag       = 0;
  alignas(64) volatile uint32_t g_redjet_parkpoint_gate_flag         = 0;
  alignas(64) volatile uint32_t g_redjet_dockpoint_pressure_level     = 0;

  // Aliases for compatibility
  alignas(64) volatile uint32_t g_redjet_safepoint_active_flag       = 0;
  alignas(64) volatile uint32_t g_redjet_pausepoint_gate_flag        = 0;
  alignas(64) volatile uint32_t g_redjet_controlpoint_pressure_level = 0;
}

namespace {

struct alignas(64) RedjetParkSlot {
  std::atomic<uintptr_t> target_addr{ 0 };
  std::atomic<uint32_t>  hold_count{ 0 };
  std::atomic<uint32_t>  generation{ 0 };
};

struct alignas(64) RedjetMethodHeatSlot {
  std::atomic<uintptr_t> method_key{ 0 };
  std::atomic<uint32_t>  accumulated_heat{ 0 };
  std::atomic<uint32_t>  promotion_epoch{ 0 };
};

struct alignas(64) RedjetCoordinatorState {
  std::atomic<bool>     initialized{ false };

  // 1. Parkpoint state (Safepoint STW + Pausepoint Pitstop bay yielding)
  alignas(64) std::atomic<uint32_t> parkpoint_state{ 0 };
  alignas(64) std::atomic<uint32_t> parkpoint_parked_threads{ 0 };
  alignas(64) std::atomic<uint32_t> active_in_flight_docks{ 0 };
  alignas(64) std::atomic<uint64_t> parkpoint_polls{ 0 };
  alignas(64) std::atomic<uint64_t> parkpoint_stalls{ 0 };

  alignas(64) RedjetParkSlot        park_slots[RedjetPointCoordinator::MAX_PARKPOINT_SLOTS];
  alignas(64) std::atomic<uint32_t> park_active_refcount{ 0 };
  alignas(64) std::atomic<uint32_t> park_epoch{ 1 };
  alignas(64) std::atomic<uint64_t> parkpoint_gate_hits{ 0 };
  alignas(64) std::atomic<uint64_t> parkpoint_targeted_yields{ 0 };

  // 2. Dockpoint state (Hookpoint object capture/pin/canary + Controlpoint heap pacing/paging)
  alignas(64) std::atomic<uint64_t> dockpoint_triggers{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_pins{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_canary_checks{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_canary_faults{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_puzzler_promotions{ 0 };

  alignas(64) std::atomic<uint64_t> dockpoint_checks{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_bytes_validated{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_pacing_stalls{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_falline_drains{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_auto_pageins{ 0 };
  alignas(64) std::atomic<uint64_t> dockpoint_host_gc_requests{ 0 };
  alignas(64) std::atomic<uint64_t> c1_heat_reports{ 0 };

  static const size_t HEAT_TABLE_SIZE = 256;
  alignas(64) RedjetMethodHeatSlot  heat_table[HEAT_TABLE_SIZE];

  RedjetDockpointHostHookFn         host_hook{ nullptr };
  void*                             host_hook_user_data{ nullptr };
};

static RedjetCoordinatorState g_coord;

static inline void sync_global_flags() {
  g_redjet_safepoint_active_flag        = g_redjet_parkpoint_active_flag;
  g_redjet_pausepoint_gate_flag         = g_redjet_parkpoint_gate_flag;
  g_redjet_controlpoint_pressure_level  = g_redjet_dockpoint_pressure_level;
}

static inline uint64_t get_host_tsc() {
  return RedjetOSAffinity::read_fast_cycles();
}

static inline void backoff_smt_pause(uint32_t spin_iters) {
  RedjetOSAffinity::smt_spin_pause(spin_iters);
}

} // namespace

void RedjetPointCoordinator::initialize() {
  if (g_coord.initialized.load(std::memory_order_acquire)) {
    return;
  }

  for (uint32_t i = 0; i < MAX_PARKPOINT_SLOTS; ++i) {
    g_coord.park_slots[i].target_addr.store(0, std::memory_order_relaxed);
    g_coord.park_slots[i].hold_count.store(0, std::memory_order_relaxed);
    g_coord.park_slots[i].generation.store(0, std::memory_order_relaxed);
  }

  for (size_t i = 0; i < RedjetCoordinatorState::HEAT_TABLE_SIZE; ++i) {
    g_coord.heat_table[i].method_key.store(0, std::memory_order_relaxed);
    g_coord.heat_table[i].accumulated_heat.store(0, std::memory_order_relaxed);
    g_coord.heat_table[i].promotion_epoch.store(0, std::memory_order_relaxed);
  }

  g_redjet_parkpoint_active_flag   = 0;
  g_redjet_parkpoint_gate_flag     = 0;
  g_redjet_dockpoint_pressure_level = 0;
  sync_global_flags();

  RedjetOSAffinity::initialize_topology();

  g_coord.initialized.store(true, std::memory_order_release);
  log_info(gc)("Redjet 2-Point Coordination Engine initialized (Parkpoint & Dockpoint).");
}

void RedjetPointCoordinator::shutdown() {
  disarm_parkpoint();
  for (uint32_t i = 0; i < MAX_PARKPOINT_SLOTS; ++i) {
    g_coord.park_slots[i].target_addr.store(0, std::memory_order_release);
    g_coord.park_slots[i].hold_count.store(0, std::memory_order_release);
  }
  g_redjet_parkpoint_gate_flag = 0;
  sync_global_flags();
  g_coord.initialized.store(false, std::memory_order_release);
}

/* ========================================================================= */
/* 1. PARKPOINT (SAFEPOINT STW + PAUSEPOINT PITSTOP BAY YIELDING)            */
/* ========================================================================= */

void RedjetPointCoordinator::arm_parkpoint() {
  g_coord.parkpoint_state.store(1, std::memory_order_release);
  g_redjet_parkpoint_active_flag = 1;
  sync_global_flags();
  g_coord.park_epoch.fetch_add(1, std::memory_order_acq_rel);
}

void RedjetPointCoordinator::disarm_parkpoint() {
  g_coord.parkpoint_state.store(0, std::memory_order_release);
  g_redjet_parkpoint_active_flag = 0;
  if (g_coord.park_active_refcount.load(std::memory_order_acquire) == 0) {
    g_redjet_parkpoint_gate_flag = 0;
  }
  sync_global_flags();
}

bool RedjetPointCoordinator::is_parkpoint_active() {
  return (g_redjet_parkpoint_active_flag != 0) || (g_redjet_parkpoint_gate_flag != 0);
}

void RedjetPointCoordinator::parkpoint_poll(Thread* thread, void* target_slot) {
  g_coord.parkpoint_polls.fetch_add(1, std::memory_order_relaxed);

  // 1. Check targeted object parking (Pitstop bay evolution)
  if (target_slot != nullptr && g_redjet_parkpoint_gate_flag != 0) {
    if (is_object_parked(target_slot)) {
      g_coord.parkpoint_gate_hits.fetch_add(1, std::memory_order_relaxed);
      const uint32_t spin_limit = static_cast<uint32_t>(RedjetPausepointSpinCount);
      uint32_t iters = 0;
      while (is_object_parked(target_slot) && iters < spin_limit) {
        backoff_smt_pause(1);
        ++iters;
      }
      if (is_object_parked(target_slot)) {
        g_coord.parkpoint_targeted_yields.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
      }
    }
  }

  // 2. Check global STW parkpoint
  if (g_redjet_parkpoint_active_flag == 0) {
    return;
  }

  g_coord.parkpoint_stalls.fetch_add(1, std::memory_order_relaxed);
  g_coord.parkpoint_parked_threads.fetch_add(1, std::memory_order_relaxed);

  if (thread != nullptr && thread->is_Java_thread()) {
    RedjetThreadLocalData::set_parkpoint_active(JavaThread::cast(thread), true);
  }

  const uint32_t spin_count = static_cast<uint32_t>(RedjetPausepointSpinCount);
  uint32_t spins = 0;
  while (g_redjet_parkpoint_active_flag != 0 && spins < spin_count) {
    backoff_smt_pause(4);
    spins += 4;
  }

  while (g_redjet_parkpoint_active_flag != 0) {
    std::this_thread::yield();
  }

  if (thread != nullptr && thread->is_Java_thread()) {
    RedjetThreadLocalData::set_parkpoint_active(JavaThread::cast(thread), false);
  }
  g_coord.parkpoint_parked_threads.fetch_sub(1, std::memory_order_relaxed);
}

void RedjetPointCoordinator::request_object_park(void* target_slot) {
  if (target_slot == nullptr) return;
  const uintptr_t target_key = reinterpret_cast<uintptr_t>(target_slot);

  for (uint32_t i = 0; i < MAX_PARKPOINT_SLOTS; ++i) {
    uintptr_t expected = 0;
    if (g_coord.park_slots[i].target_addr.compare_exchange_strong(expected, target_key,
                                                                   std::memory_order_acq_rel)) {
      g_coord.park_slots[i].hold_count.store(1, std::memory_order_release);
      g_coord.park_slots[i].generation.fetch_add(1, std::memory_order_relaxed);
      g_coord.park_active_refcount.fetch_add(1, std::memory_order_acq_rel);
      g_redjet_parkpoint_gate_flag = 1;
      sync_global_flags();
      return;
    } else if (expected == target_key) {
      g_coord.park_slots[i].hold_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  // Table saturated: engage global parkpoint gate as fallback
  g_coord.park_active_refcount.fetch_add(1, std::memory_order_acq_rel);
  g_redjet_parkpoint_gate_flag = 1;
  sync_global_flags();
}

void RedjetPointCoordinator::release_object_park(void* target_slot) {
  if (target_slot == nullptr) return;
  const uintptr_t target_key = reinterpret_cast<uintptr_t>(target_slot);

  for (uint32_t i = 0; i < MAX_PARKPOINT_SLOTS; ++i) {
    if (g_coord.park_slots[i].target_addr.load(std::memory_order_acquire) == target_key) {
      uint32_t prev_holds = g_coord.park_slots[i].hold_count.fetch_sub(1, std::memory_order_acq_rel);
      if (prev_holds <= 1) {
        g_coord.park_slots[i].target_addr.store(0, std::memory_order_release);
        uint32_t remaining = g_coord.park_active_refcount.fetch_sub(1, std::memory_order_acq_rel);
        if (remaining <= 1) {
          g_redjet_parkpoint_gate_flag = 0;
          sync_global_flags();
        }
      }
      return;
    }
  }

  uint32_t rem = g_coord.park_active_refcount.fetch_sub(1, std::memory_order_acq_rel);
  if (rem <= 1) {
    g_redjet_parkpoint_gate_flag = 0;
    sync_global_flags();
  }
}

bool RedjetPointCoordinator::is_object_parked(const void* target_slot) {
  if (target_slot == nullptr || g_redjet_parkpoint_gate_flag == 0) return false;
  const uintptr_t key = reinterpret_cast<uintptr_t>(target_slot);
  for (uint32_t i = 0; i < MAX_PARKPOINT_SLOTS; ++i) {
    if (g_coord.park_slots[i].target_addr.load(std::memory_order_acquire) == key &&
        g_coord.park_slots[i].hold_count.load(std::memory_order_relaxed) > 0) {
      return true;
    }
  }
  return false;
}

/* ========================================================================= */
/* 2. DOCKPOINT (HOOKPOINT OBJECT PIN/CANARY + CONTROLPOINT HEAP PACING)      */
/* ========================================================================= */

void RedjetPointCoordinator::dockpoint_enter(Thread* thread, void* obj_or_hdr, uint32_t fn_id) {
  g_coord.dockpoint_triggers.fetch_add(1, std::memory_order_relaxed);
  if (obj_or_hdr == nullptr) return;

  auto* hdr = reinterpret_cast<HS_custominiVm_live_header_t*>(obj_or_hdr);
  if (thread != nullptr && thread->is_Java_thread()) {
    JavaThread* jt = JavaThread::cast(thread);
    RedjetThreadLocalData::add_pinned_hdr(jt, hdr);
    RedjetThreadLocalData::set_active_dock_hdr(jt, hdr);
    RedjetThreadLocalData::set_dock_enter_tsc(jt, get_host_tsc());
    RedjetThreadLocalData::set_dockpoint_active(jt, true);
    g_coord.dockpoint_pins.fetch_add(1, std::memory_order_relaxed);
  }

  g_coord.dockpoint_canary_checks.fetch_add(1, std::memory_order_relaxed);
  if (hdr->memory_footprint.load(std::memory_order_relaxed) > 0) {
    if (!hdr->isTailCanaryValid()) {
      g_coord.dockpoint_canary_faults.fetch_add(1, std::memory_order_relaxed);
      hdr->writeTailCanary();
    }
  }

  uint16_t sim_idx = 0;
  bool is_fledgling = false;
  inspect_and_verify_object(obj_or_hdr, &sim_idx, &is_fledgling);
}

void RedjetPointCoordinator::dockpoint_exit(Thread* thread, void* obj_or_hdr, uint32_t fn_id) {
  if (obj_or_hdr == nullptr) return;
  auto* hdr = reinterpret_cast<HS_custominiVm_live_header_t*>(obj_or_hdr);

  if (thread != nullptr && thread->is_Java_thread()) {
    JavaThread* jt = JavaThread::cast(thread);
    uint64_t enter_tsc = RedjetThreadLocalData::dock_enter_tsc(jt);
    uint64_t exit_tsc  = get_host_tsc();
    uint64_t delta     = (exit_tsc > enter_tsc) ? (exit_tsc - enter_tsc) : 1ULL;

    hdr->optimization_duration_cycles.fetch_add(delta, std::memory_order_relaxed);
    RedjetThreadLocalData::remove_pinned_hdr(jt, hdr);
    RedjetThreadLocalData::set_active_dock_hdr(jt, nullptr);
    RedjetThreadLocalData::set_dockpoint_active(jt, false);
  }
}

bool RedjetPointCoordinator::inspect_and_verify_object(void* obj_or_hdr,
                                                      uint16_t* out_active_simd_idx,
                                                      bool* out_is_fledgling) {
  if (obj_or_hdr == nullptr) return false;
  auto* hdr = reinterpret_cast<HS_custominiVm_live_header_t*>(obj_or_hdr);

  if (out_active_simd_idx != nullptr) {
    *out_active_simd_idx = hdr->active_simd_candidate_idx;
  }
  const bool is_fledgling = (hdr->has_optimized.load(std::memory_order_acquire) != 0u);
  if (out_is_fledgling != nullptr) {
    *out_is_fledgling = is_fledgling;
  }
  return true;
}

uint32_t RedjetPointCoordinator::dockpoint_check(Thread* thread, size_t alloc_bytes, void* cold_target_hdr) {
  g_coord.dockpoint_checks.fetch_add(1, std::memory_order_relaxed);
  g_coord.dockpoint_bytes_validated.fetch_add(alloc_bytes, std::memory_order_relaxed);

  RedjetGC* gc = RedjetGC::gc();
  if (gc == nullptr || !RedjetControlpointPacing) {
    return REDJET_DOCK_ACTION_NONE;
  }

  size_t used = gc->used();
  size_t max_c = gc->max_capacity();
  if (max_c == 0) return REDJET_DOCK_ACTION_NONE;

  double ratio = static_cast<double>(used) / static_cast<double>(max_c);
  uint32_t action = REDJET_DOCK_ACTION_NONE;

  if (ratio >= 0.98) {
    g_redjet_dockpoint_pressure_level = 3;
    sync_global_flags();
    g_coord.dockpoint_pacing_stalls.fetch_add(1, std::memory_order_relaxed);
    g_coord.dockpoint_host_gc_requests.fetch_add(1, std::memory_order_relaxed);
    action = REDJET_DOCK_ACTION_REQUEST_HOST_GC;
    backoff_smt_pause(1024);
  } else if (ratio >= RedjetLandfillCriticalRatio) {
    g_redjet_dockpoint_pressure_level = 2;
    sync_global_flags();
    g_coord.dockpoint_pacing_stalls.fetch_add(1, std::memory_order_relaxed);
    g_coord.dockpoint_falline_drains.fetch_add(1, std::memory_order_relaxed);
    action = REDJET_DOCK_ACTION_DRAIN_FALLINE;
    backoff_smt_pause(256);
  } else if (ratio >= 0.60) {
    g_redjet_dockpoint_pressure_level = 1;
    sync_global_flags();
    action = REDJET_DOCK_ACTION_PACING_STALL;
    backoff_smt_pause(32);
  } else {
    g_redjet_dockpoint_pressure_level = 0;
    sync_global_flags();
  }

  if (cold_target_hdr != nullptr) {
    g_coord.dockpoint_auto_pageins.fetch_add(1, std::memory_order_relaxed);
  }

  if (g_coord.host_hook != nullptr && action != REDJET_DOCK_ACTION_NONE) {
    g_coord.host_hook(static_cast<RedjetDockpointAction>(action),
                      alloc_bytes,
                      g_coord.host_hook_user_data);
  }

  return action;
}

void RedjetPointCoordinator::register_dockpoint_hook(RedjetDockpointHostHookFn hook, void* user_data) {
  g_coord.host_hook = hook;
  g_coord.host_hook_user_data = user_data;
}

void RedjetPointCoordinator::report_c1_method_heat(void* method_ptr,
                                                   uint32_t loop_depth,
                                                   uint32_t lir_op_weight,
                                                   bool bypass_warmup) {
  g_coord.c1_heat_reports.fetch_add(1, std::memory_order_relaxed);
  if (method_ptr == nullptr) return;

  const uintptr_t key = reinterpret_cast<uintptr_t>(method_ptr);
  const size_t idx = (key ^ (key >> 7)) % RedjetCoordinatorState::HEAT_TABLE_SIZE;
  RedjetMethodHeatSlot& slot = g_coord.heat_table[idx];

  uintptr_t cur_key = slot.method_key.load(std::memory_order_acquire);
  if (cur_key != key) {
    slot.method_key.store(key, std::memory_order_release);
    slot.accumulated_heat.store(lir_op_weight * (loop_depth + 1), std::memory_order_release);
    slot.promotion_epoch.store(0, std::memory_order_relaxed);
    return;
  }

  uint32_t inc = lir_op_weight * (loop_depth + 1);
  if (bypass_warmup) {
    inc += 10000u; // Immediate promotion weight
  }
  uint32_t total = slot.accumulated_heat.fetch_add(inc, std::memory_order_acq_rel) + inc;

  if (total >= 1000u && slot.promotion_epoch.load(std::memory_order_relaxed) == 0) {
    slot.promotion_epoch.store(1, std::memory_order_release);
    g_coord.dockpoint_puzzler_promotions.fetch_add(1, std::memory_order_relaxed);
  }
}

void RedjetPointCoordinator::snapshot_telemetry(RedjetTwoPointTelemetry* out) {
  if (out == nullptr) return;
  out->parkpoint_polls           = g_coord.parkpoint_polls.load(std::memory_order_relaxed);
  out->parkpoint_stalls          = g_coord.parkpoint_stalls.load(std::memory_order_relaxed);
  out->parkpoint_parked_threads  = g_coord.parkpoint_parked_threads.load(std::memory_order_relaxed);
  out->active_in_flight_docks    = g_coord.active_in_flight_docks.load(std::memory_order_relaxed);
  out->parkpoint_gate_hits       = g_coord.parkpoint_gate_hits.load(std::memory_order_relaxed);
  out->parkpoint_targeted_yields = g_coord.parkpoint_targeted_yields.load(std::memory_order_relaxed);
  out->parkpoint_active_slots    = g_coord.park_active_refcount.load(std::memory_order_relaxed);
  out->parkpoint_epoch           = g_coord.park_epoch.load(std::memory_order_relaxed);

  out->dockpoint_triggers          = g_coord.dockpoint_triggers.load(std::memory_order_relaxed);
  out->dockpoint_pins              = g_coord.dockpoint_pins.load(std::memory_order_relaxed);
  out->dockpoint_canary_checks     = g_coord.dockpoint_canary_checks.load(std::memory_order_relaxed);
  out->dockpoint_canary_faults     = g_coord.dockpoint_canary_faults.load(std::memory_order_relaxed);
  out->dockpoint_puzzler_promotions= g_coord.dockpoint_puzzler_promotions.load(std::memory_order_relaxed);
  out->dockpoint_checks            = g_coord.dockpoint_checks.load(std::memory_order_relaxed);
  out->dockpoint_bytes_validated   = g_coord.dockpoint_bytes_validated.load(std::memory_order_relaxed);
  out->dockpoint_pacing_stalls     = g_coord.dockpoint_pacing_stalls.load(std::memory_order_relaxed);
  out->dockpoint_falline_drains    = g_coord.dockpoint_falline_drains.load(std::memory_order_relaxed);
  out->dockpoint_auto_pageins      = g_coord.dockpoint_auto_pageins.load(std::memory_order_relaxed);
  out->dockpoint_host_gc_requests  = g_coord.dockpoint_host_gc_requests.load(std::memory_order_relaxed);
  out->c1_heat_reports             = g_coord.c1_heat_reports.load(std::memory_order_relaxed);
}

/* ========================================================================= */
/* C RUNTIME ABI EXPORTS FOR C1, C2, INTERPRETER, AND ADLC STUBS             */
/* ========================================================================= */

extern "C" {

void redjet_rt_parkpoint_poll_slow(JavaThread* thread, void* target_oop) {
  RedjetPointCoordinator::parkpoint_poll(thread, target_oop);
}

void redjet_rt_dockpoint_enter(JavaThread* thread, void* target_oop, uint32_t fn_id) {
  RedjetPointCoordinator::dockpoint_enter(thread, target_oop, fn_id);
}

void redjet_rt_dockpoint_exit(JavaThread* thread, void* target_oop, uint32_t fn_id) {
  RedjetPointCoordinator::dockpoint_exit(thread, target_oop, fn_id);
}

uint32_t redjet_rt_dockpoint_check(JavaThread* thread, size_t alloc_bytes) {
  return RedjetPointCoordinator::dockpoint_check(thread, alloc_bytes, nullptr);
}

void redjet_rt_safepoint_poll_slow(JavaThread* thread) {
  RedjetPointCoordinator::parkpoint_poll(thread, nullptr);
}

void redjet_rt_pausepoint_poll_slow(JavaThread* thread, void* target_oop) {
  RedjetPointCoordinator::parkpoint_poll(thread, target_oop);
}

void redjet_rt_hookpoint_enter(JavaThread* thread, void* target_oop, uint32_t fn_id) {
  RedjetPointCoordinator::dockpoint_enter(thread, target_oop, fn_id);
}

void redjet_rt_hookpoint_exit(JavaThread* thread, void* target_oop, uint32_t fn_id) {
  RedjetPointCoordinator::dockpoint_exit(thread, target_oop, fn_id);
}

uint32_t redjet_rt_controlpoint_check(JavaThread* thread, size_t alloc_bytes) {
  return RedjetPointCoordinator::dockpoint_check(thread, alloc_bytes, nullptr);
}

void redjet_rt_c1_report_method_heat(void* method_ptr, uint32_t loop_depth, uint32_t lir_op_weight, bool bypass_warmup) {
  RedjetPointCoordinator::report_c1_method_heat(method_ptr, loop_depth, lir_op_weight, bypass_warmup);
}

} // extern "C"
