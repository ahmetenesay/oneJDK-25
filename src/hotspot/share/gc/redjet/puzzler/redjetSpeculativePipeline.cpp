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

#include "gc/redjet/puzzler/redjetSpeculativePipeline.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetSpeculativePipeline::RedjetSpeculativePipeline()
  : _branch_count(0),
    _total_branches_profiled(0),
    _total_btb_entries_warmed(0),
    _total_stlf_hazards_detected(0),
    _total_stlf_hazards_neutralized(0) {
  hs_ticket_lock_init(&_pipeline_lock);
  std::memset(_branches, 0, sizeof(_branches));
}

RedjetSpeculativePipeline::~RedjetSpeculativePipeline() {
}

void RedjetSpeculativePipeline::record_branch_outcome(uint64_t branch_pc, uint64_t target_pc, bool taken) {
  hs_ticket_lock_acquire(&_pipeline_lock);

  RedjetBranchProfile* profile = nullptr;
  for (size_t i = 0; i < _branch_count; ++i) {
    if (_branches[i].branch_pc == branch_pc) {
      profile = &_branches[i];
      break;
    }
  }

  if (profile == nullptr && _branch_count < MAX_TRACKED_BRANCHES) {
    profile = &_branches[_branch_count++];
    profile->branch_pc = branch_pc;
    profile->target_pc = target_pc;
    profile->taken_count = 0;
    profile->not_taken_count = 0;
    profile->bias = RedjetBranchBias::STRONGLY_TAKEN;
  }

  if (profile != nullptr) {
    if (taken) {
      profile->taken_count++;
      if (profile->bias == RedjetBranchBias::STRONGLY_NOT_TAKEN) {
        profile->bias = RedjetBranchBias::WEAKLY_NOT_TAKEN;
      } else if (profile->bias == RedjetBranchBias::WEAKLY_NOT_TAKEN) {
        profile->bias = RedjetBranchBias::WEAKLY_TAKEN;
      } else if (profile->bias == RedjetBranchBias::WEAKLY_TAKEN) {
        profile->bias = RedjetBranchBias::STRONGLY_TAKEN;
      }
    } else {
      profile->not_taken_count++;
      if (profile->bias == RedjetBranchBias::STRONGLY_TAKEN) {
        profile->bias = RedjetBranchBias::WEAKLY_TAKEN;
      } else if (profile->bias == RedjetBranchBias::WEAKLY_TAKEN) {
        profile->bias = RedjetBranchBias::WEAKLY_NOT_TAKEN;
      } else if (profile->bias == RedjetBranchBias::WEAKLY_NOT_TAKEN) {
        profile->bias = RedjetBranchBias::STRONGLY_NOT_TAKEN;
      }
    }
    _total_branches_profiled.fetch_add(1, std::memory_order_relaxed);
  }

  hs_ticket_lock_release(&_pipeline_lock);
}

RedjetBranchBias RedjetSpeculativePipeline::get_branch_bias(uint64_t branch_pc) const noexcept {
  for (size_t i = 0; i < _branch_count; ++i) {
    if (_branches[i].branch_pc == branch_pc) {
      return _branches[i].bias;
    }
  }
  return RedjetBranchBias::STRONGLY_TAKEN;
}

void RedjetSpeculativePipeline::warm_branch_targets(const uint64_t* branch_pcs, size_t count) {
  if (branch_pcs == nullptr || count == 0) return;

  for (size_t i = 0; i < count; ++i) {
    uint64_t pc = branch_pcs[i];
    // Prime CPU Branch Target Buffer (BTB) using speculative prefetch hint
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
    _mm_prefetch(reinterpret_cast<const char*>(pc), _MM_HINT_T0);
#else
    __builtin_prefetch(reinterpret_cast<const void*>(pc), 0, 3);
#endif
#endif
    _total_btb_entries_warmed.fetch_add(1, std::memory_order_relaxed);
  }
}

