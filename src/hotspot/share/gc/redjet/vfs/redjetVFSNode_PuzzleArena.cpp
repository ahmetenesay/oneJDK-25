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

#include "gc/redjet/vfs/redjetVFSNode_PuzzleArena.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <algorithm>

RedjetVFSNode_PuzzleArena::RedjetVFSNode_PuzzleArena(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  for (size_t i = 0; i < STAGNANT_RING_CAPACITY; ++i) {
    _stagnant_ring[i].method_id = 0;
    _stagnant_ring[i].nmethod_ptr = nullptr;
    _stagnant_ring[i].invocation_count = 0;
    _stagnant_ring[i].estimated_tsc_gain = 0;
    _stagnant_ring[i].admission_tsc = 0;
    _stagnant_ring[i].is_occupied.store(false, std::memory_order_relaxed);
    _stagnant_ring[i].promoted_to_eval = false;
  }

  for (size_t i = 0; i < EVAL_LANE_COUNT; ++i) {
    _eval_lanes[i].lane_id = static_cast<uint32_t>(i);
    _eval_lanes[i].is_busy.store(false, std::memory_order_relaxed);
    _eval_lanes[i].method_id = 0;
    _eval_lanes[i].duel_start_tsc = 0;
    _eval_lanes[i].baseline_cycles = 0;
    _eval_lanes[i].candidate_cycles = 0;
    _eval_lanes[i].pmu_inst_retired = 0;
    _eval_lanes[i].l1d_misses = 0;
    _eval_lanes[i].measured_speedup = 1.0;
    _eval_lanes[i].precision_verified = false;
    _eval_lanes[i].hotswap_armed = false;
  }
}

RedjetVFSNode_PuzzleArena::~RedjetVFSNode_PuzzleArena() {
  for (size_t i = 0; i < STAGNANT_RING_CAPACITY; ++i) {
    _stagnant_ring[i].is_occupied.store(false, std::memory_order_relaxed);
  }
  for (size_t i = 0; i < EVAL_LANE_COUNT; ++i) {
    _eval_lanes[i].is_busy.store(false, std::memory_order_relaxed);
  }
}

bool RedjetVFSNode_PuzzleArena::admit_to_stagnant(
    uint32_t method_id, void* nmethod_ptr, uint32_t call_count, uint64_t estimated_gain) {

  if (nmethod_ptr == nullptr || call_count < 256) {
    return false; // Does not cross 256-invocation barrier
  }

  hs_ticket_guard_t guard(_stagnant_lock);

  // If ring is full, evict lowest TSC gain candidate (LFU eviction policy)
  if (active_stagnant_count() >= STAGNANT_RING_CAPACITY) {
    evict_lowest_tsc_gain_stagnant();
  }

  uint64_t cur = _stagnant_cursor.fetch_add(1, std::memory_order_relaxed) & STAGNANT_MASK;
  RedjetPuzzleStagnantSlot& slot = _stagnant_ring[cur];

  slot.method_id = method_id;
  slot.nmethod_ptr = nmethod_ptr;
  slot.invocation_count = call_count;
  slot.estimated_tsc_gain = (estimated_gain > 0) ? estimated_gain : (call_count * 10ULL);
  slot.admission_tsc = sacculus_rdtsc();
  slot.promoted_to_eval = false;
  slot.is_occupied.store(true, std::memory_order_release);

  _total_stagnant_admissions.fetch_add(1, std::memory_order_relaxed);
  return true;
}

int RedjetVFSNode_PuzzleArena::acquire_eval_lane(uint32_t method_id) {
  hs_ticket_guard_t guard(_arena_lock);

  for (size_t i = 0; i < EVAL_LANE_COUNT; ++i) {
    bool expected = false;
    if (_eval_lanes[i].is_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      _eval_lanes[i].method_id = method_id;
      _eval_lanes[i].duel_start_tsc = sacculus_rdtsc();
      _eval_lanes[i].baseline_cycles = 0;
      _eval_lanes[i].candidate_cycles = 0;
      _eval_lanes[i].pmu_inst_retired = 0;
      _eval_lanes[i].l1d_misses = 0;
      _eval_lanes[i].measured_speedup = 1.0;
      _eval_lanes[i].precision_verified = false;
      _eval_lanes[i].hotswap_armed = false;

      _total_eval_dispatches.fetch_add(1, std::memory_order_relaxed);
      return static_cast<int>(i);
    }
  }

  return -1; // All 64 lanes busy
}

