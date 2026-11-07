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

#include "gc/redjet/runtime/redjetGCPolicyCounters.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/runtime/redjetScheduler.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetGCMemoryManager.hpp"
#include "gc/redjet/runtime/redjetSnatchRebate.hpp"
#include "gc/redjet/engine/redjetSeasonalCoordinator.hpp"
#include "gc/shared/gc_globals.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/perfData.hpp"
#include "memory/resourceArea.hpp"
#endif

RedjetGCPolicyCounters::RedjetGCPolicyCounters(RedjetHeap* heap)
  : GCPolicyCounters("RedjetGC", 1, 1),
    _heap(heap),
    _throughput_ratio(nullptr),
    _pause_budget_ms(nullptr),
    _pause_actual_us(nullptr),
    _rss_bytes(nullptr),
    _allocation_rate_bytes_sec(nullptr),
    _duty_fusion_count(nullptr),
    _in_order_efficiency_ratio(nullptr),
    _mltc_congestion(nullptr),
    _rmrp_reclaimed_bytes(nullptr),
    _domestic_scheduler_active(nullptr),
    _season_ordinal(nullptr),
    _last_update_tsc(0),
    _last_allocated_bytes(0),
    _smoothed_alloc_rate(0.0),
    _smoothed_alloc_rate_short(0.0),
    _smoothed_alloc_rate_long(0.0),
    _cumulative_mutator_cycles(0),
    _cumulative_gc_cycles(0),
    _update_counter(0) {

#ifndef REDJET_STANDALONE_BUILD
  if (UsePerfData) {
    EXCEPTION_MARK;
    ResourceMark rm;

    const char* ns = PerfDataManager::name_space("gc.policy");

    _throughput_ratio = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "throughputRatio"),
      PerfData::U_None, (jlong)99, CHECK);

    _pause_budget_ms = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "pauseBudgetMs"),
      PerfData::U_Ticks, (jlong)MaxGCPauseMillis, CHECK);

    _pause_actual_us = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "pauseActualUs"),
      PerfData::U_Ticks, (jlong)0, CHECK);

    _rss_bytes = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "rssBytes"),
      PerfData::U_Bytes, (jlong)0, CHECK);

    _allocation_rate_bytes_sec = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "allocationRateBytesPerSec"),
      PerfData::U_Bytes, (jlong)0, CHECK);

    _duty_fusion_count = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "dutyFusionCount"),
      PerfData::U_Events, (jlong)0, CHECK);

    _in_order_efficiency_ratio = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "inOrderEfficiencyRatio"),
      PerfData::U_None, (jlong)100, CHECK);

    _mltc_congestion = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "mltcCongestionScore"),
      PerfData::U_None, (jlong)0, CHECK);

    _rmrp_reclaimed_bytes = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "rmrpReclaimedBytes"),
      PerfData::U_Bytes, (jlong)0, CHECK);

    _domestic_scheduler_active = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "domesticSchedulerActive"),
      PerfData::U_None, (jlong)1, CHECK);

    _season_ordinal = PerfDataManager::create_variable(
      SUN_GC, PerfDataManager::counter_name(ns, "seasonOrdinal"),
      PerfData::U_None, (jlong)0, CHECK);
  }
#endif
  _last_update_tsc = sacculus_rdtsc();
}

