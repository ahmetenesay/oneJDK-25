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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETSPECULATIVEPIPELINE_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETSPECULATIVEPIPELINE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// Redjet Speculative Pipeline & STLF Hazard Analyzer (PLAN.md Section 5.3, 7.2)
// - Branch Target Buffer (BTB) warmers and branch bias predictor
// - Speculative Store-to-Load Forwarding (STLF) stall hazard analyzer
// - Detects partial store overlap and unaligned load penalties
// - Automatic NOP padding and instruction scheduling to guarantee 0-cycle STLF
// ============================================================================

enum class RedjetBranchBias : uint8_t {
  STRONGLY_NOT_TAKEN = 0,
  WEAKLY_NOT_TAKEN   = 1,
  WEAKLY_TAKEN       = 2,
  STRONGLY_TAKEN     = 3
};

struct RedjetBranchProfile {
  uint64_t         branch_pc{ 0 };
  uint64_t         target_pc{ 0 };
  uint64_t         taken_count{ 0 };
  uint64_t         not_taken_count{ 0 };
  RedjetBranchBias bias{ RedjetBranchBias::STRONGLY_TAKEN };
};

struct RedjetStlfHazard {
  size_t  store_offset{ 0 };
  size_t  store_bytes{ 0 };
  size_t  load_offset{ 0 };
  size_t  load_bytes{ 0 };
  bool    is_exact_match{ false };
  bool    is_partial_overlap{ false }; // Triggers severe 10-15 cycle STLF stall!
  uint8_t recommended_nop_padding{ 0 };
};

class RedjetSpeculativePipeline {
public:
  static constexpr size_t MAX_TRACKED_BRANCHES = 4096;

private:
  RedjetBranchProfile   _branches[MAX_TRACKED_BRANCHES];
  size_t                _branch_count{ 0 };
  hs_ticket_lock_t      _pipeline_lock;

  // Telemetry
  std::atomic<uint64_t> _total_branches_profiled{ 0 };
  std::atomic<uint64_t> _total_btb_entries_warmed{ 0 };
  std::atomic<uint64_t> _total_stlf_hazards_detected{ 0 };
  std::atomic<uint64_t> _total_stlf_hazards_neutralized{ 0 };

public:
  RedjetSpeculativePipeline();
  ~RedjetSpeculativePipeline();

  // 1. Branch Profiling & Bias Estimation
  RJ_API void record_branch_outcome(uint64_t branch_pc, uint64_t target_pc, bool taken);
  RJ_API RedjetBranchBias get_branch_bias(uint64_t branch_pc) const noexcept;

  // 2. BTB Warmer (primes CPU branch predictor before hot-swap)
  RJ_API void warm_branch_targets(const uint64_t* branch_pcs, size_t count);

  // 3. Store-to-Load Forwarding (STLF) Hazard Analysis
  // Checks if a store followed by a load at given offsets causes a pipeline bubble
  RJ_API RedjetStlfHazard analyze_stlf_hazard(size_t store_off, size_t store_bytes,
                                             size_t load_off, size_t load_bytes) noexcept;

  // 4. Injects padding or schedule adjustments into machine code buffer to neutralize hazard
  RJ_API bool neutralize_stlf_hazard(uint8_t* code_buffer, size_t code_len,
                                    const RedjetStlfHazard& hazard);

  // Telemetry
  uint64_t branches_profiled()     const noexcept { return _total_branches_profiled.load(std::memory_order_relaxed); }
  uint64_t btb_warmed()            const noexcept { return _total_btb_entries_warmed.load(std::memory_order_relaxed); }
  uint64_t stlf_hazards_detected() const noexcept { return _total_stlf_hazards_detected.load(std::memory_order_relaxed); }
  uint64_t stlf_neutralized()      const noexcept { return _total_stlf_hazards_neutralized.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETSPECULATIVEPIPELINE_HPP
