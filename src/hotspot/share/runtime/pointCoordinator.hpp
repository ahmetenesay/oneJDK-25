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

#ifndef SHARE_RUNTIME_POINTCOORDINATOR_HPP
#define SHARE_RUNTIME_POINTCOORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

class JavaThread;
class Thread;

// ============================================================================
// Tier 1 HotSpot Universal Point Coordinator (PLAN.md Section 8.2)
// GC-independent runtime arbiter for 2-Points (Dockpoint & Parkpoint)
// Provides 0.5-cycle L1-D cache poll flags (_park_flag and _dock_flag)
// Zero-vtable inline static coupling with Tier 2 (RedjetPointCoordinator)
// ============================================================================

class PointCoordinator : public AllStatic {
public:
  enum DockReason {
    DOCK_REASON_METHOD_ENTRY    = 0,
    DOCK_REASON_ALLOC_TLAB      = 1,
    DOCK_REASON_ALLOC_SLOW      = 2,
    DOCK_REASON_METHOD_EXIT     = 3,
    DOCK_REASON_TAILTRACES      = 4,
    DOCK_REASON_CANARY_CHECK    = 5,
    DOCK_REASON_BUS_PACING      = 6
  };

  enum ParkReason {
    PARK_REASON_LOOP_BACKEDGE   = 0,
    PARK_REASON_JNI_ENTER       = 1,
    PARK_REASON_JNI_EXIT        = 2,
    PARK_REASON_SLOT16_SWAP     = 3,
    PARK_REASON_DEFRAG_RENDEZ   = 4,
    PARK_REASON_VFS_PINNING     = 5
  };

private:
  static alignas(64) std::atomic<uint8_t>  _global_park_armed;
  static alignas(64) std::atomic<uint8_t>  _global_dock_armed;
  static std::atomic<uint64_t>             _total_park_polls;
  static std::atomic<uint64_t>             _total_dock_checks;
  static std::atomic<uint64_t>             _total_jni_transitions;
  static std::atomic<uint64_t>             _total_slot16_hotswaps;
  static std::atomic<uint64_t>             _total_park_synchronizations;
  static std::atomic<bool>                 _initialized;

public:
  static void initialize();
  static void shutdown();
  static bool is_initialized() { return _initialized.load(std::memory_order_relaxed); }

  // -------------------------------------------------------------------------
  // 1. Thread-Local Poll Tests (Ultra-Low Latency L1-D Cache Access)
  // -------------------------------------------------------------------------
  static bool is_park_armed(JavaThread* thread);
  static bool is_dock_armed(JavaThread* thread);

  // -------------------------------------------------------------------------
  // 2. Individual Thread Arming / Disarming
  // -------------------------------------------------------------------------
  static void arm_park(JavaThread* thread);
  static void disarm_park(JavaThread* thread);
  static void arm_dock(JavaThread* thread);
  static void disarm_dock(JavaThread* thread);

  // -------------------------------------------------------------------------
  // 3. Global Fleet-Wide Arming / Disarming
  // -------------------------------------------------------------------------
  static void arm_park_all();
  static void disarm_park_all();
  static void arm_dock_all();
  static void disarm_dock_all();

  // -------------------------------------------------------------------------
  // 4. Java <-> Native (JNI / FFM) Boundary Hooks
  // -------------------------------------------------------------------------
  static void on_jni_park_enter(JavaThread* thread);
  static void on_jni_park_exit(JavaThread* thread);

  // -------------------------------------------------------------------------
  // 5. Point Dispatch Handlers
  // -------------------------------------------------------------------------
  static void on_park_point_hit(JavaThread* thread, ParkReason reason = PARK_REASON_LOOP_BACKEDGE);
  static void on_dock_point_hit(JavaThread* thread, DockReason reason = DOCK_REASON_METHOD_ENTRY);

  // -------------------------------------------------------------------------
  // 6. Zero-STW Synchronizations and Hardware Fences
  // -------------------------------------------------------------------------
  static void synchronize_park();
  static void desynchronize_park();
  static void hardware_fence();
  static bool coordinate_slot16_hotswap(void* slot16_addr, void* new_target_slot);

  // -------------------------------------------------------------------------
  // 7. Direct Poll Flag Addresses for Compilers (VAC, MIRC, MacroAssembler)
  // -------------------------------------------------------------------------
  static address global_park_flag_address() {
    return reinterpret_cast<address>(const_cast<std::atomic<uint8_t>*>(&_global_park_armed));
  }

  static address global_dock_flag_address() {
    return reinterpret_cast<address>(const_cast<std::atomic<uint8_t>*>(&_global_dock_armed));
  }

  // -------------------------------------------------------------------------
  // 8. Telemetry & Diagnostics
  // -------------------------------------------------------------------------
  static uint64_t total_park_polls()            { return _total_park_polls.load(std::memory_order_relaxed); }
  static uint64_t total_dock_checks()           { return _total_dock_checks.load(std::memory_order_relaxed); }
  static uint64_t total_jni_transitions()       { return _total_jni_transitions.load(std::memory_order_relaxed); }
  static uint64_t total_slot16_hotswaps()       { return _total_slot16_hotswaps.load(std::memory_order_relaxed); }
  static uint64_t total_park_synchronizations() { return _total_park_synchronizations.load(std::memory_order_relaxed); }

  static void print_on(outputStream* st);
};

#endif // SHARE_RUNTIME_POINTCOORDINATOR_HPP
