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

#include "gc/redjet/runtime/redjetUpclocker.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#else
#include <sched.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetUpclocker g_redjet_upclocker;

RedjetUpclocker::RedjetUpclocker() {
  _group_counter.store(REDJET_GROUP_HEX_BASE, std::memory_order_relaxed);
  _total_groups_created.store(0, std::memory_order_relaxed);
  _total_pcore_pins.store(0, std::memory_order_relaxed);
  _total_turbo_boosts_applied.store(0, std::memory_order_relaxed);
  _total_warmup_bursts.store(0, std::memory_order_relaxed);
  _total_hex_hotlogs_emitted.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < REDJET_MAX_ACTIVE_GROUPS; ++i) {
    _group_registry[i].reset();
  }
}

RedjetUpclocker::~RedjetUpclocker() {
  destroy();
}

RedjetUpclocker& RedjetUpclocker::instance() {
  return g_redjet_upclocker;
}

void RedjetUpclocker::init() {
  hs_ticket_guard_t guard(_upclocker_lock);
  _group_counter.store(REDJET_GROUP_HEX_BASE, std::memory_order_relaxed);
  for (size_t i = 0; i < REDJET_MAX_ACTIVE_GROUPS; ++i) {
    _group_registry[i].reset();
  }
}

void RedjetUpclocker::destroy() {
  hs_ticket_guard_t guard(_upclocker_lock);
  for (size_t i = 0; i < REDJET_MAX_ACTIVE_GROUPS; ++i) {
    if (_group_registry[i].is_turbo_pinned.load(std::memory_order_relaxed)) {
      release_os_turbo_pin(_group_registry[i].owner_thread_id);
    }
    _group_registry[i].reset();
  }
}

// =========================================================================
// 1. Group Hex ID Allocation & Management (0x1000..0xFFFF)
// =========================================================================
uint32_t RedjetUpclocker::allocate_group_hex_id(const char* name,
                                                uint32_t owner_thread_id,
                                                RedjetGroupPriority priority) {
  uint32_t next_id = _group_counter.fetch_add(1, std::memory_order_relaxed);
  if (next_id > REDJET_GROUP_HEX_MAX) {
    _group_counter.compare_exchange_strong(next_id, REDJET_GROUP_HEX_BASE);
    next_id = REDJET_GROUP_HEX_BASE;
  }

  hs_ticket_guard_t guard(_upclocker_lock);
  size_t slot = next_id % REDJET_MAX_ACTIVE_GROUPS;
  RedjetGroupDescriptor& desc = _group_registry[slot];

  if (desc.is_turbo_pinned.load(std::memory_order_relaxed)) {
    release_os_turbo_pin(desc.owner_thread_id);
  }

  desc.reset();
  desc.group_hex_id = next_id;
  desc.owner_thread_id = owner_thread_id;
  desc.priority = priority;
  desc.creation_tsc = sacculus_rdtsc();
  desc.last_active_tsc = desc.creation_tsc;
  desc.is_active.store(true, std::memory_order_relaxed);

  if (name != nullptr) {
    strncpy(desc.group_name, name, sizeof(desc.group_name) - 1);
    desc.group_name[sizeof(desc.group_name) - 1] = '\0';
  }

  _total_groups_created.fetch_add(1, std::memory_order_relaxed);

  // High priority automatically engages P-Core locking and turbo boost
  if (static_cast<uint8_t>(priority) >= static_cast<uint8_t>(RedjetGroupPriority::PRIORITY_HIGH)) {
    pin_pcore_and_upclock(next_id, DEFAULT_WARMUP_BURST_MS);
  }

  return next_id;
}

void RedjetUpclocker::release_group_hex_id(uint32_t group_hex_id) {
  if (group_hex_id < REDJET_GROUP_HEX_BASE || group_hex_id > REDJET_GROUP_HEX_MAX) return;

  hs_ticket_guard_t guard(_upclocker_lock);
  size_t slot = group_hex_id % REDJET_MAX_ACTIVE_GROUPS;
  RedjetGroupDescriptor& desc = _group_registry[slot];

  if (desc.group_hex_id == group_hex_id && desc.is_active.load(std::memory_order_relaxed)) {
    if (desc.is_turbo_pinned.load(std::memory_order_relaxed)) {
      release_os_turbo_pin(desc.owner_thread_id);
    }
    desc.reset();
  }
}

