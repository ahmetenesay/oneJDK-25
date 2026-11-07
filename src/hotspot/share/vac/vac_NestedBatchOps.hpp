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

#ifndef SHARE_VAC_VAC_NESTEDBATCHOPS_HPP
#define SHARE_VAC_VAC_NESTEDBATCHOPS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "utilities/globalDefinitions.hpp"

// ============================================================================
// VAC_NestedBatchOps
//
// Compiler-First Implementation of Nested Superloop Flattening & 16-Deep LIFO
// `BatchScope` Arena Stack-Tree with Universal GC Compatibility:
//
// 1. Nested Loop Superloop Flattener (`VAC_NestedLoopFlattener`):
//    Transforms `k`-dimensional nested loops `(N_0, N_1, ..., N_{k-1})` into
//    either:
//      - Flattened 1D linear superloop `[0 .. prod(N_i))` with reciprocal
//        Barrett/magic-multiplier index recovery (eliminating inner branch
//        mispredictions when inner trip counts are small/irregular), OR
//      - L1D/SRAM Cache-Blocked Strip-Mined Tiles when working set exceeds L1D.
//
// 2. 16-Deep LIFO `BatchScope` Stack-Tree (`VAC_BatchScopeStack`):
//    - Under G1 / ZGC / Shenandoah / Parallel / Serial:
//      Uses thread-local scratch arena / TLAB watermark rewind for non-escaping
//      ephemeral batch allocations, and promotes escaping return values cleanly.
//    - Under RedjetGC (`-XX:+UseRedjetGC`):
//      Synergizes with Redjet Runway / `batch_ring` for O(1) 0-cycle bulk
//      cursor rewind on scope exit and suppresses per-object GC write barriers
//      inside non-escaping batch regions.
// ============================================================================

struct VAC_LoopNestDim {
  int64_t lower_bound;
  int64_t upper_bound;
  int64_t step;
  int64_t stride_bytes;
};

enum class VAC_NestStrategy : uint8_t {
  KEEP_HIERARCHICAL     = 0,
  FLATTEN_1D_SUPERLOOP  = 1, // Collapse k loops into single linear PILF loop
  CACHE_TILED_L1D_BLOCK = 2  // Strip-mine into L1D/SRAM resident tiles
};

struct VAC_FlattenedNestPlan {
  static constexpr size_t MAX_NEST_DEPTH = 8;

  VAC_NestStrategy strategy{ VAC_NestStrategy::KEEP_HIERARCHICAL };
  uint32_t         original_depth{ 0 };
  uint64_t         total_trip_count{ 0 };
  uint64_t         dim_extents[MAX_NEST_DEPTH]{ 0 };
  uint64_t         radix_divisors[MAX_NEST_DEPTH]{ 0 };
  uint32_t         recommended_tile_elems{ 64 };
  uint32_t         recommended_unroll{ 4 };
  bool             contiguous_stride_1{ false };
};

struct VAC_BatchFrame {
  uint32_t  scope_depth;
  uint32_t  group_hex_id;
  uintptr_t arena_base;
  uintptr_t watermark_offset;
  size_t    bytes_allocated;
  bool      has_escaping_return;
  bool      redjet_turbo_mode;
};

class VAC_BatchScopeStack {
public:
  static constexpr size_t MAX_BATCH_DEPTH       = 16;
  static constexpr size_t DEFAULT_ARENA_BYTES   = 256 * 1024; // 256 KB L2-resident per-thread batch scratch

private:
  uint8_t*       _scratch_arena{ nullptr };
  size_t         _arena_capacity{ 0 };
  size_t         _current_offset{ 0 };
  uint32_t       _depth{ 0 };
  bool           _owns_scratch{ false };
  VAC_BatchFrame _frames[MAX_BATCH_DEPTH];

  // Telemetry
  uint64_t       _scopes_entered{ 0 };
  uint64_t       _zero_cycle_rewinds{ 0 };
  uint64_t       _bytes_reclaimed_on_exit{ 0 };
  uint64_t       _escaped_promotions{ 0 };

public:
  explicit VAC_BatchScopeStack(size_t arena_bytes = DEFAULT_ARENA_BYTES);
  ~VAC_BatchScopeStack();

  // Enter nested batch scope (depth 1 .. 16)
  bool enter_scope(uint32_t group_hex_id = 0) noexcept;

  // Bump-allocate inside current batch scope (64-byte SIMD aligned by default)
  void* batch_alloc(size_t bytes, size_t alignment = 64) noexcept;

  // Exit current batch scope: O(1) 0-cycle watermark rewind if non-escaping;
  // copies/promotes single escaping result if `escaping_result_ptr != nullptr`.
  void* exit_scope(const void* escaping_result_ptr = nullptr, size_t escaping_bytes = 0) noexcept;

  uint32_t current_depth() const noexcept          { return _depth; }
  size_t   current_used_bytes() const noexcept     { return _current_offset; }
  uint64_t scopes_entered() const noexcept         { return _scopes_entered; }
  uint64_t zero_cycle_rewinds() const noexcept     { return _zero_cycle_rewinds; }
  uint64_t bytes_reclaimed() const noexcept        { return _bytes_reclaimed_on_exit; }
  uint64_t escaped_promotions() const noexcept     { return _escaped_promotions; }
};

class VAC_NestedBatchOps {
public:
  // Analyzes a k-deep loop nest and synthesizes the optimal flattening or L1D tiling plan
  static VAC_FlattenedNestPlan plan_nested_loops(const VAC_LoopNestDim* dims,
                                                 size_t depth,
                                                 size_t elem_size_bytes,
                                                 size_t l1d_cache_bytes = 49152) noexcept;

  // Reconstructs multi-dimensional loop coordinates from linear flattened index `flat_idx`
  static void reconstruct_coordinates(const VAC_FlattenedNestPlan& plan,
                                      uint64_t flat_idx,
                                      uint64_t* out_coords) noexcept;
};

#endif // SHARE_VAC_VAC_NESTEDBATCHOPS_HPP
