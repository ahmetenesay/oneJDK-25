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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETPMUHUB_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETPMUHUB_HPP

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
// Redjet Hardware PMU Micro-Telemetry Hub (PLAN.md Section 8.8, 8.9)
// - Reads Hardware Performance Monitoring Counters (PMC / MSR)
// - Real-time IPC (Instructions Per Cycle), L1/L2/L3 miss rates, Memory Stalls
// - Informs Puzzler Mod 2 & MLTC of microarchitectural pipeline bottlenecks
// ============================================================================

struct RedjetPmuSnapshot {
  uint64_t instructions_retired{ 0 };
  uint64_t unhalted_cycles{ 0 };
  uint64_t l1_d_misses{ 0 };
  uint64_t l3_misses{ 0 };
  uint64_t memory_stall_cycles{ 0 };
  double   ipc{ 1.0 };
  double   l3_mpki{ 0.0 }; // L3 Misses per 1000 instructions
  uint64_t timestamp_tsc{ 0 };
};

class RedjetPmuHub {
private:
  static RedjetPmuHub*     _instance;
  static std::atomic<bool> _initialized;

  RedjetPmuSnapshot        _last_snapshot;
  hs_ticket_lock_t         _pmu_lock;
  bool                     _hardware_pmu_available{ false };

  // Telemetry
  std::atomic<uint64_t>    _total_pmu_samples{ 0 };
  std::atomic<uint64_t>    _total_high_stall_alerts{ 0 };

  void detect_pmu_hardware();

public:
  RedjetPmuHub();
  ~RedjetPmuHub();

  static void initialize();
  static void destroy();
  static RedjetPmuHub* get() noexcept { return _instance; }

  // 1. Sample Hardware PMU Counters
  RJ_API RedjetPmuSnapshot sample_counters();

  // 2. Query Current IPC & Memory Stall Level
  RJ_API double current_ipc() const noexcept;
  RJ_API bool   is_pipeline_memory_bound() const noexcept;

  // Status
  bool pmu_available() const noexcept { return _hardware_pmu_available; }

  // Telemetry
  uint64_t pmu_samples()       const noexcept { return _total_pmu_samples.load(std::memory_order_relaxed); }
  uint64_t high_stall_alerts() const noexcept { return _total_high_stall_alerts.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETPMUHUB_HPP
