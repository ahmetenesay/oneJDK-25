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

#include "mirc/mirc_GroupCompute.hpp"
#include "gc/shared/gc_globals.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_REDJETGC
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetUpclocker.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

std::atomic<uint32_t> MIRC_GroupCompute::_fallback_hex_counter{ 0x1000u };
std::atomic<uint64_t> MIRC_GroupCompute::_total_groups_entered{ 0 };
std::atomic<uint64_t> MIRC_GroupCompute::_total_alu_dispatches{ 0 };

namespace {
static inline uint64_t group_read_tsc() noexcept {
#if INCLUDE_REDJETGC
  return sacculus_rdtsc();
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
  uint32_t lo = 0, hi = 0;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<uint64_t>(hi) << 32) | lo;
#else
  return 0;
#endif
}
} // namespace

MIRC_GroupHandle MIRC_GroupCompute::enter_group(const char* group_label,
                                                uint8_t priority,
                                                uint32_t thread_id) noexcept {
  MIRC_GroupHandle h{};
  if (priority > 10) priority = 10;
  h.priority  = priority;
  h.enter_tsc = group_read_tsc();
  _total_groups_entered.fetch_add(1, std::memory_order_relaxed);

#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetGroupPriority rj_prio = RedjetGroupPriority::PRIORITY_NORMAL;
    if (priority >= 10)     rj_prio = RedjetGroupPriority::PRIORITY_CRITICAL;
    else if (priority >= 8) rj_prio = RedjetGroupPriority::PRIORITY_HIGH;
    else if (priority == 0) rj_prio = RedjetGroupPriority::PRIORITY_IDLE;

    h.group_hex_id    = RedjetUpclocker::instance().allocate_group_hex_id(group_label, thread_id, rj_prio);
    h.pinned_to_pcore = (priority >= 8);

    const bool allow_dispatch_flag =
        RedjetGCAllowOperationDispatchings || RedjetFlags::config().strings.allow_operation_dispatchings;
    // Priority 8..10 groups lock to local L1/L2 CCD core to protect locality;
    // Priority 1..7 groups distribute across ALU lanes when AllowOperationDispatchings is enabled.
    h.alu_steal_allowed = allow_dispatch_flag && (priority > 0 && priority < 8) && !RedjetSCMode && !RedjetGCSCMode;
    if (h.alu_steal_allowed) {
      _total_alu_dispatches.fetch_add(1, std::memory_order_relaxed);
    }
    RedjetUpclocker::instance().emit_hex_hotlog(h.group_hex_id, 0x01u, priority);
    return h;
  }
#endif

  // Universal GC path (G1 / ZGC / Shenandoah / Parallel / Serial)
  uint32_t next_hex = _fallback_hex_counter.fetch_add(1, std::memory_order_relaxed);
  if (next_hex > 0xFFFFu) {
    _fallback_hex_counter.store(0x1000u, std::memory_order_relaxed);
    next_hex = 0x1000u;
  }
  h.group_hex_id      = next_hex;
  h.alu_steal_allowed = (priority > 0 && priority < 8);

  if (priority >= 8) {
#if defined(_WIN32)
    HANDLE hThread = GetCurrentThread();
    SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
    THREAD_POWER_THROTTLING_STATE power_state{};
    power_state.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    power_state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    power_state.StateMask   = 0; // HighQoS P-Core preference
    (void)SetThreadInformation(hThread, ThreadPowerThrottling, &power_state, sizeof(power_state));
#endif
#endif
    h.pinned_to_pcore = true;
  }
  (void)group_label;
  (void)thread_id;
  return h;
}

void MIRC_GroupCompute::record_group_step(const MIRC_GroupHandle& handle,
                                          uint32_t event_code,
                                          uint64_t work_units) noexcept {
#if INCLUDE_REDJETGC
  if (UseRedjetGC && handle.group_hex_id != 0) {
    RedjetUpclocker::instance().emit_hex_hotlog(handle.group_hex_id, event_code, work_units);
    return;
  }
#endif
  (void)handle;
  (void)event_code;
  (void)work_units;
}

void MIRC_GroupCompute::exit_group(MIRC_GroupHandle& handle) noexcept {
  if (handle.group_hex_id == 0) return;

#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    uint64_t elapsed_cycles = group_read_tsc() - handle.enter_tsc;
    RedjetUpclocker::instance().emit_hex_hotlog(handle.group_hex_id, 0x02u, elapsed_cycles);
    RedjetUpclocker::instance().release_group_hex_id(handle.group_hex_id);
    handle.group_hex_id = 0;
    return;
  }
#endif

  if (handle.pinned_to_pcore) {
#if defined(_WIN32)
    HANDLE hThread = GetCurrentThread();
    SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
    THREAD_POWER_THROTTLING_STATE power_state{};
    power_state.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    power_state.ControlMask = 0;
    power_state.StateMask   = 0;
    (void)SetThreadInformation(hThread, ThreadPowerThrottling, &power_state, sizeof(power_state));
#endif
#endif
  }
  handle.group_hex_id = 0;
}