RedjetStlfHazard RedjetSpeculativePipeline::analyze_stlf_hazard(size_t store_off, size_t store_bytes,
                                                               size_t load_off, size_t load_bytes) noexcept {
  RedjetStlfHazard hazard;
  hazard.store_offset = store_off;
  hazard.store_bytes = store_bytes;
  hazard.load_offset = load_off;
  hazard.load_bytes = load_bytes;
  hazard.is_exact_match = false;
  hazard.is_partial_overlap = false;
  hazard.recommended_nop_padding = 0;

  if (store_off == load_off && store_bytes == load_bytes) {
    // Exact match: 0-cycle forwarding
    hazard.is_exact_match = true;
    return hazard;
  }

  size_t store_end = store_off + store_bytes;
  size_t load_end  = load_off + load_bytes;

  // Check for partial overlap:
  // e.g. Store 8 bytes at 0, Load 4 bytes at 2 (misaligned sub-load)
  // or Store 4 bytes at 0, Load 8 bytes at 0 (load wider than store)
  bool overlaps = (load_off < store_end) && (load_end > store_off);

  if (overlaps) {
    if (load_off >= store_off && load_end <= store_end) {
      // Contained sub-load: modern CPUs can forward if naturally aligned
      if ((load_off - store_off) % load_bytes == 0) {
        hazard.is_exact_match = false;
        hazard.is_partial_overlap = false;
      } else {
        hazard.is_partial_overlap = true;
        hazard.recommended_nop_padding = 4;
        _total_stlf_hazards_detected.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      // Load exceeds store boundary: severe STLF failure!
      hazard.is_partial_overlap = true;
      hazard.recommended_nop_padding = 8;
      _total_stlf_hazards_detected.fetch_add(1, std::memory_order_relaxed);
    }
  }

  return hazard;
}

bool RedjetSpeculativePipeline::neutralize_stlf_hazard(uint8_t* code_buffer, size_t code_len,
                                                      const RedjetStlfHazard& hazard) {
  if (code_buffer == nullptr || code_len < 16 || !hazard.is_partial_overlap) {
    return false;
  }

  // Inject multi-byte NOPs between Store and Load to separate them by pipeline depth
  uint8_t nop_pad = hazard.recommended_nop_padding;
  if (nop_pad > 0 && nop_pad <= 15) {
    static const uint8_t nops_4byte[4] = { 0x0F, 0x1F, 0x40, 0x00 };
    static const uint8_t nops_8byte[8] = { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };

    if (nop_pad == 4 && code_len >= 4) {
      std::memcpy(code_buffer, nops_4byte, 4);
    } else if (nop_pad == 8 && code_len >= 8) {
      std::memcpy(code_buffer, nops_8byte, 8);
    } else {
      for (size_t i = 0; i < nop_pad && i < code_len; ++i) {
        code_buffer[i] = 0x90; // Standard 1-byte NOP
      }
    }
    _total_stlf_hazards_neutralized.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

void RedjetSpeculativePipeline::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Speculative Pipeline & STLF Hazard Analyzer ---");
  st->print_cr("  Tracked Branches:     %zu / %zu", _branch_count, MAX_TRACKED_BRANCHES);
  st->print_cr("  Branches Profiled:    %llu", static_cast<unsigned long long>(_total_branches_profiled.load(std::memory_order_relaxed)));
  st->print_cr("  BTB Entries Warmed:   %llu", static_cast<unsigned long long>(_total_btb_entries_warmed.load(std::memory_order_relaxed)));
  st->print_cr("  STLF Hazards Detected:%llu", static_cast<unsigned long long>(_total_stlf_hazards_detected.load(std::memory_order_relaxed)));
  st->print_cr("  STLF Hazards Resolved:%llu", static_cast<unsigned long long>(_total_stlf_hazards_neutralized.load(std::memory_order_relaxed)));
}