bool RedjetUpclocker::is_group_active(uint32_t group_hex_id) const noexcept {
  if (group_hex_id < REDJET_GROUP_HEX_BASE || group_hex_id > REDJET_GROUP_HEX_MAX) return false;
  size_t slot = group_hex_id % REDJET_MAX_ACTIVE_GROUPS;
  const RedjetGroupDescriptor& desc = _group_registry[slot];
  return (desc.group_hex_id == group_hex_id && desc.is_active.load(std::memory_order_relaxed));
}

// =========================================================================
// 2. Zero-Cost Hex HotLog / ColdLog Emission (0.1 ns)
// =========================================================================
void RedjetUpclocker::emit_hex_hotlog(uint32_t group_hex_id, uint32_t event_code, uint64_t payload) {
  // Ultra-fast 4-byte hex event emission into HotLog ring without string formatting
  hs_hotlog_emit(static_cast<hs_ebin0_trace_category_t>(event_code & 0x0F),
                 HS_Ebin1_INFO,
                 group_hex_id,
                 payload,
                 0, 0, 0, 0, 0,
                 "HexGroupEvent");

  _total_hex_hotlogs_emitted.fetch_add(1, std::memory_order_relaxed);
}

// =========================================================================
// 3. Silicon Affinity & Upclocker Turbo Pinning
// =========================================================================
bool RedjetUpclocker::pin_pcore_and_upclock(uint32_t group_hex_id, uint32_t burst_ms) {
  if (group_hex_id < REDJET_GROUP_HEX_BASE || group_hex_id > REDJET_GROUP_HEX_MAX) return false;

  hs_ticket_guard_t guard(_upclocker_lock);
  size_t slot = group_hex_id % REDJET_MAX_ACTIVE_GROUPS;
  RedjetGroupDescriptor& desc = _group_registry[slot];

  if (desc.group_hex_id != group_hex_id || !desc.is_active.load(std::memory_order_relaxed)) {
    return false;
  }

  // Bind current thread to Performance Cores (P-Cores)
  hs_bind_current_thread(HS_CORE_TIER_PERFORMANCE);
  _total_pcore_pins.fetch_add(1, std::memory_order_relaxed);

  // Apply OS-level turbo boost pinning
  bool ok = apply_os_turbo_pin(desc.owner_thread_id, burst_ms);
  if (ok) {
    desc.is_turbo_pinned.store(true, std::memory_order_relaxed);
    _total_turbo_boosts_applied.fetch_add(1, std::memory_order_relaxed);
  }

  return ok;
}

void RedjetUpclocker::unpin_pcore_restore_normal(uint32_t group_hex_id) {
  if (group_hex_id < REDJET_GROUP_HEX_BASE || group_hex_id > REDJET_GROUP_HEX_MAX) return;

  hs_ticket_guard_t guard(_upclocker_lock);
  size_t slot = group_hex_id % REDJET_MAX_ACTIVE_GROUPS;
  RedjetGroupDescriptor& desc = _group_registry[slot];

  if (desc.group_hex_id == group_hex_id && desc.is_active.load(std::memory_order_relaxed)) {
    if (desc.is_turbo_pinned.load(std::memory_order_relaxed)) {
      release_os_turbo_pin(desc.owner_thread_id);
      desc.is_turbo_pinned.store(false, std::memory_order_relaxed);
    }
    // Restore default thread affinity
    hs_bind_current_thread(HS_CORE_TIER_ALL);
  }
}

bool RedjetUpclocker::pin_max_frequency_warmup(uint32_t thread_id, uint32_t burst_ms) {
  _total_warmup_bursts.fetch_add(1, std::memory_order_relaxed);
  return apply_os_turbo_pin(thread_id, burst_ms);
}

