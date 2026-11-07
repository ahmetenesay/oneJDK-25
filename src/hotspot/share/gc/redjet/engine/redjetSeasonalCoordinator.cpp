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

#include "gc/redjet/engine/redjetSeasonalCoordinator.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetLog.hpp"
#include "runtime/os.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

// Static state initialization
alignas(64) std::atomic<RedjetSeason>        RedjetSeasonalCoordinator::_current_season{ RedjetSeason::SPRING_WARMUP };
alignas(64) std::atomic<RedjetInspectionMode> RedjetSeasonalCoordinator::_current_inspection_mode{ RedjetInspectionMode::SCAN_AND_RECONNAISSANCE };
alignas(64) std::atomic<uint64_t>            RedjetSeasonalCoordinator::_current_epoch{ 0 };
alignas(64) std::atomic<uint64_t>            RedjetSeasonalCoordinator::_last_activity_tsc{ 0 };
alignas(64) std::atomic<uint64_t>            RedjetSeasonalCoordinator::_season_entry_epoch{ 0 };
alignas(64) std::atomic<uint64_t>            RedjetSeasonalCoordinator::_season_entry_tsc{ 0 };
alignas(64) std::atomic<size_t>              RedjetSeasonalCoordinator::_recent_allocation_bytes{ 0 };
alignas(64) std::atomic<size_t>              RedjetSeasonalCoordinator::_baseline_allocation_velocity{ 100 * 1024 * 1024 }; // 100 MB/s baseline

RedjetSeasonConfig   RedjetSeasonalCoordinator::_config;
RedjetSeasonalMetrics RedjetSeasonalCoordinator::_metrics;
RedjetMarkovMatrix   RedjetSeasonalCoordinator::_markov;
RedjetBayesianBelief RedjetSeasonalCoordinator::_belief;
std::atomic<bool>    RedjetSeasonalCoordinator::_initialized{ false };

alignas(64) std::atomic<uint32_t> RedjetSeasonalCoordinator::_recent_transition_count{ 0 };
alignas(64) std::atomic<uint64_t> RedjetSeasonalCoordinator::_last_transition_epoch{ 0 };

RedjetQuarantineRecord RedjetSeasonalCoordinator::_quarantine_table[QUARANTINE_SLOTS];
alignas(64) std::atomic<size_t> RedjetSeasonalCoordinator::_quarantine_head{ 0 };
alignas(64) std::atomic<size_t> RedjetSeasonalCoordinator::_quarantine_active_count{ 0 };

const char* RedjetSeasonalCoordinator::season_name(RedjetSeason season) noexcept {
  switch (season) {
    case RedjetSeason::SPRING_WARMUP:          return "Spring (Warmup & Reconnaissance)";
    case RedjetSeason::SUMMER_STEADY_STATE:    return "Summer (Steady-State & 99% TailTraces)";
    case RedjetSeason::AUTUMN_DIMENSION_SHIFT: return "Autumn (Dimension Shift & Full Scan Resurgence)";
    case RedjetSeason::WINTER_IDLE_REBATE:     return "Winter (AFK / Idle & Deep OS Rebate)";
    default:                                   return "Unknown Season";
  }
}

const char* RedjetSeasonalCoordinator::inspection_mode_name(RedjetInspectionMode mode) noexcept {
  switch (mode) {
    case RedjetInspectionMode::SCAN_AND_RECONNAISSANCE:  return "Scan & Reconnaissance";
    case RedjetInspectionMode::MARK_AND_EXECUTION_CLASP: return "Mark & Execution Clasping";
    case RedjetInspectionMode::HYBRID_COOPERATIVE:       return "Hybrid Cooperative";
    default:                                            return "Unknown Inspection Mode";
  }
}

