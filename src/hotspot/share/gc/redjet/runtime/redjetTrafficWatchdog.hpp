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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETTRAFFICWATCHDOG_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETTRAFFICWATCHDOG_HPP

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
// Redjet Allocation Traffic Watchdog & Flow Monitor (PLAN.md Section 4.4)
// - Monitors Continuous Flow allocations on @RJGC(ContinuousFlowWatch) annotated code
// - Tracks moving window allocation rates per thread
// - Detects sudden allocation surges and initiates rapid 256KB page rebates
// ============================================================================

struct RedjetTrafficSample {
  uint64_t timestamp_tsc{ 0 };
  size_t   bytes_allocated{ 0 };
  double   rate_mb_per_sec{ 0.0 };
  bool     surge_detected{ false };
};

class RedjetTrafficWatchdog {
public:
  static constexpr size_t MAX_TRACKED_THREADS   = 256;
  static constexpr double SURGE_THRESHOLD_MBPS   = 500.0; // 500 MB/s allocation surge

private:
  static RedjetTrafficWatchdog* _instance;
  static std::atomic<bool>      _initialized;

  struct ThreadFlowRecord {
    uint32_t thread_id{ 0 };
    uint64_t last_sample_tsc{ 0 };
    size_t   accumulated_bytes{ 0 };
    double   current_rate_mbps{ 0.0 };
    bool     is_flow_watched{ false };
  };

  ThreadFlowRecord         _threads[MAX_TRACKED_THREADS];
  hs_ticket_lock_t         _watchdog_lock;

  // Telemetry
  std::atomic<uint64_t>    _total_surges_detected{ 0 };
  std::atomic<uint64_t>    _total_rebates_triggered{ 0 };
  std::atomic<uint64_t>    _total_monitored_bytes{ 0 };

public:
  RedjetTrafficWatchdog();
  ~RedjetTrafficWatchdog();

  static void initialize();
  static void destroy();
  static RedjetTrafficWatchdog* get() noexcept { return _instance; }

  // 1. Thread Flow Registration
  RJ_API void set_thread_flow_watch(uint32_t thread_id, bool enable);

  // 2. Account Allocation and Detect Surge
  RJ_API bool record_allocation(uint32_t thread_id, size_t bytes);

  // Status queries
  double get_thread_rate_mbps(uint32_t thread_id) const noexcept;

  // Telemetry
  uint64_t surges_detected()   const noexcept { return _total_surges_detected.load(std::memory_order_relaxed); }
  uint64_t rebates_triggered() const noexcept { return _total_rebates_triggered.load(std::memory_order_relaxed); }
  uint64_t monitored_bytes()   const noexcept { return _total_monitored_bytes.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETTRAFFICWATCHDOG_HPP
