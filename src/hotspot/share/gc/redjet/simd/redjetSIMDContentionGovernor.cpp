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

#include "gc/redjet/simd/redjetSIMDContentionGovernor.hpp"
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "is/isMacrowebHolder.hpp"
#include <algorithm>
#include <cstdio>

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#else
static bool RedjetGCAutoScalarOnSIMDContention = true;
static bool RedjetDynamicSIMDFallback = true;
static bool RedjetGCCriticalSIMDOnly = false;
static bool RedjetCriticalSIMDOnly = false;
#endif

RedjetSIMDContentionGovernor g_redjet_simd_governor;

RedjetSIMDContentionGovernor& RedjetSIMDContentionGovernor::instance() noexcept {
  return g_redjet_simd_governor;
}

static inline bool is_auto_scalar_contention_active() noexcept {
  return RedjetGCAutoScalarOnSIMDContention && RedjetDynamicSIMDFallback;
}

static inline bool is_critical_simd_only_active() noexcept {
  if (RedjetGCCriticalSIMDOnly || RedjetCriticalSIMDOnly) {
    return true;
  }
  const RedjetConfig& cfg = RedjetFlags::config();
  return cfg.strings.critical_simd_explicit &&
         cfg.strings.critical_simd_mode == RedjetCriticalSIMD::ALLOW &&
         cfg.strings.gc_operation_facility != RedjetGCOperationFacility::SIMD;
}

RedjetGCVecTier RedjetSIMDContentionGovernor::detect_hardware_ceiling_tier() noexcept {
  isMacrowebHolder::initialize();
  _apx_egpr_available.store(isMacrowebHolder::IS_APX(), std::memory_order_relaxed);

  if (isMacrowebHolder::IS_AVX10_2() || isMacrowebHolder::IS_AVX512()) {
    return RedjetGCVecTier::WIDE_SIMD_512;
  }
  if (isMacrowebHolder::IS_SVE2()) {
    uint32_t vl = isMacrowebHolder::max_vector_bytes();
    return (vl >= 64) ? RedjetGCVecTier::WIDE_SIMD_512
                      : (vl >= 32 ? RedjetGCVecTier::MEDIUM_SIMD_256 : RedjetGCVecTier::NARROW_SIMD_128);
  }
  if (isMacrowebHolder::IS_RVV_1_0()) {
    uint32_t vl = isMacrowebHolder::max_vector_bytes();
    return (vl >= 64) ? RedjetGCVecTier::WIDE_SIMD_512
                      : (vl >= 32 ? RedjetGCVecTier::MEDIUM_SIMD_256 : RedjetGCVecTier::NARROW_SIMD_128);
  }
  if (isMacrowebHolder::IS_AVX10_1() || isMacrowebHolder::IS_AVX2() || isMacrowebHolder::IS_SVE()) {
    return RedjetGCVecTier::MEDIUM_SIMD_256;
  }
  if (isMacrowebHolder::IS_SSE4_2() || isMacrowebHolder::IS_SSE2() || isMacrowebHolder::IS_NEON()) {
    return RedjetGCVecTier::NARROW_SIMD_128;
  }
  return RedjetGCVecTier::PURE_SCALAR_GPR;
}

