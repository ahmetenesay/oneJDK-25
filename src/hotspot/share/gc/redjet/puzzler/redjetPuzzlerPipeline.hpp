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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETPUZZLERPIPELINE_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETPUZZLERPIPELINE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include "utilities/macros.hpp"
#include "gc/redjet/puzzler/redjetSIMDTable.hpp"
#include "gc/redjet/puzzler/redjetAosSoaMorph.hpp"
#include "gc/redjet/puzzler/redjetLoopUnroller.hpp"
#include "gc/redjet/puzzler/redjetBytecodeParser.hpp"
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
// 1. PIPELINE STAGE ENUMERATION
// ============================================================================
enum class RedjetPipelineStage : uint8_t {
  FRONTEND_PARSE      = 0,
  LOOP_UNROLL_OPT     = 1,
  AOS_TO_SOA_MORPH    = 2,
  SIMD_RUNG_SELECT    = 3,
  PRECISION_GUARD     = 4,
  DUEL_BENCHMARK      = 5,
  SHADOW_RCU_DEPLOY   = 6,
  STAGE_COUNT         = 7
};

// ============================================================================
// 2. CANDIDATE WORKLOAD DESCRIPTOR
// ============================================================================
struct RedjetWorkloadCandidate {
  uint32_t            function_id{ 0 };
  uint64_t            signature_hash{ 0 };
  char                symbol_name[64]{ 0 };
  uint32_t            unroll_factor{ 1 };
  bool                soa_converted{ false };
  RedjetSIMDRung      selected_rung{ RedjetSIMDRung::RUNG_00_SCALAR };
  float               measured_drift{ 0.0f };
  uint64_t            baseline_cycles{ 0 };
  uint64_t            optimized_cycles{ 0 };
  double              speedup_ratio{ 1.0 };
  RedjetVectorKernelFn compiled_kernel{ nullptr };
};

// ============================================================================
// 3. REDJET PUZZLER PIPELINE (PLAN.md Section 8.8, 8.9 & Section F)
// Coordinates bytecode loop extraction, AoS->SoA transformation, unrolling,
// precision verification, duel benchmarking, and Shadow RCU zero-pause hot-swap.
// ============================================================================
class RedjetPuzzlerPipeline {
private:
  RedjetSIMDTable            _simd_table;
  RedjetPrecisionGuardEngine _precision_guard;
  std::atomic<bool>          _enabled{ true };

  // Telemetry & metrics
  std::atomic<uint64_t>      _pipeline_runs{ 0 };
  std::atomic<uint64_t>      _optimizations_successful{ 0 };
  std::atomic<uint64_t>      _optimizations_rejected_drift{ 0 };
  std::atomic<uint64_t>      _optimizations_rejected_speed{ 0 };
  std::atomic<uint64_t>      _cumulative_cycles_saved{ 0 };
  std::atomic<uint64_t>      _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::STAGE_COUNT)];

public:
  RedjetPuzzlerPipeline() noexcept;
  ~RedjetPuzzlerPipeline() = default;

  // Process a candidate loop through the full optimization pipeline
  bool process_candidate(RedjetWorkloadCandidate& candidate, const float* test_input, float* test_output, size_t count);

  // Access SIMD Table
  RedjetSIMDTable& simd_table() noexcept { return _simd_table; }
  const RedjetSIMDTable& simd_table() const noexcept { return _simd_table; }

  // Configuration
  void set_enabled(bool enabled) noexcept { _enabled.store(enabled, std::memory_order_release); }
  bool is_enabled() const noexcept { return _enabled.load(std::memory_order_relaxed); }

  // Telemetry accessors
  uint64_t pipeline_runs()                 const noexcept { return _pipeline_runs.load(std::memory_order_relaxed); }
  uint64_t optimizations_successful()      const noexcept { return _optimizations_successful.load(std::memory_order_relaxed); }
  uint64_t optimizations_rejected_drift()  const noexcept { return _optimizations_rejected_drift.load(std::memory_order_relaxed); }
  uint64_t optimizations_rejected_speed()  const noexcept { return _optimizations_rejected_speed.load(std::memory_order_relaxed); }
  uint64_t cumulative_cycles_saved()       const noexcept { return _cumulative_cycles_saved.load(std::memory_order_relaxed); }

  uint64_t stage_executions(RedjetPipelineStage stage) const noexcept {
    return _stage_execution_counts[static_cast<size_t>(stage)].load(std::memory_order_relaxed);
  }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETPUZZLERPIPELINE_HPP
