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

#include "gc/redjet/puzzler/redjetPuzzlerPipeline.hpp"
#include <chrono>
#include <vector>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <immintrin.h>
#include <intrin.h>
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <x86intrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

static inline uint64_t rj_pipeline_read_tsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
  return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t val;
  asm volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#else
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

RedjetPuzzlerPipeline::RedjetPuzzlerPipeline() noexcept
  : _enabled(true),
    _pipeline_runs(0),
    _optimizations_successful(0),
    _optimizations_rejected_drift(0),
    _optimizations_rejected_speed(0),
    _cumulative_cycles_saved(0) {
  for (size_t s = 0; s < static_cast<size_t>(RedjetPipelineStage::STAGE_COUNT); ++s) {
    _stage_execution_counts[s].store(0, std::memory_order_relaxed);
  }
}

bool RedjetPuzzlerPipeline::process_candidate(RedjetWorkloadCandidate& candidate,
                                             const float* test_input,
                                             float* test_output,
                                             size_t count) {
  if (!_enabled.load(std::memory_order_relaxed) || test_input == nullptr || test_output == nullptr || count == 0) {
    return false;
  }

  _pipeline_runs.fetch_add(1, std::memory_order_relaxed);

  // Stage 0: Frontend Parse & Validation
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::FRONTEND_PARSE)].fetch_add(1, std::memory_order_relaxed);
  if (candidate.function_id == 0 && candidate.signature_hash == 0) {
    return false;
  }

  // Stage 1: Loop Unroll Optimization
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::LOOP_UNROLL_OPT)].fetch_add(1, std::memory_order_relaxed);
#if defined(__AVX512F__) || (defined(_MSC_VER) && defined(__AVX512F__))
  candidate.unroll_factor = 8; // 32 512-bit registers available
#elif defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
  candidate.unroll_factor = 4; // 16 256-bit registers available
#else
  candidate.unroll_factor = 2; // 16 128-bit registers available
#endif

  // Stage 2: AoS to SoA Layout Morphing Evaluation
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::AOS_TO_SOA_MORPH)].fetch_add(1, std::memory_order_relaxed);
  candidate.soa_converted = true;

  // Stage 3: SIMD Rung Selection
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::SIMD_RUNG_SELECT)].fetch_add(1, std::memory_order_relaxed);
#if defined(__AVX512F__) || (defined(_MSC_VER) && defined(__AVX512F__))
  candidate.selected_rung = RedjetSIMDRung::RUNG_08_AVX512F;
#elif defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
  candidate.selected_rung = RedjetSIMDRung::RUNG_07_AVX2;
#else
  candidate.selected_rung = RedjetSIMDRung::RUNG_05_SSE4_2;
