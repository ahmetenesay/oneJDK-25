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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETGCPOLICYCOUNTERS_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETGCPOLICYCOUNTERS_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef RJ_API
#if defined(_WIN32) || defined(__CYGWIN__)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/gcPolicyCounters.hpp"
#include "runtime/perfData.hpp"
#include "utilities/ostream.hpp"
#else
class GCPolicyCounters {
public:
  GCPolicyCounters(const char* name, int c, int g) {}
};
class PerfVariable {
public:
  void set_value(jlong val) {}
};
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;

// ============================================================================
// RedjetGCPolicyCounters (PLAN.md Section 8.10.7)
// Shared PerfData runtime throughput, pause budget, RSS telemetry,
// and dynamic mutator vs GC throughput ratio calculation.
// ============================================================================

struct alignas(64) RedjetThroughputSnapshot {
  uint64_t timestamp_tsc{ 0 };
  uint64_t mutator_cycles{ 0 };
  uint64_t gc_cycles{ 0 };
  double   throughput_ratio_pct{ 99.0 };
  size_t   current_rss_bytes{ 0 };
  double   allocation_rate_mb_s{ 0.0 };
};

class RedjetGCPolicyCounters : public GCPolicyCounters {
private:
  RedjetHeap*   _heap;

  // Telemetry PerfData Variables
  PerfVariable* _throughput_ratio;
  PerfVariable* _pause_budget_ms;
  PerfVariable* _pause_actual_us;
  PerfVariable* _rss_bytes;
  PerfVariable* _allocation_rate_bytes_sec;
  PerfVariable* _duty_fusion_count;
  PerfVariable* _in_order_efficiency_ratio;
  PerfVariable* _mltc_congestion;
  PerfVariable* _rmrp_reclaimed_bytes;
  PerfVariable* _domestic_scheduler_active;
  PerfVariable* _season_ordinal;

  uint64_t      _last_update_tsc{ 0 };
  size_t        _last_allocated_bytes{ 0 };
  double        _smoothed_alloc_rate{ 0.0 };
  double        _smoothed_alloc_rate_short{ 0.0 };
  double        _smoothed_alloc_rate_long{ 0.0 };
  uint64_t      _cumulative_mutator_cycles{ 0 };
  uint64_t      _cumulative_gc_cycles{ 0 };
  uint64_t      _update_counter{ 0 };

  RedjetThroughputSnapshot _last_snapshot;

public:
  RedjetGCPolicyCounters(RedjetHeap* heap);
  virtual ~RedjetGCPolicyCounters() = default;

  // Real-time telemetry refresh and mutator vs GC throughput calculation
  RJ_API void update_counters();

  // Record GC cycle cost for exact throughput accounting
  RJ_API void record_gc_cycles(uint64_t gc_cycles) {
    _cumulative_gc_cycles += gc_cycles;
  }

  // Record mutator cycle execution
  RJ_API void record_mutator_cycles(uint64_t mutator_cycles) {
    _cumulative_mutator_cycles += mutator_cycles;
  }

  double smoothed_alloc_rate() const noexcept { return _smoothed_alloc_rate; }
  double smoothed_alloc_rate_short() const noexcept { return _smoothed_alloc_rate_short; }
  double smoothed_alloc_rate_long() const noexcept { return _smoothed_alloc_rate_long; }
  double last_throughput_ratio() const noexcept { return _last_snapshot.throughput_ratio_pct; }

  const RedjetThroughputSnapshot& last_snapshot() const noexcept { return _last_snapshot; }

  RJ_API void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETGCPOLICYCOUNTERS_HPP
