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

#ifndef SHARE_GC_REDJET_SIMD_REDJETSIMDCONTENTIONGOVERNOR_HPP
#define SHARE_GC_REDJET_SIMD_REDJETSIMDCONTENTIONGOVERNOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#else
class outputStream;
#endif

// ============================================================================
// RedjetSIMDContentionGovernor
//
// Dynamic SIMD-to-Scalar Contention Switch & Critical-Only SIMD Governor
// Controlled by:
//   -XX:+RedjetGCAutoScalarOnSIMDContention (alias: RedjetDynamicSIMDFallback)
//   -XX:+RedjetGCCriticalSIMDOnly           (alias: RedjetCriticalSIMDOnly)
//
// Monitors mutator vector ALU/FMA port pressure, AVX-512/AVX10 frequency
// license downclock ratio, L1D/L2 bandwidth saturation, and concurrent Java
// vector scopes. When mutator workloads saturate vector execution ports,
// RedjetGC seamlessly steps down GC sweeps/bitmap scans/zeroing from 512-bit
// SIMD -> 256-bit SIMD -> 128-bit Critical SIMD -> Pure 64-bit Scalar / APX
// 32-EGPR integer ports, preventing mutator IPC degradation.
// ============================================================================

enum class RedjetGCOpCriticality : uint8_t {
  BACKGROUND_SCAVENGE  = 0, // Background RMRP, ColdLog compression, idle defrag
  STANDARD_SWEEP       = 1, // Concurrent Motherline sweep, bitmap clear, slab zeroing
  LATENCY_CRITICAL     = 2, // TLAB refill zeroing, Parkpoint barrier scan, TailTraces flush
  HARD_REALTIME_URGENT = 3  // Falline revival, MLTC emergency remediation, SSIOLSC step
};

enum class RedjetGCVecTier : uint8_t {
  PURE_SCALAR_GPR      = 0, // 64-bit GPR / x86 APX 32-EGPR NDD (Zero FPU/SIMD port usage)
  NARROW_SIMD_128      = 1, // 128-bit SSE4.2 / NEON / RVV-128 (Zero license penalty)
  MEDIUM_SIMD_256      = 2, // 256-bit AVX10-256 / AVX2 / SVE2-256 / RVV-256
  WIDE_SIMD_512        = 3  // 512-bit AVX10.2-512 / AVX-512 / SVE2-512 / RVV-512
};

struct RedjetSIMDContentionSnapshot {
  double          ema_port_pressure;          // [0.0 .. 1.0] smoothed ALU/FMA vector port load
  double          downclock_penalty_ratio;    // [0.0 .. 1.0] frequency drop ratio under wide vectors
  uint32_t        active_mutator_simd_scopes; // Concurrent Java/MIRC/BREE vector loops in flight
  uint32_t        logical_core_count;
  RedjetGCVecTier current_global_tier;
  bool            auto_scalar_enabled;
  bool            critical_simd_only_enabled;
  bool            hardware_apx_available;
  uint64_t        total_scalar_fallbacks;
  uint64_t        total_tier_downgrades;
  uint64_t        total_tier_upgrades;
  uint64_t        total_critical_simd_grants;
};

class RedjetSIMDContentionGovernor {
public:
  static constexpr double PRESSURE_TIER_512_MAX    = 0.35;
  static constexpr double PRESSURE_TIER_256_MAX    = 0.70;
  static constexpr double PRESSURE_TIER_128_MAX    = 0.88;
  static constexpr double HYSTERESIS_BAND          = 0.06;
  static constexpr uint64_t MIN_SWITCH_INTERVAL_NS = 250000ULL; // 250 us anti-thrash window

private:
  alignas(64) std::atomic<uint32_t> _active_mutator_simd_threads{ 0 };
  alignas(64) std::atomic<uint64_t> _mutator_vector_ops_window{ 0 };
  alignas(64) std::atomic<uint32_t> _ema_pressure_permille{ 0 };      // 0..1000
  std::atomic<uint32_t>             _downclock_penalty_permille{ 0 }; // 0..1000
  std::atomic<uint8_t>              _current_tier{ static_cast<uint8_t>(RedjetGCVecTier::WIDE_SIMD_512) };
  std::atomic<uint8_t>              _max_hardware_tier{ static_cast<uint8_t>(RedjetGCVecTier::WIDE_SIMD_512) };
  std::atomic<bool>                 _apx_egpr_available{ false };
  std::atomic<bool>                 _initialized{ false };
  std::atomic<uint64_t>             _last_switch_ns{ 0 };
  uint32_t                          _logical_cores{ 1 };

