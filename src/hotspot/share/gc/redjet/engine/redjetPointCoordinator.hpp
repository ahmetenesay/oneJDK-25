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

#ifndef SHARE_GC_REDJET_REDJETPOINTCOORDINATOR_HPP
#define SHARE_GC_REDJET_REDJETPOINTCOORDINATOR_HPP

#include "memory/allStatic.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/globalDefinitions.hpp"

// Strictly C++14-compatible header for direct inclusion by C1, C2, MacroAssembler,
// Interpreter, JNI Bridge, and RedjetGC/RedjetGC subsystems.
//
// 2-Points Coordination Engine:
// 1. Parkpoint (replaces Safepoint + Pausepoint):
//    - Unifies STW mutator parking and targeted microsecond loop yielding / object pause.
// 2. Dockpoint (replaces Hookpoint + Controlpoint):
//    - Unifies object capture, tail canary inspection, ISA ladder verify, object pinning,
//      and heap pressure regulation, pacing stalls, Falline draining, cold tensor page-in.

extern "C" {
  extern volatile uint32_t g_redjet_parkpoint_active_flag;
  extern volatile uint32_t g_redjet_parkpoint_gate_flag;
  extern volatile uint32_t g_redjet_dockpoint_pressure_level;

  // Compatibility aliases for assembler and transition stubs
  extern volatile uint32_t g_redjet_safepoint_active_flag;
  extern volatile uint32_t g_redjet_pausepoint_gate_flag;
  extern volatile uint32_t g_redjet_controlpoint_pressure_level;
}

enum RedjetDockpointAction : uint32_t {
  REDJET_DOCK_ACTION_NONE            = 0,
  REDJET_DOCK_ACTION_PACING_STALL    = 1,
  REDJET_DOCK_ACTION_DRAIN_FALLINE   = 2,
  REDJET_DOCK_ACTION_AUTO_PAGEIN     = 3,
  REDJET_DOCK_ACTION_PARK_MUTATORS   = 4,
  REDJET_DOCK_ACTION_RESUME_MUTATORS = 5,
  REDJET_DOCK_ACTION_REQUEST_HOST_GC = 6
};

typedef RedjetDockpointAction RedjetControlpointAction;
typedef RedjetDockpointAction hs_cp_action_e;

enum RedjetDockpointEvent : uint32_t {
  REDJET_DOCK_KERNEL_ENTER  = 0,
  REDJET_DOCK_KERNEL_EXIT   = 1,
  REDJET_DOCK_INSPECT_ISA   = 2,
  REDJET_DOCK_VERIFY_CANARY = 3
};

typedef RedjetDockpointEvent RedjetHookpointEvent;

struct RedjetTwoPointTelemetry {
  // 1. Parkpoint (Safepoint + Pausepoint)
  uint64_t parkpoint_polls;
  uint64_t parkpoint_stalls;
  uint32_t parkpoint_parked_threads;
  uint32_t active_in_flight_docks;
  uint64_t parkpoint_gate_hits;
  uint64_t parkpoint_targeted_yields;
  uint32_t parkpoint_active_slots;
  uint32_t parkpoint_epoch;

  // 2. Dockpoint (Hookpoint + Controlpoint & C1 Heat)
  uint64_t dockpoint_triggers;
  uint64_t dockpoint_pins;
  uint64_t dockpoint_canary_checks;
  uint64_t dockpoint_canary_faults;
  uint64_t dockpoint_puzzler_promotions;
  uint64_t dockpoint_checks;
  uint64_t dockpoint_bytes_validated;
  uint64_t dockpoint_pacing_stalls;
  uint64_t dockpoint_falline_drains;
  uint64_t dockpoint_auto_pageins;
  uint64_t dockpoint_host_gc_requests;
  uint64_t c1_heat_reports;
};

typedef RedjetTwoPointTelemetry RedjetFourPointTelemetry;

typedef void (*RedjetDockpointHostHookFn)(RedjetDockpointAction action,
                                          uint64_t projected_bytes,
                                          void* user_data);
typedef RedjetDockpointHostHookFn RedjetControlpointHostHookFn;

class RedjetPointCoordinator : public AllStatic {
public:
  static const uint32_t MAX_PARKPOINT_SLOTS = 16;
  static const uint32_t MAX_PAUSEPOINT_SLOTS = MAX_PARKPOINT_SLOTS;

  static void initialize();
  static void shutdown();

  // Direct poll flag addresses for MacroAssembler / C1 / C2 / TableGen / ADLC
  static address parkpoint_flag_address() {
    return reinterpret_cast<address>(const_cast<uint32_t*>(&g_redjet_parkpoint_active_flag));
  }

  static address parkpoint_gate_address() {
    return reinterpret_cast<address>(const_cast<uint32_t*>(&g_redjet_parkpoint_gate_flag));
  }

  static address dockpoint_level_address() {
    return reinterpret_cast<address>(const_cast<uint32_t*>(&g_redjet_dockpoint_pressure_level));
  }

  // Backward-compatible address getters
  static address safepoint_flag_address()    { return parkpoint_flag_address(); }
  static address pausepoint_gate_address()   { return parkpoint_gate_address(); }
  static address controlpoint_level_address(){ return dockpoint_level_address(); }

