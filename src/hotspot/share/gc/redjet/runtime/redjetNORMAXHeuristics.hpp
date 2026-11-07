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

#ifndef SHARE_GC_REDJET_NORMAX_REDJETNORMAXHEURISTICS_HPP
#define SHARE_GC_REDJET_NORMAX_REDJETNORMAXHEURISTICS_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "utilities/ostream.hpp"
#else
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

// ============================================================================
// RedjetNORMAXHeuristics (PLAN.md Section 7.5 & 8.10)
// Dynamic Vector Execution Mode Heuristics:
// - Exponential Moving Average (EMA) Allocation Slope Estimation
// - Predictive Time-to-Exhaustion (TTE) Modeling
// - Dynamic Elastic TLAB Sizing with Sigmoidal Smoothing
// - Runway Capacity Pressure & MLTC Traffic Trend Integration
// ============================================================================
class RedjetNORMAXHeuristics {
public:
  static constexpr size_t DEFAULT_MIN_TLAB_SIZE = 16 * 1024;        // 16 KB
  static constexpr size_t DEFAULT_MAX_TLAB_SIZE = 16 * 1024 * 1024; // 16 MB
  static constexpr double DEFAULT_EMA_ALPHA     = 0.25;             // 25% weight on current sample
  static constexpr double SAFETY_HEADROOM_RATIO = 1.25;             // 25% runway safety margin

private:
  std::atomic<size_t>   _allocated_bytes{ 0 };
  std::atomic<uint64_t> _cycle_count{ 0 };
  size_t                _min_tlab_size{ DEFAULT_MIN_TLAB_SIZE };
  size_t                _max_tlab_size{ DEFAULT_MAX_TLAB_SIZE };
  size_t                _current_elastic_tlab_size{ 1024 * 1024 };

  // Telemetry & Rate Estimation
  std::atomic<uint64_t> _last_sample_tsc{ 0 };
  std::atomic<size_t>   _last_sample_bytes{ 0 };
  double                _smoothed_alloc_rate_bytes_sec{ 0.0 };
  double                _estimated_gc_duration_ms{ 2.5 };
  double                _target_tlab_refill_interval_ms{ 10.0 };

  // Dynamic Thresholds
  double                _runway_occupancy_ratio{ 0.0 };
  size_t                _runway_capacity_bytes{ 128 * 1024 * 1024 };
  size_t                _runway_used_bytes{ 0 };
  bool                  _elastic_tlab_enabled{ true };
  bool                  _mltc_trend_override{ false };

  // Performance telemetry counters
  std::atomic<uint64_t> _evaluations_count{ 0 };
  std::atomic<uint64_t> _triggers_from_mltc{ 0 };
  std::atomic<uint64_t> _triggers_from_tte{ 0 };
  std::atomic<uint64_t> _triggers_from_capacity{ 0 };

public:
  RedjetNORMAXHeuristics();
  virtual ~RedjetNORMAXHeuristics() = default;

  virtual const char* name() const { return "NORMAX"; }
  virtual bool should_start_gc() const;
  virtual bool is_dynamic_growth_allowed() const { return true; }
  virtual size_t max_tlab_size() const { return _max_tlab_size; }
  virtual size_t min_tlab_size() const { return _min_tlab_size; }
  virtual size_t current_elastic_tlab_size() const { return _current_elastic_tlab_size; }

  // Allocation & Cycle Tracking
  virtual void record_allocation(size_t bytes);
  virtual void record_gc_start();
  virtual void record_gc_end();
  virtual void update_runway_metrics(size_t used_bytes, size_t capacity_bytes);

  // Elastic TLAB Sizing Model
  size_t compute_elastic_tlab_size(uint32_t active_mutator_threads) const;
  void recompute_heuristics(uint32_t active_mutator_threads);

  // Predictive Models
  double smoothed_alloc_rate_bytes_sec() const noexcept { return _smoothed_alloc_rate_bytes_sec; }
  double estimated_time_to_exhaustion_ms() const;
  double estimated_gc_duration_ms() const noexcept { return _estimated_gc_duration_ms; }

  // Configuration
  void set_elastic_tlab_enabled(bool enabled) noexcept { _elastic_tlab_enabled = enabled; }
  void set_tlab_bounds(size_t min_sz, size_t max_sz) noexcept;
  void set_target_refill_interval_ms(double interval_ms) noexcept { _target_tlab_refill_interval_ms = interval_ms; }

  // Telemetry
  uint64_t cycle_count() const noexcept { return _cycle_count.load(std::memory_order_relaxed); }
  uint64_t evaluations_count() const noexcept { return _evaluations_count.load(std::memory_order_relaxed); }
  uint64_t triggers_from_mltc() const noexcept { return _triggers_from_mltc.load(std::memory_order_relaxed); }
  uint64_t triggers_from_tte() const noexcept { return _triggers_from_tte.load(std::memory_order_relaxed); }
  uint64_t triggers_from_capacity() const noexcept { return _triggers_from_capacity.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_NORMAX_REDJETNORMAXHEURISTICS_HPP
