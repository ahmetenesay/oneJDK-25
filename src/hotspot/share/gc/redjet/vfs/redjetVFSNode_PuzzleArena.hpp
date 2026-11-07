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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_PUZZLEARENA_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_PUZZLEARENA_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Puzzle Arena VFS Node (/heap/on_heap/puzzle_arena) (PLAN.md Section 8.8 & 9.1)
// - Stagnant: 256-slot circular ring buffer for optimization candidates
// - Eval: 64 parallel lanes for 64-run TSC-Delta duels (RDTSC, PMU, L1-D Misses)
// - TSC-Delta LFU eviction policy (lowest cycle gain evicted when saturated)
// ============================================================================

struct alignas(64) RedjetPuzzleStagnantSlot {
  uint32_t               method_id{ 0 };
  void*                  nmethod_ptr{ nullptr };
  uint32_t               invocation_count{ 0 };
  uint64_t               estimated_tsc_gain{ 0 };
  uint64_t               admission_tsc{ 0 };
  std::atomic<bool>      is_occupied{ false };
  bool                   promoted_to_eval{ false };
};

struct alignas(64) RedjetPuzzleEvalLane {
  uint32_t               lane_id{ 0 };
  std::atomic<bool>      is_busy{ false };
  uint32_t               method_id{ 0 };
  uint64_t               duel_start_tsc{ 0 };
  uint64_t               baseline_cycles{ 0 };
  uint64_t               candidate_cycles{ 0 };
  uint64_t               pmu_inst_retired{ 0 };
  uint64_t               l1d_misses{ 0 };
  double                 measured_speedup{ 1.0 };
  bool                   precision_verified{ false };
  bool                   hotswap_armed{ false };
  hs_ticket_lock_t       lane_lock;
};

class RedjetVFSNode_PuzzleArena : public RedjetVFSNode {
public:
  static constexpr size_t STAGNANT_RING_CAPACITY = 256; // 256-slot circular ring buffer
  static constexpr size_t STAGNANT_MASK = STAGNANT_RING_CAPACITY - 1;
  static constexpr size_t EVAL_LANE_COUNT = 64;        // 64 parallel duel lanes
  static constexpr double MIN_WINNING_SPEEDUP = 1.05;   // >= 5% net speedup required
  static constexpr size_t DEFAULT_QUOTA = 128ULL * 1024 * 1024; // 128 MB

private:
  RedjetPuzzleStagnantSlot          _stagnant_ring[STAGNANT_RING_CAPACITY];
  alignas(64) std::atomic<uint64_t> _stagnant_cursor{ 0 };
  hs_ticket_lock_t                  _stagnant_lock;

  RedjetPuzzleEvalLane              _eval_lanes[EVAL_LANE_COUNT];
  hs_ticket_lock_t                  _arena_lock;

  // Telemetry
  std::atomic<uint64_t> _total_stagnant_admissions{ 0 };
  std::atomic<uint64_t> _total_eval_dispatches{ 0 };
  std::atomic<uint64_t> _total_duels_won{ 0 };
  std::atomic<uint64_t> _total_cycles_saved{ 0 };
  std::atomic<uint64_t> _total_lfu_evictions{ 0 };

public:
  RedjetVFSNode_PuzzleArena(const char* name = "puzzle_arena",
                            const char* full_path = "/heap/on_heap/puzzle_arena",
                            RedjetVFSNode* parent = nullptr,
                            size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_PuzzleArena();

  // 1. Stagnant Pool: Admits candidate crossing 256-call threshold into circular ring
  RJ_API bool admit_to_stagnant(uint32_t method_id, void* nmethod_ptr, uint32_t call_count, uint64_t estimated_gain = 0);

  // 2. Eval Lanes: Acquires a parallel lane for 64-run TSC-Delta duel
  RJ_API int acquire_eval_lane(uint32_t method_id);

  // 3. Eval Lanes: Records duel result and releases lane
  RJ_API bool release_eval_lane(uint32_t lane_id, uint64_t baseline_tsc, uint64_t candidate_tsc,
                                uint64_t pmu_insts, uint64_t l1d_miss, bool precision_ok);

  // 4. LFU Eviction: Discards candidate with lowest TSC-Delta gain when stagnant ring is full
  RJ_API bool evict_lowest_tsc_gain_stagnant();

  // 5. Query Active Counts
  size_t active_stagnant_count() const noexcept;
  size_t busy_lanes_count() const noexcept;

  // Sizing & Telemetry
  uint64_t total_stagnant_admissions() const noexcept { return _total_stagnant_admissions.load(std::memory_order_relaxed); }
  uint64_t total_eval_dispatches()     const noexcept { return _total_eval_dispatches.load(std::memory_order_relaxed); }
  uint64_t total_duels_won()           const noexcept { return _total_duels_won.load(std::memory_order_relaxed); }
  uint64_t total_cycles_saved()        const noexcept { return _total_cycles_saved.load(std::memory_order_relaxed); }
  uint64_t total_lfu_evictions()       const noexcept { return _total_lfu_evictions.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_PUZZLEARENA_HPP
