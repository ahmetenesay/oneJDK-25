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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETSIMDTABLE_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETSIMDTABLE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include <cmath>
#include <vector>
#include "utilities/macros.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvancePrecision.hpp"

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
// 1. SIMD LADDER RUNG ENUMERATION (16 Primary + 1 Overwrite Wildcard)
// ============================================================================
enum class RedjetSIMDRung : uint8_t {
  RUNG_00_SCALAR          = 0,
  RUNG_01_SSE2            = 1,
  RUNG_02_SSE3            = 2,
  RUNG_03_SSSE3           = 3,
  RUNG_04_SSE4_1          = 4,
  RUNG_05_SSE4_2          = 5,
  RUNG_06_AVX             = 6,
  RUNG_07_AVX2            = 7,
  RUNG_08_AVX512F         = 8,
  RUNG_09_AVX512CD        = 9,
  RUNG_10_AVX512BW        = 10,
  RUNG_11_AVX512DQ        = 11,
  RUNG_12_AVX512VL        = 12,
  RUNG_13_ARM_NEON        = 13,
  RUNG_14_ARM_SVE         = 14,
  RUNG_15_RISCV_VECTOR    = 15,
  RUNG_16_PUZZLER_OVERWRITE = 16,
  RUNG_COUNT              = 17
};

// ============================================================================
// 2. DISPATCH FUNCTION POINTER & METRIC DESCRIPTOR
// ============================================================================
typedef void (*RedjetVectorKernelFn)(const void* src, void* dst, size_t count);

struct alignas(64) RedjetLadderRungDescriptor {
  char                 name[32]{ 0 };
  uint32_t             ladder_level{ 0 };
  uint8_t              is_available{ 0 };
  uint8_t              is_active{ 0 };
  uint8_t              precision_tier{ static_cast<uint8_t>(RedjetPrecisionTier::NUMERIC_IEEE_FP) };
  uint8_t              _pad0{ 0 };
  RedjetVectorKernelFn kernel_fn{ nullptr };
  uint64_t             benchmarked_cycles{ 0 };
  double               drift_max_relative_error{ 0.0 };
  uint64_t             total_dispatches{ 0 };
  uint64_t             drift_violations{ 0 };
};

// ============================================================================
// 3. ZERO-PAUSE SHADOW RCU DISPATCH HOLDER
// Provides double-buffered atomic pointer hot-swapping with quiescent-state tracking.
// ============================================================================
class alignas(64) RedjetShadowRCULadder {
public:
  static constexpr size_t MAX_RCU_THREADS = 256;

private:
  RedjetLadderRungDescriptor _table_a[static_cast<size_t>(RedjetSIMDRung::RUNG_COUNT)];
  RedjetLadderRungDescriptor _table_b[static_cast<size_t>(RedjetSIMDRung::RUNG_COUNT)];

  std::atomic<RedjetLadderRungDescriptor*> _active_table{ nullptr };
  std::atomic<uint64_t>                    _global_rcu_epoch{ 0 };
  alignas(64) std::atomic<uint64_t>        _thread_epochs[MAX_RCU_THREADS];

public:
  RedjetShadowRCULadder() noexcept;

  // Read-side lock-free entry & exit
  inline RedjetLadderRungDescriptor* read_lock(uint32_t thread_id) noexcept {
    uint64_t e = _global_rcu_epoch.load(std::memory_order_relaxed);
    if (thread_id < MAX_RCU_THREADS) {
      _thread_epochs[thread_id].store(e, std::memory_order_release);
    }
    return _active_table.load(std::memory_order_acquire);
  }

  inline void read_unlock(uint32_t thread_id) noexcept {
    if (thread_id < MAX_RCU_THREADS) {
      _thread_epochs[thread_id].store(0, std::memory_order_release);
    }
  }

  // Write-side hot-swap: update shadow, atomic pointer swap, synchronize grace period
  bool hot_swap_slot16(const char* name, RedjetVectorKernelFn fn, uint32_t level);

  // Synchronize RCU grace period across Domestic workers
  void synchronize_rcu() noexcept;

  RedjetLadderRungDescriptor* active_table() noexcept {
    return _active_table.load(std::memory_order_acquire);
  }
};

// ============================================================================
// 4. REDJET SIMD TABLE & PRECISION DRIFT ENFORCER (PLAN.md Section 8.8 & F)
// 17-Slot SIMD ladder table with IEEE fallback on drift and zero-pause Shadow RCU hot-swap.
// ============================================================================
class RedjetSIMDTable {
public:
  static constexpr size_t SLOT_COUNT = 17;

private:
  RedjetShadowRCULadder _rcu_ladder;
  uint16_t              _active_rung_index{ 0 };
  float                 _drift_tolerance_epsilon{ 1e-5f };

  // Telemetry & metrics
  std::atomic<uint64_t> _total_vector_executions{ 0 };
  std::atomic<uint64_t> _fallback_to_ieee_events{ 0 };
  std::atomic<uint64_t> _shadow_rcu_swaps{ 0 };
  std::atomic<uint64_t> _precision_verifications{ 0 };

  // Built-in reference baseline kernels
  static void kernel_scalar_baseline(const void* src, void* dst, size_t count);
  static void kernel_sse42_fallback(const void* src, void* dst, size_t count);
  static void kernel_avx2_fallback(const void* src, void* dst, size_t count);
  static void kernel_avx512_fallback(const void* src, void* dst, size_t count);

public:
  RedjetSIMDTable() noexcept;
  ~RedjetSIMDTable() = default;

  // Initialize ladder with hardware-detected rungs
  void initialize_hardware_rungs();

  // Populate object header simd_table[17] from master template
  void populate_live_header(HS_hotspot_live_header_t* header) const noexcept;

  // Execute kernel with precision verification
  void dispatch_vector(RedjetSIMDRung rung, const void* src, void* dst, size_t count, uint32_t thread_id = 0);

  // Precision drift enforcement: compares result buffer against exact IEEE baseline
  bool verify_and_enforce_precision(const float* candidate_output, const float* reference_output, size_t count);

  // Hot-swap slot 16 trampoline via Shadow RCU
  bool install_puzzler_specialization(const char* name, RedjetVectorKernelFn fn, uint32_t level);

  // Fallback to strict IEEE scalar rung upon drift violation
  void trigger_emergency_ieee_fallback();

  // Accessors
  uint16_t active_rung_index()         const noexcept { return _active_rung_index; }
  float    drift_tolerance_epsilon()   const noexcept { return _drift_tolerance_epsilon; }
  uint64_t total_vector_executions()   const noexcept { return _total_vector_executions.load(std::memory_order_relaxed); }
  uint64_t fallback_to_ieee_events()   const noexcept { return _fallback_to_ieee_events.load(std::memory_order_relaxed); }
  uint64_t shadow_rcu_swaps()          const noexcept { return _shadow_rcu_swaps.load(std::memory_order_relaxed); }
  uint64_t precision_verifications()   const noexcept { return _precision_verifications.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETSIMDTABLE_HPP