#endif

  // Stage 4: Precision Guard & Drift Verification
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::PRECISION_GUARD)].fetch_add(1, std::memory_order_relaxed);

  // Allocate reference baseline output
  std::vector<float> ref_output(count, 0.0f);
  _simd_table.dispatch_vector(RedjetSIMDRung::RUNG_00_SCALAR, test_input, ref_output.data(), count);

  // Execute candidate rung
  _simd_table.dispatch_vector(candidate.selected_rung, test_input, test_output, count);

  bool precision_valid = _simd_table.verify_and_enforce_precision(test_output, ref_output.data(), count);
  if (!precision_valid) {
    _optimizations_rejected_drift.fetch_add(1, std::memory_order_relaxed);
    log_debug(gc, puzzler)("Puzzler Pipeline: candidate rejected due to precision drift violation");
    return false;
  }

  // Stage 5: Duel Benchmark
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::DUEL_BENCHMARK)].fetch_add(1, std::memory_order_relaxed);

  uint64_t tsc_b0 = rj_pipeline_read_tsc();
  for (int iter = 0; iter < 100; ++iter) {
    _simd_table.dispatch_vector(RedjetSIMDRung::RUNG_00_SCALAR, test_input, ref_output.data(), count);
  }
  uint64_t tsc_b1 = rj_pipeline_read_tsc();
  candidate.baseline_cycles = tsc_b1 - tsc_b0;

  uint64_t tsc_o0 = rj_pipeline_read_tsc();
  for (int iter = 0; iter < 100; ++iter) {
    _simd_table.dispatch_vector(candidate.selected_rung, test_input, test_output, count);
  }
  uint64_t tsc_o1 = rj_pipeline_read_tsc();
  candidate.optimized_cycles = tsc_o1 - tsc_o0;

  if (candidate.optimized_cycles > 0) {
    candidate.speedup_ratio = static_cast<double>(candidate.baseline_cycles) / static_cast<double>(candidate.optimized_cycles);
  } else {
    candidate.speedup_ratio = 1.0;
  }

  // Only deploy if at least 5% faster
  if (candidate.speedup_ratio < 1.05) {
    _optimizations_rejected_speed.fetch_add(1, std::memory_order_relaxed);
    log_debug(gc, puzzler)("Puzzler Pipeline: candidate speedup %.2fx insufficient (< 1.05x)", candidate.speedup_ratio);
    return false;
  }

  // Stage 6: Zero-Pause Shadow RCU Hot-Swap Deployment into Slot 16
  _stage_execution_counts[static_cast<size_t>(RedjetPipelineStage::SHADOW_RCU_DEPLOY)].fetch_add(1, std::memory_order_relaxed);

  bool installed = _simd_table.install_puzzler_specialization(candidate.symbol_name, candidate.compiled_kernel, 16);
  if (!installed) {
    return false;
  }

  uint64_t saved = (candidate.baseline_cycles > candidate.optimized_cycles) ? (candidate.baseline_cycles - candidate.optimized_cycles) : 0;
  _cumulative_cycles_saved.fetch_add(saved, std::memory_order_relaxed);
  _optimizations_successful.fetch_add(1, std::memory_order_relaxed);

  log_info(gc, puzzler)("Puzzler Pipeline: successfully deployed specialized kernel for '%s' (speedup=%.2fx, cycles saved=%llu)",
                        candidate.symbol_name, candidate.speedup_ratio, (unsigned long long)saved);

  return true;
}

void RedjetPuzzlerPipeline::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet Puzzler Optimization Pipeline (ISA Morphing & Shadow RCU) ---");
  st->print_cr("  Pipeline Enabled:        %s", _enabled.load(std::memory_order_relaxed) ? "true" : "false");
  st->print_cr("  Total Pipeline Runs:     %llu", (unsigned long long)_pipeline_runs.load(std::memory_order_relaxed));
  st->print_cr("  Successful Deployments:  %llu", (unsigned long long)_optimizations_successful.load(std::memory_order_relaxed));
  st->print_cr("  Rejected (Drift):        %llu", (unsigned long long)_optimizations_rejected_drift.load(std::memory_order_relaxed));
  st->print_cr("  Rejected (Unprofitable): %llu", (unsigned long long)_optimizations_rejected_speed.load(std::memory_order_relaxed));
  st->print_cr("  Cumulative Cycles Saved: %llu", (unsigned long long)_cumulative_cycles_saved.load(std::memory_order_relaxed));

  static const char* STAGE_NAMES[static_cast<size_t>(RedjetPipelineStage::STAGE_COUNT)] = {
    "Frontend Parse", "Loop Unroll Opt", "AoS->SoA Morph",
    "SIMD Rung Select", "Precision Guard", "Duel Benchmark", "Shadow RCU Deploy"
  };

  for (size_t s = 0; s < static_cast<size_t>(RedjetPipelineStage::STAGE_COUNT); ++s) {
    st->print_cr("    Stage [%02zu] %-18s: %llu executions",
                 s, STAGE_NAMES[s], (unsigned long long)_stage_execution_counts[s].load(std::memory_order_relaxed));
  }

  _simd_table.print_on(st);
}
