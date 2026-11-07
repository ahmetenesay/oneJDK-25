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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETBPUDIRECTOR_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETBPUDIRECTOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "utilities/ostream.hpp"
#else
class outputStream;
#endif

// ============================================================================
// RedjetBPUDirector (BranchPredictingDirector + TailTraces BPU Co-Design)
//
// High-Accuracy Hybrid Branch & Object Liveness Predictor for RedjetGC:
//  1. 4096-entry Bimodal Base Predictor (2-bit saturating counters)
//  2. 4-Bank TAGE (TAgged GEometric History Length: L = {4, 12, 32, 88})
//     with folded CSR history, 11-bit tags, 3-bit signed prediction counters,
//     and 2-bit usefulness counters.
//  3. 256-entry Hashed Perceptron Neural Liveness Unit (16 history weights)
//     for loop-carried TailTraces escape/death correlation.
//  4. Hardware CPU BTB/BPU Symbiosis: computes static branch polarity hints
//     so VAC/MIRC place common non-escape TailTraces clasp paths on straight
//     fall-through (zero BTB slot consumption on cold escape paths).
//
// Ports & supersedes SacFor_VM `BranchPredictingDirector` (`HS_hotspot_branch_predict`).
// ============================================================================

struct RedjetTAGEEntry {
  int8_t   pred; // 3-bit signed counter [-4 .. +3], >= 0 means taken/dead-at-clasp
  uint8_t  u;    // 2-bit usefulness [0 .. 3]
  uint16_t tag;  // 11-bit folded PC ^ GHR tag
};

struct RedjetBPULivenessDecision {
  bool    predicted_truly_dead;      // True if object will NOT escape or revive
  uint8_t confidence_percent;        // [0 .. 100] fused TAGE + Perceptron confidence
  bool    bypass_long_probation;     // True when confidence >= 94% (1-epoch fast reclaim)
  bool    emit_likely_fallthrough;   // Hardware BTB static layout hint for MIRC
};

class RedjetBPUDirector {
public:
  static constexpr size_t BIMODAL_SIZE      = 4096;
  static constexpr size_t NUM_TAGE_BANKS    = 4;
  static constexpr size_t TAGE_BANK_ENTRIES = 1024;
  static constexpr size_t PERCEPTRON_COUNT  = 256;
  static constexpr size_t PERCEPTRON_HIST   = 16;

  static constexpr int HIST_LENGTHS[NUM_TAGE_BANKS] = { 4, 12, 32, 88 };

private:
  // Predictor tables
  uint8_t         _bimodal[BIMODAL_SIZE];
  RedjetTAGEEntry _tage[NUM_TAGE_BANKS][TAGE_BANK_ENTRIES];
  int8_t          _perceptrons[PERCEPTRON_COUNT][PERCEPTRON_HIST + 1]; // [0] = bias

  // 128-bit rolling Global History Register (low 64 + high 64)
  std::atomic<uint64_t> _ghr_lo{ 0 };
  std::atomic<uint64_t> _ghr_hi{ 0 };
  hs_ticket_lock_t      _update_lock;

  // Telemetry counters
  std::atomic<uint64_t> _total_predictions{ 0 };
  std::atomic<uint64_t> _correct_predictions{ 0 };
  std::atomic<uint64_t> _tage_hits{ 0 };
  std::atomic<uint64_t> _tailtrace_fast_grants{ 0 };
  std::atomic<uint64_t> _tailtrace_escape_vetoes{ 0 };

  uint32_t compute_tage_index(uint64_t pc, int bank, uint64_t ghr_lo, uint64_t ghr_hi) const noexcept;
  uint16_t compute_tage_tag(uint64_t pc, int bank, uint64_t ghr_lo, uint64_t ghr_hi) const noexcept;
  int32_t  compute_perceptron_sum(uint64_t pc, uint64_t ghr_lo) const noexcept;
  void     push_history_bit(bool taken) noexcept;

public:
  RedjetBPUDirector() noexcept;
  ~RedjetBPUDirector() = default;

  static RedjetBPUDirector& instance() noexcept;

  RJ_API void reset() noexcept;

  // General branch prediction & update (1 = predicted taken, 0 = not taken)
  RJ_API int predict_and_update(uint64_t branch_pc, int actual_taken) noexcept;
  RJ_API int predict_only(uint64_t branch_pc, uint8_t* out_confidence_pct = nullptr) const noexcept;

  // TailTraces-specific high-accuracy liveness / clasp predictor
  RJ_API RedjetBPULivenessDecision evaluate_tailtrace_clasp(uint64_t callsite_or_region_hash,
                                                            uint32_t thread_id,
                                                            size_t object_bytes) noexcept;

  RJ_API void record_tailtrace_outcome(uint64_t callsite_or_region_hash,
                                       uint32_t thread_id,
                                       bool was_truly_dead) noexcept;

  // Accuracy & telemetry accessors
  uint64_t total_predictions() const noexcept       { return _total_predictions.load(std::memory_order_relaxed); }
  uint64_t correct_predictions() const noexcept     { return _correct_predictions.load(std::memory_order_relaxed); }
  uint64_t tage_hits() const noexcept               { return _tage_hits.load(std::memory_order_relaxed); }
  uint64_t tailtrace_fast_grants() const noexcept   { return _tailtrace_fast_grants.load(std::memory_order_relaxed); }
  uint64_t tailtrace_escape_vetoes() const noexcept { return _tailtrace_escape_vetoes.load(std::memory_order_relaxed); }

  double accuracy_percent() const noexcept {
    uint64_t tot = total_predictions();
    if (tot == 0) return 100.0;
    return (static_cast<double>(correct_predictions()) * 100.0) / static_cast<double>(tot);
  }

#ifndef REDJET_STANDALONE_BUILD
  void print_on(outputStream* st) const;
#endif
};

extern RedjetBPUDirector g_redjet_bpu_director;

#ifdef __cplusplus
extern "C" {
#endif

// Canonical SacFor_VM heritage export (`HS_hotspot_branch_predict`)
RJ_API int    HS_hotspot_branch_predict(uint64_t branch_pc, int actual_taken);
RJ_API double HS_hotspot_bpu_accuracy_percent(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_RUNTIME_REDJETBPUDIRECTOR_HPP