void RedjetGCPolicyCounters::update_counters() {
  if (_heap == nullptr) return;

  uint64_t now_tsc = sacculus_rdtsc();
  uint64_t delta_tsc = (now_tsc > _last_update_tsc) ? (now_tsc - _last_update_tsc) : 1;
  _last_update_tsc = now_tsc;
  _update_counter++;

  size_t current_used = _heap->used();

  // Compute allocation delta
  size_t delta_alloc = (current_used > _last_allocated_bytes) ? (current_used - _last_allocated_bytes) : 0;
  _last_allocated_bytes = current_used;

  // Approximate seconds assuming 3.0 GHz CPU
  double delta_seconds = static_cast<double>(delta_tsc) / 3000000000.0;
  double instantaneous_rate = (delta_seconds > 0.00001) ? (delta_alloc / delta_seconds) : 0.0;

  // Multi-tier exponential moving average:
  // Short-term (alpha = 0.3)
  if (_smoothed_alloc_rate_short <= 0.0) {
    _smoothed_alloc_rate_short = instantaneous_rate;
  } else {
    _smoothed_alloc_rate_short = 0.3 * instantaneous_rate + 0.7 * _smoothed_alloc_rate_short;
  }

  // Medium-term (alpha = 0.1)
  if (_smoothed_alloc_rate <= 0.0) {
    _smoothed_alloc_rate = instantaneous_rate;
  } else {
    _smoothed_alloc_rate = 0.1 * instantaneous_rate + 0.9 * _smoothed_alloc_rate;
  }

  // Long-term (alpha = 0.02)
  if (_smoothed_alloc_rate_long <= 0.0) {
    _smoothed_alloc_rate_long = instantaneous_rate;
  } else {
    _smoothed_alloc_rate_long = 0.02 * instantaneous_rate + 0.98 * _smoothed_alloc_rate_long;
  }

  // Dynamic mutator vs GC throughput calculation:
  // Mutator cycles vs GC pause cycles
  uint64_t pause_us = 0;
  if (_heap->gc_memory_manager() != nullptr) {
    pause_us = _heap->gc_memory_manager()->total_pause_time_us();
  }
  uint64_t pause_cycles = pause_us * 3000;
  _cumulative_gc_cycles = pause_cycles;

  uint64_t total_cycles = now_tsc;
  double throughput_ratio = 99.0;
  if (total_cycles > 0) {
    if (total_cycles > _cumulative_gc_cycles) {
      _cumulative_mutator_cycles = total_cycles - _cumulative_gc_cycles;
      throughput_ratio = (static_cast<double>(_cumulative_mutator_cycles) / static_cast<double>(total_cycles)) * 100.0;
    } else {
      throughput_ratio = 1.0;
    }
  }
  if (throughput_ratio < 1.0)  throughput_ratio = 1.0;
  if (throughput_ratio > 99.9) throughput_ratio = 99.9;

  jlong throughput_percent = static_cast<jlong>(throughput_ratio);

  // Snapshot recording
  _last_snapshot.timestamp_tsc = now_tsc;
  _last_snapshot.mutator_cycles = _cumulative_mutator_cycles;
  _last_snapshot.gc_cycles = _cumulative_gc_cycles;
  _last_snapshot.throughput_ratio_pct = throughput_ratio;
  _last_snapshot.current_rss_bytes = current_used;
  _last_snapshot.allocation_rate_mb_s = _smoothed_alloc_rate / (1024.0 * 1024.0);

  // Update PerfData variables
#ifndef REDJET_STANDALONE_BUILD
  if (_throughput_ratio != nullptr) {
    _throughput_ratio->set_value(throughput_percent);
  }
  if (_pause_actual_us != nullptr) {
    _pause_actual_us->set_value(static_cast<jlong>(pause_us));
  }
  if (_rss_bytes != nullptr) {
    _rss_bytes->set_value(static_cast<jlong>(current_used));
  }
  if (_allocation_rate_bytes_sec != nullptr) {
    _allocation_rate_bytes_sec->set_value(static_cast<jlong>(_smoothed_alloc_rate));
  }
  if (_duty_fusion_count != nullptr) {
    uint64_t fused = g_redjet_scheduler.domestic_scheduler().fusion_engine().total_fused_superduties();
    _duty_fusion_count->set_value(static_cast<jlong>(fused));
  }
  if (_in_order_efficiency_ratio != nullptr) {
    uint64_t dispatched = g_redjet_scheduler.domestic_scheduler().total_duties_dispatched();
    uint64_t violations = g_redjet_scheduler.domestic_scheduler().precedence_violations_prevented();
    jlong eff = 100;
    if (dispatched > 0) {
      double ratio = (1.0 - (static_cast<double>(violations) / static_cast<double>(dispatched))) * 100.0;
      eff = static_cast<jlong>(std::max(0.0, std::min(100.0, ratio)));
    }
    _in_order_efficiency_ratio->set_value(eff);
  }
  if (_domestic_scheduler_active != nullptr) {
    _domestic_scheduler_active->set_value(g_redjet_scheduler.is_domestic_active() ? 1 : 0);
  }
  if (_season_ordinal != nullptr) {
    _season_ordinal->set_value(static_cast<jlong>(RedjetSeasonalCoordinator::current_season()));
  }
#endif
}

void RedjetGCPolicyCounters::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet GC Policy & Runtime Throughput Telemetry ===");
  st->print_cr("  Mutator Throughput: %.2f %%", _last_snapshot.throughput_ratio_pct);
  st->print_cr("  Allocation Rate (Smoothed): %.2f MB/s", _smoothed_alloc_rate / (1024.0 * 1024.0));
  st->print_cr("  Allocation Rate (Short-term): %.2f MB/s", _smoothed_alloc_rate_short / (1024.0 * 1024.0));
  st->print_cr("  Allocation Rate (Long-term): %.2f MB/s", _smoothed_alloc_rate_long / (1024.0 * 1024.0));
  st->print_cr("  Cumulative Mutator Cycles: " UINT64_FORMAT, _cumulative_mutator_cycles);
  st->print_cr("  Cumulative GC Pause Cycles: " UINT64_FORMAT, _cumulative_gc_cycles);
  st->print_cr("  Domestic Scheduler: %s", g_redjet_scheduler.is_domestic_active() ? "ACTIVE (In-Order DSP)" : "QUIESCED");
  st->print_cr("  Current Season: %s", RedjetSeasonalCoordinator::season_name(RedjetSeasonalCoordinator::current_season()));
}
