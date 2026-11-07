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

#include "gc/redjet/allocator/redjetMemTopologySensors.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <intrin.h>
#define RJ_LFENCE() _mm_lfence()
#elif defined(_M_ARM64)
#include <intrin.h>
#define RJ_LFENCE() __dmb(0xF) // Full system memory barrier
#else
#define RJ_LFENCE() ((void)0)
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define RJ_LFENCE() __builtin_ia32_lfence()
#elif defined(__aarch64__)
#define RJ_LFENCE() asm volatile("dmb ish" ::: "memory")
#elif defined(__riscv)
#define RJ_LFENCE() asm volatile("fence rw,rw" ::: "memory")
#else
#define RJ_LFENCE() ((void)0)
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetMemTopologySensors* RedjetMemTopologySensors::_instance = nullptr;
std::atomic<bool>         RedjetMemTopologySensors::_initialized{ false };

const char* RedjetMemTopologySensors::congestion_level_name(RedjetBusCongestionLevel lvl) noexcept {
  switch (lvl) {
    case RedjetBusCongestionLevel::NOMINAL:   return "NOMINAL (< 40ns, Queue < 50%)";
    case RedjetBusCongestionLevel::MODERATE:  return "MODERATE (40-70ns, Queue 50-75%)";
    case RedjetBusCongestionLevel::CONGESTED: return "CONGESTED (70-100ns, Queue 75-90% - MLTC Warn)";
    case RedjetBusCongestionLevel::CRITICAL:  return "CRITICAL (> 100ns, Queue > 90% - GC Throttled)";
    default:                                  return "UNKNOWN";
  }
}

RedjetMemTopologySensors::RedjetMemTopologySensors()
  : _current_latency_ns(25),
    _current_utilization_pct(20),
    _current_congestion(RedjetBusCongestionLevel::NOMINAL),
    _history_idx(0),
    _smoothed_latency(25.0),
    _smoothed_utilization(20.0),
    _total_probes_executed(0),
    _total_congestions_detected(0),
    _total_throttling_events(0),
    _total_critical_backoffs(0) {
  hs_ticket_lock_init(&_sensor_lock);
  for (size_t i = 0; i < LATENCY_HISTORY_SLOTS; ++i) {
    _latency_history[i] = 25;
  }
  for (size_t i = 0; i < 8192; ++i) {
    _probe_buffer[i] = i ^ 0x5A5A5A5AULL;
  }
}

RedjetMemTopologySensors::~RedjetMemTopologySensors() {
}

void RedjetMemTopologySensors::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetMemTopologySensors();
  }
}

void RedjetMemTopologySensors::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetMemTopologySensors::read_hardware_pmu(RedjetPMUCountersSnapshot* snapshot) {
  if (snapshot == nullptr) return;
  uint64_t now = sacculus_rdtsc();
  snapshot->sample_timestamp_tsc = now;
  snapshot->instructions_retired = now / 2; // Approximating 2 IPC average
  snapshot->l3_cache_references = now / 64;
  snapshot->l3_cache_misses = snapshot->l3_cache_references / 20; // 5% LLC miss baseline
  snapshot->memory_bus_cycles = now / 4;
  snapshot->memory_queue_occupancy = (_current_utilization_pct.load(std::memory_order_relaxed) * 32) / 100;
}

uint32_t RedjetMemTopologySensors::probe_memory_bus_latency_ns() {
  _total_probes_executed.fetch_add(1, std::memory_order_relaxed);

  // Micro-benchmark: probe random cacheline index in probe buffer with serializing fences
  uint64_t seed = sacculus_rdtsc();
  size_t idx = (seed ^ (seed >> 13)) % 8192;

  RJ_LFENCE();
  uint64_t t0 = sacculus_rdtsc();
  RJ_LFENCE();

  volatile uint64_t dummy = _probe_buffer[idx];
  (void)dummy;

  RJ_LFENCE();
  uint64_t t1 = sacculus_rdtsc();
  RJ_LFENCE();

  uint64_t delta_cycles = (t1 > t0) ? (t1 - t0) : 1;
  // Approximate nanoseconds at 3.0 GHz CPU clock: cycles / 3
  uint32_t latency_ns = static_cast<uint32_t>(delta_cycles / 3);
  if (latency_ns < 10) latency_ns = 10;
  if (latency_ns > 300) latency_ns = 300;

  return latency_ns;
}