  // 1. Parkpoint (Unifying Safepoint STW and Pausepoint targeted microsecond mutator pause)
  static void arm_parkpoint();
  static void disarm_parkpoint();
  static bool is_parkpoint_active();
  static void parkpoint_poll(Thread* thread, void* target_slot = nullptr);

  static void request_object_park(void* target_slot);
  static void release_object_park(void* target_slot);
  static bool is_object_parked(const void* target_slot);

  // Backward compatibility bridges
  static void arm_safepoint()                       { arm_parkpoint(); }
  static void disarm_safepoint()                    { disarm_parkpoint(); }
  static bool is_safepoint_active()                 { return is_parkpoint_active(); }
  static void safepoint_poll(Thread* thread)        { parkpoint_poll(thread, nullptr); }
  static void request_object_pause(void* target_slot){ request_object_park(target_slot); }
  static void release_object_pause(void* target_slot){ release_object_park(target_slot); }
  static bool is_object_paused(const void* target_slot) { return is_object_parked(target_slot); }
  static void pausepoint_poll(Thread* thread, void* target_slot = nullptr) { parkpoint_poll(thread, target_slot); }

  // 2. Dockpoint (Unifying Hookpoint object capture/pin/canary and Controlpoint heap pressure/pacing/paging)
  static void     dockpoint_enter(Thread* thread, void* obj_or_hdr, uint32_t fn_id = 0);
  static void     dockpoint_exit(Thread* thread, void* obj_or_hdr, uint32_t fn_id = 0);
  static uint32_t dockpoint_check(Thread* thread, size_t alloc_bytes, void* cold_target_hdr = nullptr);
  static bool     inspect_and_verify_object(void* obj_or_hdr,
                                            uint16_t* out_active_simd_idx = nullptr,
                                            bool* out_is_fledgling = nullptr);
  static void     register_dockpoint_hook(RedjetDockpointHostHookFn hook, void* user_data = nullptr);

  // Backward compatibility bridges
  static void     hookpoint_enter(Thread* thread, void* obj_or_hdr, uint32_t fn_id = 0) {
    dockpoint_enter(thread, obj_or_hdr, fn_id);
  }
  static void     hookpoint_exit(Thread* thread, void* obj_or_hdr, uint32_t fn_id = 0) {
    dockpoint_exit(thread, obj_or_hdr, fn_id);
  }
  static uint32_t controlpoint_check(Thread* thread, size_t alloc_bytes, void* cold_target_hdr = nullptr) {
    return dockpoint_check(thread, alloc_bytes, cold_target_hdr);
  }
  static void     register_controlpoint_hook(RedjetControlpointHostHookFn hook, void* user_data = nullptr) {
    register_dockpoint_hook(hook, user_data);
  }

  // 3. Hardware Synchronization & 2-Point Rendezvous
  static void sync_point();
  static void hardware_fence();
  static void flush_icache(address code_begin = nullptr, size_t code_size = 0);

  // 4. EmptyBox & Parkpoint Hardware Protection
  static bool park_object_with_emptybox_guard(void* target_slot, size_t slot_bytes, uint32_t tid, uint64_t timeout_cycles);
  static bool unpark_object_and_reclaim_emptybox(void* target_slot, uint32_t tid);

  // 5. Dockpoint Trampoline & Canary Enforcement
  static void* resolve_dockpoint_target(void* method_ptr, uint16_t active_simd_idx);
  static bool  verify_dockpoint_canary(void* obj_ptr);

  // C1 Heat Feedback Receiver -> Puzzler / Self-Advance Coach
  static void report_c1_method_heat(void* method_ptr, uint32_t loop_depth, uint32_t lir_op_weight, bool bypass_warmup);

  // Unified 2-Point Telemetry Snapshot
  static void snapshot_telemetry(RedjetTwoPointTelemetry* out);
};

extern "C" {
  void     redjet_rt_parkpoint_poll_slow(JavaThread* thread, void* target_oop);
  void     redjet_rt_dockpoint_enter(JavaThread* thread, void* target_oop, uint32_t fn_id);
  void     redjet_rt_dockpoint_exit(JavaThread* thread, void* target_oop, uint32_t fn_id);
  uint32_t redjet_rt_dockpoint_check(JavaThread* thread, size_t alloc_bytes);

  // Backward compatibility symbols
  void     redjet_rt_safepoint_poll_slow(JavaThread* thread);
  void     redjet_rt_pausepoint_poll_slow(JavaThread* thread, void* target_oop);
  void     redjet_rt_hookpoint_enter(JavaThread* thread, void* target_oop, uint32_t fn_id);
  void     redjet_rt_hookpoint_exit(JavaThread* thread, void* target_oop, uint32_t fn_id);
  uint32_t redjet_rt_controlpoint_check(JavaThread* thread, size_t alloc_bytes);
  void     redjet_rt_c1_report_method_heat(void* method_ptr, uint32_t loop_depth, uint32_t lir_op_weight, bool bypass_warmup);
}

#endif // SHARE_GC_REDJET_REDJETPOINTCOORDINATOR_HPP