void RedjetSeasonalCoordinator::initialize() {
  if (_initialized.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  uint64_t now_tsc = sacculus_rdtsc();
  _current_season.store(RedjetSeason::SPRING_WARMUP, std::memory_order_release);
  _current_inspection_mode.store(RedjetInspectionMode::SCAN_AND_RECONNAISSANCE, std::memory_order_release);
  _current_epoch.store(0, std::memory_order_release);
  _season_entry_epoch.store(0, std::memory_order_release);
  _season_entry_tsc.store(now_tsc, std::memory_order_release);
  _last_activity_tsc.store(now_tsc, std::memory_order_release);
  _recent_allocation_bytes.store(0, std::memory_order_release);
  _recent_transition_count.store(0, std::memory_order_release);
  _last_transition_epoch.store(0, std::memory_order_release);

  _metrics.current_accordion_scan_depth.store(1.0, std::memory_order_release);
  _metrics.current_bpu_accuracy.store(0.99, std::memory_order_release);

  // Initialize quarantine table
  for (size_t i = 0; i < QUARANTINE_SLOTS; ++i) {
    _quarantine_table[i].obj_address = 0;
    _quarantine_table[i].size_bytes = 0;
    _quarantine_table[i].condemned_epoch = 0;
    _quarantine_table[i].condemned_tsc = 0;
    _quarantine_table[i].origin_method_id = 0;
    _quarantine_table[i].is_rehabilitated = false;
    _quarantine_table[i].is_committed_evicted = false;
    _quarantine_table[i].active = false;
  }
  _quarantine_head.store(0, std::memory_order_release);
  _quarantine_active_count.store(0, std::memory_order_release);

  redjet_hotlog("[SEASON] RedjetSeasonalCoordinator initialized in SPRING_WARMUP season.");
}

void RedjetSeasonalCoordinator::shutdown() {
  _initialized.store(false, std::memory_order_release);
  redjet_hotlog("[SEASON] RedjetSeasonalCoordinator shutdown completed.");
}

bool RedjetSeasonalCoordinator::is_transition_dampened(RedjetSeason target_season) noexcept {
  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t entry_epoch = _season_entry_epoch.load(std::memory_order_relaxed);
  uint64_t dwell = (epoch >= entry_epoch) ? (epoch - entry_epoch) : 0;

  // Emergency dimension shifts from external signals (autumn) and mutator wakeups from Winter are NEVER suppressed
  if (target_season == RedjetSeason::AUTUMN_DIMENSION_SHIFT) {
    return false;
  }
  if (_current_season.load(std::memory_order_relaxed) == RedjetSeason::WINTER_IDLE_REBATE &&
      target_season == RedjetSeason::SPRING_WARMUP) {
    return false;
  }

  // If we haven't stayed long enough in the current season, suppress flapping
  if (dwell < _config.hysteresis_min_dwell_epochs) {
    _metrics.hysteresis_suppressed_flaps.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

void RedjetSeasonalCoordinator::transition_to_spring() {
  if (is_transition_dampened(RedjetSeason::SPRING_WARMUP)) {
    return;
  }

  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t tsc = sacculus_rdtsc();

  _current_season.store(RedjetSeason::SPRING_WARMUP, std::memory_order_release);
  _current_inspection_mode.store(RedjetInspectionMode::SCAN_AND_RECONNAISSANCE, std::memory_order_release);
  _season_entry_epoch.store(epoch, std::memory_order_release);
  _season_entry_tsc.store(tsc, std::memory_order_release);
  _last_transition_epoch.store(epoch, std::memory_order_release);
  _recent_transition_count.fetch_add(1, std::memory_order_relaxed);

  _metrics.spring_cycles.fetch_add(1, std::memory_order_relaxed);
  _metrics.scan_recon_invocations.fetch_add(1, std::memory_order_relaxed);
  _metrics.current_accordion_scan_depth.store(1.0, std::memory_order_release);

  recalibrate_markov_matrix();
  redjet_hotlog("[SEASON] Transitioned to SPRING: Full Scan & Reconnaissance active, TailTraces profiling.");
}

void RedjetSeasonalCoordinator::transition_to_summer() {
  if (is_transition_dampened(RedjetSeason::SUMMER_STEADY_STATE)) {
    return;
  }

  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t tsc = sacculus_rdtsc();

  _current_season.store(RedjetSeason::SUMMER_STEADY_STATE, std::memory_order_release);
  _current_inspection_mode.store(RedjetInspectionMode::MARK_AND_EXECUTION_CLASP, std::memory_order_release);
  _season_entry_epoch.store(epoch, std::memory_order_release);
  _season_entry_tsc.store(tsc, std::memory_order_release);
  _last_transition_epoch.store(epoch, std::memory_order_release);
  _recent_transition_count.fetch_add(1, std::memory_order_relaxed);

  _metrics.summer_cycles.fetch_add(1, std::memory_order_relaxed);
  _metrics.tailtraces_clasp_invocations.fetch_add(1, std::memory_order_relaxed);
  _metrics.current_accordion_scan_depth.store(0.01, std::memory_order_release); // 1% micro-sentinel

  recalibrate_markov_matrix();
  redjet_hotlog("[SEASON] Transitioned to SUMMER: 99% TailTraces Clasping dominant, Scan collapsed to sentinel.");
}

void RedjetSeasonalCoordinator::transition_to_autumn(const char* reason) {
  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t tsc = sacculus_rdtsc();

  _current_season.store(RedjetSeason::AUTUMN_DIMENSION_SHIFT, std::memory_order_release);
  _current_inspection_mode.store(RedjetInspectionMode::HYBRID_COOPERATIVE, std::memory_order_release);
  _season_entry_epoch.store(epoch, std::memory_order_release);
  _season_entry_tsc.store(tsc, std::memory_order_release);
  _last_transition_epoch.store(epoch, std::memory_order_release);
  _recent_transition_count.fetch_add(1, std::memory_order_relaxed);

  _metrics.autumn_cycles.fetch_add(1, std::memory_order_relaxed);
  _metrics.autumn_resurgence_events.fetch_add(1, std::memory_order_relaxed);
  _metrics.hybrid_cooperative_invocations.fetch_add(1, std::memory_order_relaxed);
  _metrics.scan_recon_invocations.fetch_add(1, std::memory_order_relaxed);
  _metrics.current_accordion_scan_depth.store(1.0, std::memory_order_release); // 100% full scan resurgence

  recalibrate_markov_matrix();
  redjet_hotlog("[SEASON] Transitioned to AUTUMN (Resurgence): %s. Full Scan sweeping heap topology.", reason != nullptr ? reason : "surge");
}

void RedjetSeasonalCoordinator::transition_to_winter() {
  if (is_transition_dampened(RedjetSeason::WINTER_IDLE_REBATE)) {
    return;
  }

  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t tsc = sacculus_rdtsc();

  _current_season.store(RedjetSeason::WINTER_IDLE_REBATE, std::memory_order_release);
  _current_inspection_mode.store(RedjetInspectionMode::SCAN_AND_RECONNAISSANCE, std::memory_order_release);
  _season_entry_epoch.store(epoch, std::memory_order_release);
  _season_entry_tsc.store(tsc, std::memory_order_release);
  _last_transition_epoch.store(epoch, std::memory_order_release);
  _recent_transition_count.fetch_add(1, std::memory_order_relaxed);

  _metrics.winter_cycles.fetch_add(1, std::memory_order_relaxed);
  _metrics.winter_os_rebates_completed.fetch_add(1, std::memory_order_relaxed);
  _metrics.current_accordion_scan_depth.store(0.20, std::memory_order_release); // 20% scan for idle coalescing

  recalibrate_markov_matrix();
  redjet_hotlog("[SEASON] Transitioned to WINTER: Application idle, Snatch & Rebate GYC and RMRP PLUMP coalescing pages.");
}

bool RedjetSeasonalCoordinator::evaluate_spring_to_summer() {
  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t entry_epoch = _season_entry_epoch.load(std::memory_order_relaxed);
  uint64_t dwell = (epoch >= entry_epoch) ? (epoch - entry_epoch) : 0;
  return dwell >= _config.spring_warmup_epoch_threshold;
}

bool RedjetSeasonalCoordinator::evaluate_summer_to_autumn(size_t current_velocity_mbps) {
  size_t baseline_mbps = _baseline_allocation_velocity.load(std::memory_order_relaxed) / (1024 * 1024);
  if (baseline_mbps == 0) baseline_mbps = 50;

  double ratio = static_cast<double>(current_velocity_mbps) / static_cast<double>(baseline_mbps);
  return ratio >= _config.autumn_allocation_spike_multiplier;
}

bool RedjetSeasonalCoordinator::evaluate_to_winter(uint64_t idle_ms) {
  return idle_ms >= _config.winter_idle_duration_ms_threshold;
}

bool RedjetSeasonalCoordinator::evaluate_winter_to_spring() {
  size_t bytes = _recent_allocation_bytes.load(std::memory_order_relaxed);
  return bytes > (1024 * 1024); // More than 1 MB allocated wakes up the system
}

void RedjetSeasonalCoordinator::update_bayesian_belief(size_t current_velocity_mbps, double tailtraces_hit_ratio, uint64_t idle_ms) {
  // Likelihood calculations based on observation vector:
  // e = (velocity, tailtraces_hit_ratio, idle_ms)

  double l_spring = 0.1;
  double l_summer = 0.1;
  double l_autumn = 0.1;
  double l_winter = 0.1;

  // Spring likelihood: Moderate velocity, hit ratio warming up, idle is low
  if (tailtraces_hit_ratio < 0.90 && idle_ms < 1000) {
    l_spring = 0.85;
  } else {
    l_spring = 0.15;
  }

  // Summer likelihood: High TailTraces hit ratio (>= 95%), moderate to high steady velocity, zero idle
  if (tailtraces_hit_ratio >= _config.summer_tailtraces_hit_ratio_min && idle_ms < 500) {
    l_summer = 0.92;
  } else {
    l_summer = 0.20;
  }

  // Autumn likelihood: Extreme velocity surge (> 2.5x baseline) or burst events
  size_t baseline_mbps = _baseline_allocation_velocity.load(std::memory_order_relaxed) / (1024 * 1024);
  if (baseline_mbps == 0) baseline_mbps = 50;
  double v_ratio = static_cast<double>(current_velocity_mbps) / static_cast<double>(baseline_mbps);
  if (v_ratio >= _config.autumn_allocation_spike_multiplier) {
    l_autumn = 0.95;
  } else {
    l_autumn = 0.10;
  }

  // Winter likelihood: Inactivity duration >= threshold
  if (idle_ms >= _config.winter_idle_duration_ms_threshold) {
    l_winter = 0.98;
  } else {
    l_winter = 0.05;
  }

  // Bayesian update: P(S_i | e) = P(e | S_i) * P(S_i) / P(e)
  double alpha = _config.bayesian_learning_rate;
  _belief.belief[0] = (1.0 - alpha) * _belief.belief[0] + alpha * l_spring;
  _belief.belief[1] = (1.0 - alpha) * _belief.belief[1] + alpha * l_summer;
  _belief.belief[2] = (1.0 - alpha) * _belief.belief[2] + alpha * l_autumn;
  _belief.belief[3] = (1.0 - alpha) * _belief.belief[3] + alpha * l_winter;
  _belief.normalize();
}

void RedjetSeasonalCoordinator::recalibrate_markov_matrix() {
  _metrics.markov_matrix_recalibrations.fetch_add(1, std::memory_order_relaxed);

  RedjetSeason s = _current_season.load(std::memory_order_relaxed);
  size_t cur_row = static_cast<size_t>(s);

  // Smoothly blend belief vector into the active state's transition row
  double decay = _config.markov_decay_factor;
  for (size_t col = 0; col < 4; ++col) {
    _markov.p[cur_row][col] = decay * _markov.p[cur_row][col] + (1.0 - decay) * _belief.belief[col];
  }
  _markov.normalize_row(cur_row);
}

void RedjetSeasonalCoordinator::recalculate_accordion_depth() {
  RedjetSeason season = _current_season.load(std::memory_order_relaxed);
  double target_depth = 1.0;

  switch (season) {
    case RedjetSeason::SPRING_WARMUP:
      target_depth = 1.0; // 100% full scan
      break;
    case RedjetSeason::SUMMER_STEADY_STATE: {
      // Modulate scan between 0.01 (1%) and 0.05 (5%) based on BPU accuracy
      double bpu_acc = _metrics.current_bpu_accuracy.load(std::memory_order_relaxed);
      target_depth = 0.01 + (1.0 - bpu_acc) * 0.20;
      if (target_depth > 0.10) target_depth = 0.10;
      break;
    }
    case RedjetSeason::AUTUMN_DIMENSION_SHIFT:
      target_depth = 1.0; // 100% full scan resurgence
      break;
    case RedjetSeason::WINTER_IDLE_REBATE:
      target_depth = 0.20; // 20% scan for background coalescing
      break;
  }

  _metrics.current_accordion_scan_depth.store(target_depth, std::memory_order_release);
}

void RedjetSeasonalCoordinator::sweep_quarantine_window(uint64_t current_epoch) {
  size_t active = _quarantine_active_count.load(std::memory_order_relaxed);
  if (active == 0) return;

  uint32_t ttl = _config.quarantine_ttl_epochs;
  size_t evicted_count = 0;

  for (size_t i = 0; i < QUARANTINE_SLOTS; ++i) {
    if (_quarantine_table[i].active && !_quarantine_table[i].is_rehabilitated && !_quarantine_table[i].is_committed_evicted) {
      uint64_t cond_epoch = _quarantine_table[i].condemned_epoch;
      if (current_epoch >= cond_epoch + ttl) {
        // Quarantine epoch expired without access: Commit Eviction to Landfill trash pit
        _quarantine_table[i].is_committed_evicted = true;
        _quarantine_table[i].active = false;
        evicted_count++;
        _metrics.falline_commit_evictions.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  if (evicted_count > 0) {
    _quarantine_active_count.fetch_sub(evicted_count, std::memory_order_relaxed);
  }
}

void RedjetSeasonalCoordinator::register_quarantine_object(uintptr_t obj_addr, size_t size, uint32_t method_id) {
  size_t idx = _quarantine_head.fetch_add(1, std::memory_order_relaxed) % QUARANTINE_SLOTS;
  uint64_t epoch = _current_epoch.load(std::memory_order_relaxed);
  uint64_t tsc = sacculus_rdtsc();

  _quarantine_table[idx].obj_address = obj_addr;
  _quarantine_table[idx].size_bytes = size;
  _quarantine_table[idx].condemned_epoch = epoch;
  _quarantine_table[idx].condemned_tsc = tsc;
  _quarantine_table[idx].origin_method_id = method_id;
  _quarantine_table[idx].is_rehabilitated = false;
  _quarantine_table[idx].is_committed_evicted = false;
  _quarantine_table[idx].active = true;

  _quarantine_active_count.fetch_add(1, std::memory_order_relaxed);
  _metrics.falline_morgue_quarantines.fetch_add(1, std::memory_order_relaxed);
}

bool RedjetSeasonalCoordinator::notify_quarantine_access(uintptr_t obj_addr) {
  // Check if this address is currently quarantined in Falline morgue
  for (size_t i = 0; i < QUARANTINE_SLOTS; ++i) {
    if (_quarantine_table[i].active && _quarantine_table[i].obj_address == obj_addr) {
      if (!_quarantine_table[i].is_rehabilitated) {
        // Speculation misprediction caught! Instant 1-cycle rehabilitation
        _quarantine_table[i].is_rehabilitated = true;
        _quarantine_table[i].active = false;
        _quarantine_active_count.fetch_sub(1, std::memory_order_relaxed);

        _metrics.falline_rehabilitations.fetch_add(1, std::memory_order_relaxed);
        _metrics.bpu_misprediction_penalties.fetch_add(1, std::memory_order_relaxed);

        // Update BPU accuracy rolling metric: penalty lowers accuracy
        double cur_acc = _metrics.current_bpu_accuracy.load(std::memory_order_relaxed);
        double new_acc = cur_acc * 0.99; // 1% hit
        if (new_acc < 0.80) new_acc = 0.80;
        _metrics.current_bpu_accuracy.store(new_acc, std::memory_order_release);

        return true; // Successfully rehabilitated
      }
    }
  }
  return false;
}

void RedjetSeasonalCoordinator::tick(uint64_t current_tsc, size_t current_velocity_mbps, double tailtraces_hit_ratio) {
  if (!_initialized.load(std::memory_order_relaxed)) {
    return;
  }

  uint64_t epoch = _current_epoch.fetch_add(1, std::memory_order_acq_rel);
  uint64_t last_act = _last_activity_tsc.load(std::memory_order_relaxed);
  uint64_t idle_cycles = (current_tsc > last_act) ? (current_tsc - last_act) : 0;

  // Approximate frequency as 3.0 GHz for TSC-to-ms translation
  uint64_t idle_ms = idle_cycles / 3000000;

  // 1. Update Bayesian belief vector based on telemetry
  update_bayesian_belief(current_velocity_mbps, tailtraces_hit_ratio, idle_ms);

  // 2. Housekeep sliding Falline pretrashing quarantine window
  sweep_quarantine_window(epoch);

  // 3. Modulate Accordion scan depth
  recalculate_accordion_depth();

  // 4. Update BPU accuracy if no recent mispredictions
  double cur_acc = _metrics.current_bpu_accuracy.load(std::memory_order_relaxed);
  if (cur_acc < 0.999) {
    _metrics.current_bpu_accuracy.store(cur_acc * 0.999 + 0.001 * 0.999, std::memory_order_release);
  }

  // 5. Evaluate seasonal state transitions
  RedjetSeason season = _current_season.load(std::memory_order_relaxed);

  switch (season) {
    case RedjetSeason::SPRING_WARMUP:
      if (evaluate_spring_to_summer() && tailtraces_hit_ratio >= _config.summer_tailtraces_hit_ratio_min) {
        transition_to_summer();
      }
      break;

    case RedjetSeason::SUMMER_STEADY_STATE:
      if (evaluate_to_winter(idle_ms)) {
        transition_to_winter();
      } else if (evaluate_summer_to_autumn(current_velocity_mbps)) {
        transition_to_autumn("Allocation velocity spike");
      }
      break;

    case RedjetSeason::AUTUMN_DIMENSION_SHIFT: {
      uint64_t entry_epoch = _season_entry_epoch.load(std::memory_order_relaxed);
      uint64_t dwell = (epoch >= entry_epoch) ? (epoch - entry_epoch) : 0;
      // After sweeping through the dimension shift anomaly (at least 10 epochs), transition back to Summer
      if (dwell >= 10 && tailtraces_hit_ratio >= _config.summer_tailtraces_hit_ratio_min) {
        transition_to_summer();
      }
      break;
    }

    case RedjetSeason::WINTER_IDLE_REBATE:
      if (evaluate_winter_to_spring()) {
        transition_to_spring();
      }
      break;
  }

  // Update baseline velocity via Exponential Moving Average (alpha = 0.05)
  size_t current_velocity_bytes = current_velocity_mbps * 1024 * 1024;
  size_t old_baseline = _baseline_allocation_velocity.load(std::memory_order_relaxed);
  size_t new_baseline = static_cast<size_t>(old_baseline * 0.95 + current_velocity_bytes * 0.05);
  _baseline_allocation_velocity.store(new_baseline, std::memory_order_relaxed);

  // Reset recent allocation accumulator for the next tick interval
  _recent_allocation_bytes.store(0, std::memory_order_relaxed);
}

void RedjetSeasonalCoordinator::notify_dimension_shift(const char* module_or_event_name) {
  _last_activity_tsc.store(sacculus_rdtsc(), std::memory_order_release);
  transition_to_autumn(module_or_event_name != nullptr ? module_or_event_name : "Dynamic Dimension Shift");
}

void RedjetSeasonalCoordinator::notify_mutator_activity(size_t allocated_bytes) {
  _last_activity_tsc.store(sacculus_rdtsc(), std::memory_order_release);
  _recent_allocation_bytes.fetch_add(allocated_bytes, std::memory_order_relaxed);

  RedjetSeason season = _current_season.load(std::memory_order_relaxed);
  if (season == RedjetSeason::WINTER_IDLE_REBATE) {
    transition_to_spring();
  }
}

void RedjetSeasonalCoordinator::record_falline_quarantine(size_t object_size) {
  _metrics.falline_morgue_quarantines.fetch_add(1, std::memory_order_relaxed);
}

void RedjetSeasonalCoordinator::record_falline_rehabilitation(size_t object_size) {
  _metrics.falline_rehabilitations.fetch_add(1, std::memory_order_relaxed);
  _metrics.bpu_misprediction_penalties.fetch_add(1, std::memory_order_relaxed);
}

void RedjetSeasonalCoordinator::record_falline_eviction(size_t object_size) {
  _metrics.falline_commit_evictions.fetch_add(1, std::memory_order_relaxed);
}

bool RedjetSeasonalCoordinator::is_escape_bound_clasping_active() noexcept {
  // Lock 1: Escape-Bound Clasping active in Summer and Autumn
  RedjetSeason season = _current_season.load(std::memory_order_relaxed);
  return (season == RedjetSeason::SUMMER_STEADY_STATE ||
          season == RedjetSeason::AUTUMN_DIMENSION_SHIFT);
}

bool RedjetSeasonalCoordinator::is_falline_morgue_active() noexcept {
  // Lock 2: Falline Morgue pretrashing quarantine is active in all seasons
  return true;
}

bool RedjetSeasonalCoordinator::is_out_of_order_barrier_required() noexcept {
  // Lock 3: Out-of-order instruction barrier required during clasp signals
  return true;
}

size_t RedjetSeasonalCoordinator::compute_seasonal_reclamation_target_bytes(size_t current_heap_used, size_t max_heap_capacity) {
  RedjetSeason season = _current_season.load(std::memory_order_relaxed);
  switch (season) {
    case RedjetSeason::WINTER_IDLE_REBATE:
      // Deep rebate: target reclaiming 75% of unreferenced memory to shrink RSS
      return static_cast<size_t>(current_heap_used * 0.75);
    case RedjetSeason::AUTUMN_DIMENSION_SHIFT:
      // Resurgence: target reclaiming 35% of fragmented and orphaned memory
      return static_cast<size_t>(current_heap_used * 0.35);
    case RedjetSeason::SPRING_WARMUP:
      // Warmup: light 15% sweep of temporary initialization structures
      return static_cast<size_t>(current_heap_used * 0.15);
    case RedjetSeason::SUMMER_STEADY_STATE:
    default:
      // Steady-state: minimal 5% background recycling, prioritize 0-pause mutator flow
      return static_cast<size_t>(current_heap_used * 0.05);
  }
}

void RedjetSeasonalCoordinator::print_on(outputStream* st) {
  if (st == nullptr) return;

  RedjetSeason season = _current_season.load(std::memory_order_relaxed);
  RedjetInspectionMode mode = _current_inspection_mode.load(std::memory_order_relaxed);

  st->print_cr("Redjet Seasonal Transitions Coordinator Status:");
  st->print_cr("  Current Season:              %s", season_name(season));
  st->print_cr("  Inspection Mode:             %s", inspection_mode_name(mode));
  st->print_cr("  Current Epoch:               %llu", (unsigned long long)_current_epoch.load(std::memory_order_relaxed));
  st->print_cr("  Accordion Scan Depth:        %.2f%%", _metrics.current_accordion_scan_depth.load(std::memory_order_relaxed) * 100.0);
  st->print_cr("  BPU Accuracy:                %.3f%%", _metrics.current_bpu_accuracy.load(std::memory_order_relaxed) * 100.0);
  st->print_cr("  Baseline Velocity (MB/s):    %zu", _baseline_allocation_velocity.load(std::memory_order_relaxed) / (1024 * 1024));
  st->print_cr("  Bayesian Belief [Sp/Su/Au/Wi]: [%.2f, %.2f, %.2f, %.2f]",
               _belief.belief[0], _belief.belief[1], _belief.belief[2], _belief.belief[3]);
  st->print_cr("  Spring Warmup Cycles:        %llu", (unsigned long long)_metrics.spring_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Summer Steady Cycles:        %llu", (unsigned long long)_metrics.summer_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Autumn Resurgence Events:    %llu", (unsigned long long)_metrics.autumn_resurgence_events.load(std::memory_order_relaxed));
  st->print_cr("  Winter OS Rebates:           %llu", (unsigned long long)_metrics.winter_os_rebates_completed.load(std::memory_order_relaxed));
  st->print_cr("  Hysteresis Suppressions:     %llu", (unsigned long long)_metrics.hysteresis_suppressed_flaps.load(std::memory_order_relaxed));
  st->print_cr("  Markov Recalibrations:       %llu", (unsigned long long)_metrics.markov_matrix_recalibrations.load(std::memory_order_relaxed));
  st->print_cr("  Falline Quarantines:         %llu", (unsigned long long)_metrics.falline_morgue_quarantines.load(std::memory_order_relaxed));
  st->print_cr("  Falline Rehabilitations:     %llu", (unsigned long long)_metrics.falline_rehabilitations.load(std::memory_order_relaxed));
  st->print_cr("  Falline Evictions:           %llu", (unsigned long long)_metrics.falline_commit_evictions.load(std::memory_order_relaxed));
  st->print_cr("  BPU Misprediction Penalties: %llu", (unsigned long long)_metrics.bpu_misprediction_penalties.load(std::memory_order_relaxed));
}