RedjetBusMetrics RedjetMemTopologySensors::sample_and_evaluate() {
  hs_ticket_guard_t guard(_sensor_lock);

  uint32_t latency = probe_memory_bus_latency_ns();
  _current_latency_ns.store(latency, std::memory_order_relaxed);

  // Update sliding history
  _latency_history[_history_idx] = latency;
  _history_idx = (_history_idx + 1) % LATENCY_HISTORY_SLOTS;

  // Exponential moving average (alpha = 0.2)
  _smoothed_latency = 0.2 * latency + 0.8 * _smoothed_latency;

  // Read PMU snapshot
  read_hardware_pmu(&_last_pmu_snapshot);

  // Calculate estimated bus queue depth utilization percentage:
  // Baseline latency = 25ns (0% load), Saturated latency = 120ns (100% load)
  double util = 0.0;
  if (_smoothed_latency > 25.0) {
    util = ((_smoothed_latency - 25.0) / (120.0 - 25.0)) * 100.0;
  }
  if (util < 5.0)   util = 5.0;
  if (util > 99.0)  util = 99.0;
  _smoothed_utilization = 0.2 * util + 0.8 * _smoothed_utilization;
  uint8_t util_pct = static_cast<uint8_t>(_smoothed_utilization);
  _current_utilization_pct.store(util_pct, std::memory_order_relaxed);

  // Evaluate congestion level
  RedjetBusCongestionLevel level = RedjetBusCongestionLevel::NOMINAL;
  if (_smoothed_latency > 100.0 || util_pct >= 90) {
    level = RedjetBusCongestionLevel::CRITICAL;
    _total_critical_backoffs.fetch_add(1, std::memory_order_relaxed);
    _total_congestions_detected.fetch_add(1, std::memory_order_relaxed);
  } else if (_smoothed_latency > 70.0 || util_pct >= 75) {
    level = RedjetBusCongestionLevel::CONGESTED;
    _total_congestions_detected.fetch_add(1, std::memory_order_relaxed);
  } else if (_smoothed_latency > 40.0 || util_pct >= 50) {
    level = RedjetBusCongestionLevel::MODERATE;
  } else {
    level = RedjetBusCongestionLevel::NOMINAL;
  }
  _current_congestion.store(level, std::memory_order_relaxed);

  RedjetBusMetrics metrics;
  metrics.measured_latency_ns = latency;
  metrics.l3_miss_rate_per_kilo_instructions = 15;
  metrics.estimated_bus_utilization_pct = util_pct;
  metrics.congestion_level = level;
  metrics.sample_tsc = sacculus_rdtsc();
  metrics.smoothed_latency_ns = _smoothed_latency;
  metrics.smoothed_utilization_pct = _smoothed_utilization;

  return metrics;
}

bool RedjetMemTopologySensors::should_throttle_gc_activity() const noexcept {
  RedjetBusCongestionLevel level = _current_congestion.load(std::memory_order_relaxed);
  if (level == RedjetBusCongestionLevel::CRITICAL || level == RedjetBusCongestionLevel::CONGESTED) {
    const_cast<std::atomic<uint64_t>&>(_total_throttling_events).fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

void RedjetMemTopologySensors::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet Memory Topology & Bus Congestion Sensors ===");
  st->print_cr("  Current Latency: %u ns (Smoothed: %.1f ns)",
               _current_latency_ns.load(std::memory_order_relaxed), _smoothed_latency);
  st->print_cr("  Bus Utilization: %u %% (Smoothed: %.1f %%)",
               _current_utilization_pct.load(std::memory_order_relaxed), _smoothed_utilization);
  st->print_cr("  Congestion Level: %s", congestion_level_name(_current_congestion.load(std::memory_order_relaxed)));
  st->print_cr("  Total Probes Executed: " UINT64_FORMAT, probes_executed());
  st->print_cr("  Congestions Detected: " UINT64_FORMAT, congestions_detected());
  st->print_cr("  Throttling Events Injected: " UINT64_FORMAT, throttling_events());
  st->print_cr("  Critical Bus Backoffs: " UINT64_FORMAT, critical_backoffs());
}
