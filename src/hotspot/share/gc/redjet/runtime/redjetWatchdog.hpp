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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETWATCHDOG_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETWATCHDOG_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"

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
// Redjet Real-Time Thread Watchdog & Deadlock Breaker (PLAN.md Section 7.1, 9.3)
// - Monitors PointCoordinator poll flags (_park_flag, _dock_flag) across all threads
// - Intercepts unresponsive threads stuck in native loops or uncounted JIT loops
// - Dispatches targeted preemption signals (QueueUserAPC / SIGUSR2)
// - Guarantees zero unrecoverable STW deadlocks during micro-epochs
// ============================================================================

enum class RedjetThreadWatchState : uint8_t {
  THREAD_RESPONSIVE  = 0,
  THREAD_PENDING     = 1,
  THREAD_STALLED     = 2,
  THREAD_ESCALATED   = 3
};

struct RedjetThreadWatchEntry {
  uint32_t               thread_id{ 0 };
  uint64_t               last_heartbeat_tsc{ 0 };
  RedjetThreadWatchState state{ RedjetThreadWatchState::THREAD_RESPONSIVE };
  uint32_t               stall_duration_ms{ 0 };
};

class RedjetWatchdog {
public:
  static constexpr size_t   MAX_WATCHED_THREADS = 512;
  static constexpr uint32_t STALL_THRESHOLD_MS  = 5; // 5ms maximum allowable poll latency

private:
  static RedjetWatchdog*   _instance;
  static std::atomic<bool> _initialized;

  RedjetThreadWatchEntry   _threads[MAX_WATCHED_THREADS];
  size_t                   _thread_count{ 0 };
  hs_ticket_lock_t         _watchdog_lock;

  // Telemetry
  std::atomic<uint64_t>    _total_watchdog_ticks{ 0 };
  std::atomic<uint64_t>    _total_stalls_detected{ 0 };
  std::atomic<uint64_t>    _total_signals_dispatched{ 0 };
  std::atomic<uint64_t>    _total_deadlocks_broken{ 0 };

public:
  RedjetWatchdog();
  ~RedjetWatchdog();

  static void initialize();
  static void destroy();
  static RedjetWatchdog* get() noexcept { return _instance; }

  // 1. Thread Heartbeat & Registration
  RJ_API void register_thread(uint32_t thread_id);
  RJ_API void unregister_thread(uint32_t thread_id);
  RJ_API void post_heartbeat(uint32_t thread_id) noexcept;

  // 2. Watchdog Inspection Tick (invoked periodically by timer)
  RJ_API size_t inspect_and_escalate();

  // 3. Preemption Signal Dispatch
  RJ_API static bool dispatch_preemption_signal(uint32_t thread_id);

  // Telemetry
  uint64_t watchdog_ticks()     const noexcept { return _total_watchdog_ticks.load(std::memory_order_relaxed); }
  uint64_t stalls_detected()    const noexcept { return _total_stalls_detected.load(std::memory_order_relaxed); }
  uint64_t signals_dispatched() const noexcept { return _total_signals_dispatched.load(std::memory_order_relaxed); }
  uint64_t deadlocks_broken()   const noexcept { return _total_deadlocks_broken.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETWATCHDOG_HPP