bool RedjetVFSNode_PuzzleArena::release_eval_lane(
    uint32_t lane_id, uint64_t baseline_tsc, uint64_t candidate_tsc,
    uint64_t pmu_insts, uint64_t l1d_miss, bool precision_ok) {

  if (lane_id >= EVAL_LANE_COUNT) return false;

  RedjetPuzzleEvalLane& lane = _eval_lanes[lane_id];
  hs_ticket_guard_t guard(lane.lane_lock);

  lane.baseline_cycles = baseline_tsc;
  lane.candidate_cycles = candidate_tsc;
  lane.pmu_inst_retired = pmu_insts;
  lane.l1d_misses = l1d_miss;
  lane.precision_verified = precision_ok;

  bool duel_won = false;
  if (precision_ok && candidate_tsc > 0) {
    double speedup = static_cast<double>(baseline_tsc) / static_cast<double>(candidate_tsc);
    lane.measured_speedup = speedup;

    if (speedup >= MIN_WINNING_SPEEDUP) {
      duel_won = true;
      lane.hotswap_armed = true;
      uint64_t saved = (baseline_tsc > candidate_tsc) ? (baseline_tsc - candidate_tsc) : 0;
      _total_duels_won.fetch_add(1, std::memory_order_relaxed);
      _total_cycles_saved.fetch_add(saved, std::memory_order_relaxed);
    }
  }

  lane.is_busy.store(false, std::memory_order_release);
  return duel_won;
}

bool RedjetVFSNode_PuzzleArena::evict_lowest_tsc_gain_stagnant() {
  size_t lowest_idx = STAGNANT_RING_CAPACITY;
  uint64_t min_gain = UINT64_MAX;

  for (size_t i = 0; i < STAGNANT_RING_CAPACITY; ++i) {
    if (_stagnant_ring[i].is_occupied.load(std::memory_order_relaxed)) {
      if (_stagnant_ring[i].estimated_tsc_gain < min_gain) {
        min_gain = _stagnant_ring[i].estimated_tsc_gain;
        lowest_idx = i;
      }
    }
  }

  if (lowest_idx < STAGNANT_RING_CAPACITY) {
    _stagnant_ring[lowest_idx].is_occupied.store(false, std::memory_order_release);
    _stagnant_ring[lowest_idx].nmethod_ptr = nullptr;
    _total_lfu_evictions.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

size_t RedjetVFSNode_PuzzleArena::active_stagnant_count() const noexcept {
  size_t count = 0;
  for (size_t i = 0; i < STAGNANT_RING_CAPACITY; ++i) {
    if (_stagnant_ring[i].is_occupied.load(std::memory_order_relaxed)) {
      ++count;
    }
  }
  return count;
}

size_t RedjetVFSNode_PuzzleArena::busy_lanes_count() const noexcept {
  size_t count = 0;
  for (size_t i = 0; i < EVAL_LANE_COUNT; ++i) {
    if (_eval_lanes[i].is_busy.load(std::memory_order_relaxed)) {
      ++count;
    }
  }
  return count;
}

void RedjetVFSNode_PuzzleArena::print_on(outputStream* st) const {
  st->print_cr("Redjet Puzzle Arena Node [%s]:", full_path());
  st->print_cr("  Stagnant Pool: %zu / %zu slots occupied", active_stagnant_count(), STAGNANT_RING_CAPACITY);
  st->print_cr("  Eval Lanes: %zu / %zu lanes currently active", busy_lanes_count(), EVAL_LANE_COUNT);
  st->print_cr("  Total Admissions: " UINT64_FORMAT ", Total Dispatches: " UINT64_FORMAT,
               total_stagnant_admissions(), total_eval_dispatches());
  st->print_cr("  Duels Won: " UINT64_FORMAT ", Cycles Saved: " UINT64_FORMAT,
               total_duels_won(), total_cycles_saved());
  st->print_cr("  LFU Evictions: " UINT64_FORMAT, total_lfu_evictions());
}
