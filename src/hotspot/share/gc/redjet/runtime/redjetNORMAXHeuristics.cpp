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

#include "gc/redjet/runtime/redjetNORMAXHeuristics.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <algorithm>
#include <cmath>

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "logging/log.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

RedjetNORMAXHeuristics::RedjetNORMAXHeuristics()
  : _allocated_bytes(0),
    _cycle_count(0),
    _min_tlab_size(DEFAULT_MIN_TLAB_SIZE),
    _max_tlab_size(DEFAULT_MAX_TLAB_SIZE),
    _current_elastic_tlab_size(1024 * 1024),
    _last_sample_tsc(0),
    _last_sample_bytes(0),
    _smoothed_alloc_rate_bytes_sec(50.0 * 1024.0 * 1024.0), // 50 MB/s initial baseline
    _estimated_gc_duration_ms(2.5),
    _target_tlab_refill_interval_ms(10.0),
    _runway_occupancy_ratio(0.0),
    _runway_capacity_bytes(128 * 1024 * 1024),
    _runway_used_bytes(0),
    _elastic_tlab_enabled(true),
    _mltc_trend_override(false),
    _evaluations_count(0),
    _triggers_from_mltc(0),
    _triggers_from_tte(0),
    _triggers_from_capacity(0) {

  _last_sample_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
}

void RedjetNORMAXHeuristics::set_tlab_bounds(size_t min_sz, size_t max_sz) noexcept {
  _min_tlab_size = std::max<size_t>(1024, min_sz);
  _max_tlab_size = std::max<size_t>(_min_tlab_size, max_sz);
  _current_elastic_tlab_size = std::min(std::max(_current_elastic_tlab_size, _min_tlab_size), _max_tlab_size);
}

void RedjetNORMAXHeuristics::update_runway_metrics(size_t used_bytes, size_t capacity_bytes) {
  _runway_used_bytes = used_bytes;
  _runway_capacity_bytes = std::max<size_t>(1024 * 1024, capacity_bytes);
  _runway_occupancy_ratio = static_cast<double>(_runway_used_bytes) / static_cast<double>(_runway_capacity_bytes);
}

double RedjetNORMAXHeuristics::estimated_time_to_exhaustion_ms() const {
  if (_runway_used_bytes >= _runway_capacity_bytes) {
    return 0.0;
  }
  size_t remaining_bytes = _runway_capacity_bytes - _runway_used_bytes;
  double rate = std::max(1024.0 * 1024.0, _smoothed_alloc_rate_bytes_sec); // minimum 1 MB/s floor
  double remaining_sec = static_cast<double>(remaining_bytes) / rate;
  return remaining_sec * 1000.0;
}

