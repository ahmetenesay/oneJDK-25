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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETMEMTOPOLOGYSENSORS_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETMEMTOPOLOGYSENSORS_HPP

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
// Memory Topology PMU Sensors & Bus Congestion Watchdog (PLAN.md Section 4.4, 8.9)
// - Real-time monitoring of L3 cache miss rate, memory bus queue depth, and UPI/Fabric
// - High-precision memory latency probes via LFENCE+RDTSC
// - Proactive MLTC bus congestion alerts and GC allocation throttling
// - Dynamic mutator protection: throttles background GC threads during bus peaks
// ============================================================================

enum class RedjetBusCongestionLevel : uint8_t {
  NOMINAL     = 0, // Bus latency < 40ns, queue utilization < 50%
  MODERATE    = 1, // Bus latency 40-70ns, queue utilization 50-75%
  CONGESTED   = 2, // Bus latency 70-100ns, queue utilization 75-90% (warn MLTC)
  CRITICAL    = 3  // Bus latency > 100ns, queue utilization > 90% (throttle GC)
};

struct RedjetBusMetrics {
  uint32_t                 measured_latency_ns{ 0 };
  uint32_t                 l3_miss_rate_per_kilo_instructions{ 0 };
  uint8_t                  estimated_bus_utilization_pct{ 0 };
  RedjetBusCongestionLevel congestion_level{ RedjetBusCongestionLevel::NOMINAL };
  uint64_t                 sample_tsc{ 0 };
  double                   smoothed_latency_ns{ 25.0 };
  double                   smoothed_utilization_pct{ 20.0 };
};

struct RedjetPMUCountersSnapshot {
  uint64_t l3_cache_references{ 0 };
  uint64_t l3_cache_misses{ 0 };
  uint64_t memory_bus_cycles{ 0 };
  uint64_t memory_queue_occupancy{ 0 };
  uint64_t instructions_retired{ 0 };
  uint64_t sample_timestamp_tsc{ 0 };
};

class RedjetMemTopologySensors {
public:
  static constexpr size_t LATENCY_HISTORY_SLOTS = 64;

private:
  static RedjetMemTopologySensors* _instance;
  static std::atomic<bool>         _initialized;

  std::atomic<uint32_t>            _current_latency_ns{ 25 };
  std::atomic<uint8_t>             _current_utilization_pct{ 20 };
  std::atomic<RedjetBusCongestionLevel> _current_congestion{ RedjetBusCongestionLevel::NOMINAL };

  // Sliding latency sample history
  uint32_t                         _latency_history[LATENCY_HISTORY_SLOTS];
  size_t                           _history_idx{ 0 };
  double                           _smoothed_latency{ 25.0 };
  double                           _smoothed_utilization{ 20.0 };

  // Watchdog telemetry
  std::atomic<uint64_t>            _total_probes_executed{ 0 };
  std::atomic<uint64_t>            _total_congestions_detected{ 0 };
  std::atomic<uint64_t>            _total_throttling_events{ 0 };
  std::atomic<uint64_t>            _total_critical_backoffs{ 0 };

  hs_ticket_lock_t                 _sensor_lock;

  // Internal calibration buffer for probe reads (64KB aligned)
  alignas(64) uint64_t             _probe_buffer[8192];

  RedjetPMUCountersSnapshot        _last_pmu_snapshot;

  void read_hardware_pmu(RedjetPMUCountersSnapshot* snapshot);

public:
  RedjetMemTopologySensors();
  ~RedjetMemTopologySensors();

  static void initialize();
  static void destroy();
  static RedjetMemTopologySensors* get() noexcept { return _instance; }

  // 1. Hardware Memory Bus Latency Probe (LFENCE + Read + LFENCE + RDTSC)
  RJ_API uint32_t probe_memory_bus_latency_ns();

  // 2. Refresh Metrics & Evaluate Congestion
  RJ_API RedjetBusMetrics sample_and_evaluate();

  // 3. Proactive Throttling Query
  // Returns true if background GC threads should back off to avoid starving mutators
  RJ_API bool should_throttle_gc_activity() const noexcept;

  // 4. Memory Bus Congestion Level Query
  static const char* congestion_level_name(RedjetBusCongestionLevel lvl) noexcept;

  // Metric queries
  uint32_t current_latency_ns() const noexcept { return _current_latency_ns.load(std::memory_order_relaxed); }
  uint8_t  current_utilization_pct() const noexcept { return _current_utilization_pct.load(std::memory_order_relaxed); }
  RedjetBusCongestionLevel congestion_level() const noexcept { return _current_congestion.load(std::memory_order_relaxed); }
  double   smoothed_latency_ns() const noexcept { return _smoothed_latency; }

  // Telemetry
  uint64_t probes_executed()     const noexcept { return _total_probes_executed.load(std::memory_order_relaxed); }
  uint64_t congestions_detected()const noexcept { return _total_congestions_detected.load(std::memory_order_relaxed); }
  uint64_t throttling_events()   const noexcept { return _total_throttling_events.load(std::memory_order_relaxed); }
  uint64_t critical_backoffs()   const noexcept { return _total_critical_backoffs.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETMEMTOPOLOGYSENSORS_HPP