void RedjetSIMDContentionGovernor::initialize() {
  if (_initialized.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
#ifndef REDJET_STANDALONE_BUILD
  int procs = os::processor_count();
  _logical_cores = (procs > 0) ? static_cast<uint32_t>(procs) : 1u;
#else
  _logical_cores = 8u;
#endif
  RedjetGCVecTier hw_max = detect_hardware_ceiling_tier();
  _max_hardware_tier.store(static_cast<uint8_t>(hw_max), std::memory_order_release);
  _current_tier.store(static_cast<uint8_t>(hw_max), std::memory_order_release);
  _last_switch_ns.store(sacculus_now_ns(), std::memory_order_relaxed);

#ifndef REDJET_STANDALONE_BUILD
  log_info(gc, init)("RedjetGC SIMD Contention Governor initialized: ceiling=%s, APX_EGPR=%s, AutoScalar=%s, CriticalOnly=%s",
                     tier_name(hw_max),
                     _apx_egpr_available.load(std::memory_order_relaxed) ? "enabled" : "absent",
                     is_auto_scalar_contention_active() ? "enabled" : "disabled",
                     is_critical_simd_only_active() ? "enabled" : "disabled");
#endif
}

void RedjetSIMDContentionGovernor::reset() noexcept {
  _active_mutator_simd_threads.store(0, std::memory_order_relaxed);
  _mutator_vector_ops_window.store(0, std::memory_order_relaxed);
  _ema_pressure_permille.store(0, std::memory_order_relaxed);
  _downclock_penalty_permille.store(0, std::memory_order_relaxed);
  uint8_t hw = _max_hardware_tier.load(std::memory_order_relaxed);
  _current_tier.store(hw, std::memory_order_release);
}

void RedjetSIMDContentionGovernor::notify_mutator_simd_enter(uint32_t vector_width_bits) noexcept {
  uint32_t weight = (vector_width_bits >= 512) ? 2u : 1u;
  _active_mutator_simd_threads.fetch_add(weight, std::memory_order_relaxed);
  _mutator_vector_ops_window.fetch_add(vector_width_bits, std::memory_order_relaxed);
}

void RedjetSIMDContentionGovernor::notify_mutator_simd_exit() noexcept {
  uint32_t cur = _active_mutator_simd_threads.load(std::memory_order_relaxed);
  while (cur > 0) {
    uint32_t next = (cur >= 1) ? (cur - 1) : 0;
    if (_active_mutator_simd_threads.compare_exchange_weak(cur, next, std::memory_order_relaxed)) {
      break;
    }
  }
}

void RedjetSIMDContentionGovernor::record_mutator_vector_burst(uint64_t vector_uops) noexcept {
  _mutator_vector_ops_window.fetch_add(vector_uops, std::memory_order_relaxed);
}

RedjetGCVecTier RedjetSIMDContentionGovernor::sample_and_govern(double observed_ipc,
                                                                double l1d_miss_rate,
                                                                double observed_freq_ratio) noexcept {
  if (!_initialized.load(std::memory_order_acquire)) {
    initialize();
  }
  _total_samples.fetch_add(1, std::memory_order_relaxed);

  const uint32_t cores = (_logical_cores > 0) ? _logical_cores : 1u;
  const uint32_t active_vec_threads = _active_mutator_simd_threads.load(std::memory_order_relaxed);
  const uint64_t burst_ops = _mutator_vector_ops_window.exchange(0, std::memory_order_relaxed);

  // 1. Thread saturation component: fraction of logical cores running active Java SIMD/BREE loops
  double thread_ratio = static_cast<double>(active_vec_threads) / static_cast<double>(cores);
  if (thread_ratio > 1.0) thread_ratio = 1.0;

  // 2. UOP burst density component normalized per core
  double burst_ratio = static_cast<double>(burst_ops) / static_cast<double>(cores * 65536ULL);
  if (burst_ratio > 1.0) burst_ratio = 1.0;

  // 3. IPC + L1D back-pressure component (high IPC with low L1D miss implies compute/ALU port bound!)
  double port_bound_signal = 0.0;
  if (observed_ipc > 2.4 && l1d_miss_rate < 0.04) {
    port_bound_signal = std::min(1.0, (observed_ipc - 2.4) / 2.2);
  }

  // 4. Wide-vector frequency license downclock component
  double downclock_penalty = 0.0;
  if (observed_freq_ratio > 0.1 && observed_freq_ratio < 0.96) {
    downclock_penalty = std::min(1.0, (0.96 - observed_freq_ratio) * 4.0);
  }
  _downclock_penalty_permille.store(static_cast<uint32_t>(downclock_penalty * 1000.0), std::memory_order_relaxed);

  // Composite instant contention pressure in [0.0 .. 1.0]
  double instant_pressure = (0.45 * thread_ratio) +
                            (0.25 * burst_ratio) +
                            (0.20 * port_bound_signal) +
                            (0.10 * downclock_penalty);
  if (instant_pressure < 0.0) instant_pressure = 0.0;
  if (instant_pressure > 1.0) instant_pressure = 1.0;

  // Exponential Moving Average (alpha = 0.35 for fast contention response, 0.65 history)
  uint32_t prev_permille = _ema_pressure_permille.load(std::memory_order_relaxed);
  double prev_ema = static_cast<double>(prev_permille) / 1000.0;
  double new_ema = (0.35 * instant_pressure) + (0.65 * prev_ema);
  uint32_t new_permille = static_cast<uint32_t>(new_ema * 1000.0 + 0.5);
  if (new_permille > 1000u) new_permille = 1000u;
  _ema_pressure_permille.store(new_permille, std::memory_order_relaxed);

  RedjetGCVecTier hw_ceiling = static_cast<RedjetGCVecTier>(_max_hardware_tier.load(std::memory_order_relaxed));
  if (!is_auto_scalar_contention_active()) {
    _current_tier.store(static_cast<uint8_t>(hw_ceiling), std::memory_order_release);
    return hw_ceiling;
  }

  // Anti-thrash minimum interval check
  uint64_t now_ns = sacculus_now_ns();
  uint64_t last_ns = _last_switch_ns.load(std::memory_order_relaxed);
  RedjetGCVecTier cur_tier = static_cast<RedjetGCVecTier>(_current_tier.load(std::memory_order_acquire));

  RedjetGCVecTier target_tier = hw_ceiling;
  if (new_ema >= PRESSURE_TIER_128_MAX) {
    target_tier = RedjetGCVecTier::PURE_SCALAR_GPR;
  } else if (new_ema >= PRESSURE_TIER_256_MAX) {
    target_tier = std::min(hw_ceiling, RedjetGCVecTier::NARROW_SIMD_128);
  } else if (new_ema >= PRESSURE_TIER_512_MAX || downclock_penalty >= 0.25) {
    target_tier = std::min(hw_ceiling, RedjetGCVecTier::MEDIUM_SIMD_256);
  } else {
    target_tier = hw_ceiling;
  }

  // Apply hysteresis band when upgrading back to wider vectors to prevent oscillation
  if (static_cast<uint8_t>(target_tier) > static_cast<uint8_t>(cur_tier)) {
    if ((now_ns - last_ns) < MIN_SWITCH_INTERVAL_NS) {
      return cur_tier;
    }
    double upgrade_gate = 0.0;
    if (cur_tier == RedjetGCVecTier::PURE_SCALAR_GPR) {
      upgrade_gate = PRESSURE_TIER_128_MAX - HYSTERESIS_BAND;
    } else if (cur_tier == RedjetGCVecTier::NARROW_SIMD_128) {
      upgrade_gate = PRESSURE_TIER_256_MAX - HYSTERESIS_BAND;
    } else {
      upgrade_gate = PRESSURE_TIER_512_MAX - HYSTERESIS_BAND;
    }
    if (new_ema > upgrade_gate) {
      return cur_tier;
    }
  }

  if (target_tier != cur_tier) {
    _current_tier.store(static_cast<uint8_t>(target_tier), std::memory_order_release);
    _last_switch_ns.store(now_ns, std::memory_order_relaxed);
    if (static_cast<uint8_t>(target_tier) < static_cast<uint8_t>(cur_tier)) {
      _total_tier_downgrades.fetch_add(1, std::memory_order_relaxed);
      if (target_tier == RedjetGCVecTier::PURE_SCALAR_GPR) {
        _total_scalar_fallbacks.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      _total_tier_upgrades.fetch_add(1, std::memory_order_relaxed);
    }

    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "[SIMDContentionGovernor] Switched GC vector tier %s -> %s (EMA=%.1f%%, active_scopes=%u)",
                  tier_name(cur_tier), tier_name(target_tier), new_ema * 100.0, active_vec_threads);
    hs_unified_log_emit(HS_Ebin0_ALU_DISPATCH_DIRECT, HS_Ebin1_INFO, 0, "simd_contention_switch", msg);
  }

  return target_tier;
}

RedjetGCVecTier RedjetSIMDContentionGovernor::select_tier_for_gc_op(RedjetGCOpCriticality criticality) noexcept {
  if (!_initialized.load(std::memory_order_acquire)) {
    initialize();
  }

  // Respect explicit scalar GC_Operation_Facility override if set
  const RedjetConfig& cfg = RedjetFlags::config();
  if (cfg.strings.gc_operation_facility == RedjetGCOperationFacility::CYCLES &&
      cfg.strings.critical_simd_mode == RedjetCriticalSIMD::DENY) {
    return RedjetGCVecTier::PURE_SCALAR_GPR;
  }

  const RedjetGCVecTier hw_ceiling = static_cast<RedjetGCVecTier>(_max_hardware_tier.load(std::memory_order_relaxed));

  // Hard real-time urgent paths (Falline trap revival / SSIOLSC) always get lock-free medium/narrow SIMD without downclock
  if (criticality == RedjetGCOpCriticality::HARD_REALTIME_URGENT) {
    _total_critical_simd_grants.fetch_add(1, std::memory_order_relaxed);
    return std::min(hw_ceiling, RedjetGCVecTier::MEDIUM_SIMD_256);
  }

  // Check Critical-SIMD-Only policy (-XX:+RedjetGCCriticalSIMDOnly)
  if (is_critical_simd_only_active()) {
    if (criticality < RedjetGCOpCriticality::LATENCY_CRITICAL) {
      return RedjetGCVecTier::PURE_SCALAR_GPR;
    }
    _total_critical_simd_grants.fetch_add(1, std::memory_order_relaxed);
  }

  if (!is_auto_scalar_contention_active()) {
    return hw_ceiling;
  }

  // Fast instant check: if mutator SIMD scopes saturate >= 85% of cores right now, immediately yield vector ports!
  uint32_t active_scopes = _active_mutator_simd_threads.load(std::memory_order_relaxed);
  if (_logical_cores > 0 && active_scopes * 100u >= _logical_cores * 85u) {
    if (criticality < RedjetGCOpCriticality::LATENCY_CRITICAL) {
      _total_scalar_fallbacks.fetch_add(1, std::memory_order_relaxed);
      return RedjetGCVecTier::PURE_SCALAR_GPR;
    }
    return std::min(hw_ceiling, RedjetGCVecTier::NARROW_SIMD_128);
  }

  RedjetGCVecTier governed = static_cast<RedjetGCVecTier>(_current_tier.load(std::memory_order_acquire));

  // Background scavenge drops one extra notch under moderate pressure to keep LLC/ALU completely clear
  if (criticality == RedjetGCOpCriticality::BACKGROUND_SCAVENGE &&
      governed == RedjetGCVecTier::WIDE_SIMD_512 &&
      _ema_pressure_permille.load(std::memory_order_relaxed) >= 200u) {
    return std::min(hw_ceiling, RedjetGCVecTier::MEDIUM_SIMD_256);
  }

  return governed;
}

uint32_t RedjetSIMDContentionGovernor::recommended_vector_width_bytes(RedjetGCOpCriticality criticality) noexcept {
  switch (select_tier_for_gc_op(criticality)) {
    case RedjetGCVecTier::WIDE_SIMD_512:   return 64u;
    case RedjetGCVecTier::MEDIUM_SIMD_256: return 32u;
    case RedjetGCVecTier::NARROW_SIMD_128: return 16u;
    case RedjetGCVecTier::PURE_SCALAR_GPR:
    default:                               return 8u;
  }
}

RedjetSIMDContentionSnapshot RedjetSIMDContentionGovernor::snapshot() const noexcept {
  RedjetSIMDContentionSnapshot snap{};
  snap.ema_port_pressure          = static_cast<double>(_ema_pressure_permille.load(std::memory_order_relaxed)) / 1000.0;
  snap.downclock_penalty_ratio    = static_cast<double>(_downclock_penalty_permille.load(std::memory_order_relaxed)) / 1000.0;
  snap.active_mutator_simd_scopes = _active_mutator_simd_threads.load(std::memory_order_relaxed);
  snap.logical_core_count         = _logical_cores;
  snap.current_global_tier        = static_cast<RedjetGCVecTier>(_current_tier.load(std::memory_order_relaxed));
  snap.auto_scalar_enabled        = is_auto_scalar_contention_active();
  snap.critical_simd_only_enabled = is_critical_simd_only_active();
  snap.hardware_apx_available     = _apx_egpr_available.load(std::memory_order_relaxed);
  snap.total_scalar_fallbacks     = _total_scalar_fallbacks.load(std::memory_order_relaxed);
  snap.total_tier_downgrades      = _total_tier_downgrades.load(std::memory_order_relaxed);
  snap.total_tier_upgrades        = _total_tier_upgrades.load(std::memory_order_relaxed);
  snap.total_critical_simd_grants = _total_critical_simd_grants.load(std::memory_order_relaxed);
  return snap;
}

const char* RedjetSIMDContentionGovernor::tier_name(RedjetGCVecTier tier) noexcept {
  switch (tier) {
    case RedjetGCVecTier::WIDE_SIMD_512:   return "WIDE_SIMD_512";
    case RedjetGCVecTier::MEDIUM_SIMD_256: return "MEDIUM_SIMD_256";
    case RedjetGCVecTier::NARROW_SIMD_128: return "NARROW_SIMD_128";
    case RedjetGCVecTier::PURE_SCALAR_GPR: return "PURE_SCALAR_GPR";
    default:                               return "UNKNOWN_TIER";
  }
}

#ifndef REDJET_STANDALONE_BUILD
void RedjetSIMDContentionGovernor::print_on(outputStream* st) const {
  if (st == nullptr) return;
  RedjetSIMDContentionSnapshot s = snapshot();
  st->print_cr("Redjet SIMD Contention Governor:");
  st->print_cr("  Active Global Tier:      %s (APX 32-EGPR Scalar Boost: %s)",
               tier_name(s.current_global_tier), s.hardware_apx_available ? "Yes" : "No");
  st->print_cr("  Auto-Scalar Contention:  %s | Critical-SIMD-Only: %s",
               s.auto_scalar_enabled ? "Enabled" : "Disabled",
               s.critical_simd_only_enabled ? "Enabled" : "Disabled");
  st->print_cr("  EMA Port Pressure:       %.2f%% | Downclock Penalty: %.2f%% | Active Scopes: %u/%u",
               s.ema_port_pressure * 100.0, s.downclock_penalty_ratio * 100.0,
               s.active_mutator_simd_scopes, s.logical_core_count);
  st->print_cr("  Scalar Fallbacks:        %llu | Downgrades: %llu | Upgrades: %llu | Critical Grants: %llu",
               static_cast<unsigned long long>(s.total_scalar_fallbacks),
               static_cast<unsigned long long>(s.total_tier_downgrades),
               static_cast<unsigned long long>(s.total_tier_upgrades),
               static_cast<unsigned long long>(s.total_critical_simd_grants));
}
#endif

extern "C" {

RJ_API void HS_hotspot_simd_governor_enter_mutator(uint32_t vector_width_bits) {
  RedjetSIMDContentionGovernor::instance().notify_mutator_simd_enter(vector_width_bits);
}

RJ_API void HS_hotspot_simd_governor_exit_mutator(void) {
  RedjetSIMDContentionGovernor::instance().notify_mutator_simd_exit();
}

RJ_API uint32_t HS_hotspot_simd_governor_recommended_bytes(uint8_t criticality) {
  RedjetGCOpCriticality crit = (criticality <= 3)
      ? static_cast<RedjetGCOpCriticality>(criticality)
      : RedjetGCOpCriticality::STANDARD_SWEEP;
  return RedjetSIMDContentionGovernor::instance().recommended_vector_width_bytes(crit);
}

RJ_API int HS_hotspot_simd_governor_is_scalar_fallback(uint8_t criticality) {
  RedjetGCOpCriticality crit = (criticality <= 3)
      ? static_cast<RedjetGCOpCriticality>(criticality)
      : RedjetGCOpCriticality::STANDARD_SWEEP;
  return RedjetSIMDContentionGovernor::instance().should_use_scalar_for_gc_op(crit) ? 1 : 0;
}

} // extern "C"