  // Telemetry counters
  std::atomic<uint64_t>             _total_samples{ 0 };
  std::atomic<uint64_t>             _total_scalar_fallbacks{ 0 };
  std::atomic<uint64_t>             _total_tier_downgrades{ 0 };
  std::atomic<uint64_t>             _total_tier_upgrades{ 0 };
  std::atomic<uint64_t>             _total_critical_simd_grants{ 0 };

  RedjetGCVecTier detect_hardware_ceiling_tier() noexcept;

public:
  RedjetSIMDContentionGovernor() = default;
  ~RedjetSIMDContentionGovernor() = default;

  static RedjetSIMDContentionGovernor& instance() noexcept;

  RJ_API void initialize();
  RJ_API void reset() noexcept;

  // Mutator / BREE / MIRC scope registration hooks (lock-free L1-resident atomics)
  RJ_API void notify_mutator_simd_enter(uint32_t vector_width_bits = 256) noexcept;
  RJ_API void notify_mutator_simd_exit() noexcept;
  RJ_API void record_mutator_vector_burst(uint64_t vector_uops) noexcept;

  // Periodic PMU / Coach telemetry update
  RJ_API RedjetGCVecTier sample_and_govern(double observed_ipc,
                                           double l1d_miss_rate,
                                           double observed_freq_ratio = 1.0) noexcept;

  // Fast-path O(1) query invoked by RedjetGC sweep / zeroing / relocation kernels
  RJ_API RedjetGCVecTier select_tier_for_gc_op(RedjetGCOpCriticality criticality) noexcept;

  RJ_API bool should_use_scalar_for_gc_op(RedjetGCOpCriticality criticality) noexcept {
    return select_tier_for_gc_op(criticality) == RedjetGCVecTier::PURE_SCALAR_GPR;
  }

  RJ_API uint32_t recommended_vector_width_bytes(RedjetGCOpCriticality criticality) noexcept;

  RJ_API RedjetSIMDContentionSnapshot snapshot() const noexcept;
  RJ_API static const char* tier_name(RedjetGCVecTier tier) noexcept;

#ifndef REDJET_STANDALONE_BUILD
  void print_on(outputStream* st) const;
#endif
};

// RAII Guard for MIRC / BREE / RJ Math mutator vector execution scopes
class RedjetMutatorSIMDScopeGuard {
private:
  bool _active;
public:
  explicit RedjetMutatorSIMDScopeGuard(uint32_t width_bits = 256, bool enable = true) noexcept
    : _active(enable) {
    if (_active) {
      RedjetSIMDContentionGovernor::instance().notify_mutator_simd_enter(width_bits);
    }
  }
  ~RedjetMutatorSIMDScopeGuard() noexcept {
    if (_active) {
      RedjetSIMDContentionGovernor::instance().notify_mutator_simd_exit();
    }
  }
  RedjetMutatorSIMDScopeGuard(const RedjetMutatorSIMDScopeGuard&) = delete;
  RedjetMutatorSIMDScopeGuard& operator=(const RedjetMutatorSIMDScopeGuard&) = delete;
};

extern RedjetSIMDContentionGovernor g_redjet_simd_governor;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void     HS_hotspot_simd_governor_enter_mutator(uint32_t vector_width_bits);
RJ_API void     HS_hotspot_simd_governor_exit_mutator(void);
RJ_API uint32_t HS_hotspot_simd_governor_recommended_bytes(uint8_t criticality);
RJ_API int      HS_hotspot_simd_governor_is_scalar_fallback(uint8_t criticality);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_SIMD_REDJETSIMDCONTENTIONGOVERNOR_HPP