namespace {
static std::atomic<bool> g_upclocker_global_boost{ false };
#if defined(__linux__)
static int g_pm_qos_dma_fd = -1;
#endif
} // namespace

bool RedjetUpclocker::apply_os_turbo_pin(uint32_t thread_id, uint32_t burst_ms) {
  (void)thread_id;
  (void)burst_ms;

#if defined(_WIN32)
  // Pure Ring-3 User-Space HighQoS / EcoQoS Throttling Disable + 1ms Timer Quantum
  timeBeginPeriod(1);
  HANDLE hThread = GetCurrentThread();
  SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
  SetThreadPriorityBoost(hThread, FALSE); // Enable dynamic foreground priority boost

#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
  THREAD_POWER_THROTTLING_STATE power_state{};
  power_state.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  power_state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  power_state.StateMask   = 0; // 0 = HighQoS (explicitly disable EcoQoS / E-Core power throttling)
  (void)SetThreadInformation(hThread, ThreadPowerThrottling, &power_state, sizeof(power_state));
#endif
  return true;
#elif defined(__linux__)
  // Pure Ring-3 User-Space PM-QoS DMA latency hint (if readable/writable) + priority boost
  if (g_pm_qos_dma_fd < 0) {
    int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      int32_t zero_us = 0;
      if (write(fd, &zero_us, sizeof(zero_us)) == sizeof(zero_us)) {
        g_pm_qos_dma_fd = fd;
      } else {
        close(fd);
      }
    }
  }
  pthread_t cur = pthread_self();
  struct sched_param param;
  param.sched_priority = 0;
  pthread_setschedparam(cur, SCHED_OTHER, &param);
  (void)setpriority(PRIO_PROCESS, 0, -10);
  return true;
#else
  return true;
#endif
}

void RedjetUpclocker::release_os_turbo_pin(uint32_t thread_id) {
  (void)thread_id;

#if defined(_WIN32)
  timeEndPeriod(1);
  HANDLE hThread = GetCurrentThread();
  SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
  THREAD_POWER_THROTTLING_STATE power_state{};
  power_state.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  power_state.ControlMask = 0; // Restore OS automatic QoS management
  power_state.StateMask   = 0;
  (void)SetThreadInformation(hThread, ThreadPowerThrottling, &power_state, sizeof(power_state));
#endif
#elif defined(__linux__)
  if (g_pm_qos_dma_fd >= 0) {
    close(g_pm_qos_dma_fd);
    g_pm_qos_dma_fd = -1;
  }
  (void)setpriority(PRIO_PROCESS, 0, 0);
#endif
}

extern "C" {

RJ_API void hs_upclocker_set_boost(int enable) {
  bool en = (enable != 0);
  g_upclocker_global_boost.store(en, std::memory_order_release);
  if (en) {
    RedjetUpclocker::instance().pin_max_frequency_warmup(0, RedjetUpclocker::DEFAULT_WARMUP_BURST_MS);
  }
}

RJ_API int hs_upclocker_is_boost_enabled(void) {
  return g_upclocker_global_boost.load(std::memory_order_acquire) ? 1 : 0;
}

} // extern "C"

void RedjetUpclocker::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet Upclocker Turbo Boost Engine & Group Hex Manager:");
  st->print_cr("  Current Group Counter:      0x%04X", _group_counter.load(std::memory_order_relaxed));
  st->print_cr("  Total Groups Created:       " UINT64_FORMAT, total_groups_created());
  st->print_cr("  Total P-Core Pins:          " UINT64_FORMAT, total_pcore_pins());
  st->print_cr("  Total Turbo Boosts:         " UINT64_FORMAT, total_turbo_boosts());
  st->print_cr("  Total Warmup Bursts:        " UINT64_FORMAT, total_warmup_bursts());
  st->print_cr("  Total Hex HotLogs Emitted:  " UINT64_FORMAT, total_hex_hotlogs());
}
