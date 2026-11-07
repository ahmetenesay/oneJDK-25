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

#include "gc/redjet/puzzler/redjetLoopUnroller.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetLoopUnroller* RedjetLoopUnroller::_instance = nullptr;
std::atomic<bool>   RedjetLoopUnroller::_initialized{ false };

RedjetLoopUnroller::RedjetLoopUnroller()
  : _total_loops_analyzed(0),
    _total_loops_unrolled(0),
    _total_branches_eliminated(0) {
  hs_ticket_lock_init(&_unroller_lock);
}

RedjetLoopUnroller::~RedjetLoopUnroller() {
  // Release resources
}

void RedjetLoopUnroller::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetLoopUnroller();
  }
}

void RedjetLoopUnroller::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

RedjetUnrollFactor RedjetLoopUnroller::determine_optimal_factor(const RedjetLoopMetadata& meta) const noexcept {
  // Prevent L1-I instruction cache pressure if loop body is already large
  if (meta.body_instruction_count > MAX_LOOP_BODY_INSTRUCTIONS || meta.body_instruction_count == 0) {
    return RedjetUnrollFactor::UNROLL_NONE;
  }

  // Memory dependency hazards limit aggressive unrolling
  if (meta.has_memory_dependencies && meta.body_instruction_count > 32) {
    return RedjetUnrollFactor::UNROLL_2X;
  }

  if (meta.constant_trip_count > 0) {
    if ((meta.constant_trip_count % 16 == 0) && meta.body_instruction_count <= 16) {
      return RedjetUnrollFactor::UNROLL_16X;
    }
    if ((meta.constant_trip_count % 8 == 0) && meta.body_instruction_count <= 32) {
      return RedjetUnrollFactor::UNROLL_8X;
    }
    if ((meta.constant_trip_count % 4 == 0) && meta.body_instruction_count <= 64) {
      return RedjetUnrollFactor::UNROLL_4X;
    }
    if ((meta.constant_trip_count % 2 == 0) && meta.body_instruction_count <= 128) {
      return RedjetUnrollFactor::UNROLL_2X;
    }
    return RedjetUnrollFactor::UNROLL_NONE;
  }

  // Dynamic trip count heuristic
  if (meta.body_instruction_count <= 16) {
    return RedjetUnrollFactor::UNROLL_8X;
  }
  if (meta.body_instruction_count <= 32) {
    return RedjetUnrollFactor::UNROLL_4X;
  }
  if (meta.body_instruction_count <= 64) {
    return RedjetUnrollFactor::UNROLL_2X;
  }

  return RedjetUnrollFactor::UNROLL_NONE;
}

RedjetUnrollFactor RedjetLoopUnroller::evaluate_loop(const RedjetLoopMetadata& meta) noexcept {
  _total_loops_analyzed.fetch_add(1, std::memory_order_relaxed);
  return determine_optimal_factor(meta);
}

RedjetUnrollResult RedjetLoopUnroller::transform_loop(const RedjetLoopMetadata& meta,
                                                     const uint8_t* original_code,
                                                     size_t original_bytes,
                                                     uint8_t* output_buffer,
                                                     size_t output_buffer_capacity) {
  uint64_t start_tsc = sacculus_rdtsc();
  RedjetUnrollResult res;
  res.success = false;
  res.applied_factor = RedjetUnrollFactor::UNROLL_NONE;
  res.original_instructions = meta.body_instruction_count;
  res.unrolled_instructions = meta.body_instruction_count;
  res.estimated_branch_savings_percent = 0;
  res.processing_duration_tsc = 0;

  if (original_code == nullptr || output_buffer == nullptr || original_bytes == 0) {
    res.processing_duration_tsc = sacculus_rdtsc() - start_tsc;
    return res;
  }

  RedjetUnrollFactor factor = evaluate_loop(meta);
  if (factor == RedjetUnrollFactor::UNROLL_NONE) {
    // No unrolling; passthrough copy
    if (output_buffer_capacity >= original_bytes) {
      std::memcpy(output_buffer, original_code, original_bytes);
      res.success = true;
      res.applied_factor = RedjetUnrollFactor::UNROLL_NONE;
      res.unrolled_instructions = meta.body_instruction_count;
    }
    res.processing_duration_tsc = sacculus_rdtsc() - start_tsc;
    return res;
  }

  uint32_t unroll_multiplier = static_cast<uint32_t>(factor);
  size_t estimated_unrolled_bytes = original_bytes * unroll_multiplier;

  if (estimated_unrolled_bytes > output_buffer_capacity) {
    // Insufficient buffer space
    res.processing_duration_tsc = sacculus_rdtsc() - start_tsc;
    return res;
  }

  hs_ticket_lock_acquire(&_unroller_lock);

  // Weave unrolled body copies
  uint8_t* out_ptr = output_buffer;
  for (uint32_t rep = 0; rep < unroll_multiplier; ++rep) {
    std::memcpy(out_ptr, original_code, original_bytes);
    out_ptr += original_bytes;
  }

  // Calculate branch savings
  // For unroll factor N, N-1 branch evaluations are eliminated per unrolled iteration
  size_t branches_saved = (unroll_multiplier - 1);
  _total_branches_eliminated.fetch_add(branches_saved, std::memory_order_relaxed);
  _total_loops_unrolled.fetch_add(1, std::memory_order_relaxed);

  res.success = true;
  res.applied_factor = factor;
  res.unrolled_instructions = meta.body_instruction_count * unroll_multiplier;
  res.estimated_branch_savings_percent = ((unroll_multiplier - 1) * 100) / unroll_multiplier;

  hs_ticket_lock_release(&_unroller_lock);

  res.processing_duration_tsc = sacculus_rdtsc() - start_tsc;
  return res;
}

void RedjetLoopUnroller::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetLoopUnroller] Puzzler Step 4 Loop Optimizer Status:");
  st->print_cr("  - Loops Analyzed:       %llu",
               static_cast<unsigned long long>(_total_loops_analyzed.load(std::memory_order_relaxed)));
  st->print_cr("  - Loops Unrolled:       %llu",
               static_cast<unsigned long long>(_total_loops_unrolled.load(std::memory_order_relaxed)));
  st->print_cr("  - Branches Eliminated:  %llu branch evaluations saved",
               static_cast<unsigned long long>(_total_branches_eliminated.load(std::memory_order_relaxed)));
}
