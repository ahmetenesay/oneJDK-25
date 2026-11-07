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

#ifndef SHARE_MIRC_MIRC_GROUPCOMPUTE_HPP
#define SHARE_MIRC_MIRC_GROUPCOMPUTE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// MIRC_GroupCompute
//
// Compiler-First Group Execution, Silicon P/E-Core Affinity & ALU Dispatching:
//   - Assigns compact 4-byte `group_hex_id` (`0x1000..0xFFFF`) to `@GroupCompute`
//     methods and loops for 0.1ns zero-string HotLog tracing.
//   - Pins priority >= 8 compute groups onto P-Cores (Performance Cores) and
//     enables Ring-3 HighQoS warmup pacing across ALL GCs (using `RedjetUpclocker`
//     when `UseRedjetGC`, or direct OS Ring-3 thread affinity/QoS under G1/ZGC/Shenandoah).
//   - Enforces `-XX:+RedjetGCAllowOperationDispatchings` ALU work-stealing rules:
//     priority 8..10 groups lock onto their local L1/L2 CCD core without cross-CCD
//     stealing jitter; priority 1..7 groups allow multi-lane ALU work distribution.
// ============================================================================

struct MIRC_GroupHandle {
  uint32_t group_hex_id{ 0 };      // 0x1000 .. 0xFFFF
  uint8_t  priority{ 4 };          // 0..10 (8..10 = P-Core locked, no cross-CCD steal)
  bool     pinned_to_pcore{ false };
  bool     alu_steal_allowed{ false };
  uint64_t enter_tsc{ 0 };
};

class MIRC_GroupCompute {
private:
  static std::atomic<uint32_t> _fallback_hex_counter;
  static std::atomic<uint64_t> _total_groups_entered;
  static std::atomic<uint64_t> _total_alu_dispatches;

public:
  // Enters a compiler-emitted `@GroupCompute` scope
  RJ_API static MIRC_GroupHandle enter_group(const char* group_label,
                                             uint8_t priority,
                                             uint32_t thread_id = 0) noexcept;

  // Emits a 4-byte hex telemetry event inside an active group (0.1ns hotpath)
  RJ_API static void record_group_step(const MIRC_GroupHandle& handle,
                                       uint32_t event_code,
                                       uint64_t work_units) noexcept;

  // Queries whether multi-lane ALU work-stealing dispatch is permitted for `handle`
  RJ_API static bool can_dispatch_across_alu_lanes(const MIRC_GroupHandle& handle) noexcept {
    return handle.alu_steal_allowed;
  }

  // Exits a `@GroupCompute` scope and restores normal thread QoS/affinity
  RJ_API static void exit_group(MIRC_GroupHandle& handle) noexcept;

  static uint64_t total_groups_entered() noexcept { return _total_groups_entered.load(std::memory_order_relaxed); }
  static uint64_t total_alu_dispatches() noexcept { return _total_alu_dispatches.load(std::memory_order_relaxed); }
};

#endif // SHARE_MIRC_MIRC_GROUPCOMPUTE_HPP