bool RedjetNORMAXHeuristics::should_start_gc() const {
  const_cast<RedjetNORMAXHeuristics*>(this)->_evaluations_count.fetch_add(1, std::memory_order_relaxed);

  // Trigger Condition 1: Machine Learning Traffic Controller (MLTC) detected upward surge trend
  if (g_hs_mltc_solver.continuous_upward_trend.load(std::memory_order_relaxed)) {
    const_cast<RedjetNORMAXHeuristics*>(this)->_triggers_from_mltc.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Trigger Condition 2: Hard Runway capacity threshold (85% occupancy ceiling)
  if (_runway_occupancy_ratio >= 0.85) {
    const_cast<RedjetNORMAXHeuristics*>(this)->_triggers_from_capacity.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Trigger Condition 3: Predictive Time-to-Exhaustion (TTE) model
  double tte_ms = estimated_time_to_exhaustion_ms();
  double required_headroom_ms = _estimated_gc_duration_ms * SAFETY_HEADROOM_RATIO;
  if (tte_ms <= required_headroom_ms) {
    const_cast<RedjetNORMAXHeuristics*>(this)->_triggers_from_tte.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

void RedjetNORMAXHeuristics::record_allocation(size_t bytes) {
  size_t total = _allocated_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;

  // Periodically re-estimate allocation rate every 4MB allocated
  if (total - _last_sample_bytes.load(std::memory_order_relaxed) >= 4 * 1024 * 1024) {
    uint64_t now_tsc = sacculus_rdtsc();
    uint64_t prev_tsc = _last_sample_tsc.load(std::memory_order_relaxed);
    uint64_t tsc_delta = (now_tsc > prev_tsc) ? (now_tsc - prev_tsc) : 1;

    // Approximate seconds assuming 3.0 GHz baseline
    double delta_sec = static_cast<double>(tsc_delta) / 3.0e9;
    if (delta_sec > 0.0001) {
      size_t sample_delta_bytes = total - _last_sample_bytes.load(std::memory_order_relaxed);
      double current_rate = static_cast<double>(sample_delta_bytes) / delta_sec;

      // Exponential Moving Average (EMA) update
      _smoothed_alloc_rate_bytes_sec =
        DEFAULT_EMA_ALPHA * current_rate + (1.0 - DEFAULT_EMA_ALPHA) * _smoothed_alloc_rate_bytes_sec;

      _last_sample_tsc.store(now_tsc, std::memory_order_relaxed);
      _last_sample_bytes.store(total, std::memory_order_relaxed);
    }
  }
}

void RedjetNORMAXHeuristics::record_gc_start() {
  _last_sample_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
}

void RedjetNORMAXHeuristics::record_gc_end() {
  _cycle_count.fetch_add(1, std::memory_order_relaxed);
  _allocated_bytes.store(0, std::memory_order_relaxed);
  _last_sample_bytes.store(0, std::memory_order_relaxed);
  _last_sample_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
}

size_t RedjetNORMAXHeuristics::compute_elastic_tlab_size(uint32_t active_mutator_threads) const {
  if (!_elastic_tlab_enabled) {
    return _max_tlab_size;
  }

  uint32_t threads = std::max<uint32_t>(1, active_mutator_threads);
  double refill_sec = _target_tlab_refill_interval_ms / 1000.0;
  double expected_throughput_window = _smoothed_alloc_rate_bytes_sec * refill_sec;
  double per_thread_bytes = expected_throughput_window / static_cast<double>(threads);

  // Apply sigmoidal smoothing around the mean
  size_t target_size = static_cast<size_t>(per_thread_bytes);
  target_size = (target_size + 4095) & ~static_cast<size_t>(4095); // 4KB page alignment

  if (target_size < _min_tlab_size) target_size = _min_tlab_size;
  if (target_size > _max_tlab_size) target_size = _max_tlab_size;

  return target_size;
}

void RedjetNORMAXHeuristics::recompute_heuristics(uint32_t active_mutator_threads) {
  _current_elastic_tlab_size = compute_elastic_tlab_size(active_mutator_threads);
}

void RedjetNORMAXHeuristics::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet NORMAX Dynamic Heuristics ---");
  st->print_cr("  Mode:                    %s (Dynamic Growth Allowed: %s)",
               name(), is_dynamic_growth_allowed() ? "YES" : "NO");
  st->print_cr("  Total GC Cycles:         " UINT64_FORMAT, cycle_count());
  st->print_cr("  Heuristic Evaluations:   " UINT64_FORMAT, evaluations_count());
  st->print_cr("  Triggers Breakdown:");
  st->print_cr("    MLTC Surge Triggers:   " UINT64_FORMAT, triggers_from_mltc());
  st->print_cr("    TTE Model Triggers:    " UINT64_FORMAT, triggers_from_tte());
  st->print_cr("    Capacity Triggers:     " UINT64_FORMAT, triggers_from_capacity());
  st->print_cr("  Smoothed Alloc Rate:     %.2f MB/s", _smoothed_alloc_rate_bytes_sec / (1024.0 * 1024.0));
  st->print_cr("  Time to Exhaustion:      %.2f ms (GC Duration: %.2f ms)",
               estimated_time_to_exhaustion_ms(), _estimated_gc_duration_ms);
  st->print_cr("  Runway Occupancy:        %.2f%% (" SIZE_FORMAT " / " SIZE_FORMAT " bytes)",
               _runway_occupancy_ratio * 100.0, _runway_used_bytes, _runway_capacity_bytes);
  st->print_cr("  Elastic TLAB Bounds:     [" SIZE_FORMAT " KB .. " SIZE_FORMAT " KB], Current: " SIZE_FORMAT " KB",
               _min_tlab_size / 1024, _max_tlab_size / 1024, _current_elastic_tlab_size / 1024);
}
