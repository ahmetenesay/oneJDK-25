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

#include "vac/vac_NestedBatchOps.hpp"
#include "gc/shared/gc_globals.hpp"
#include "memory/allocation.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_REDJETGC
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "memory/universe.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

// ============================================================================
// 1. VAC_BatchScopeStack Implementation (Universal GC + Redjet Turbo Synergy)
// ============================================================================

VAC_BatchScopeStack::VAC_BatchScopeStack(size_t arena_bytes)
  : _scratch_arena(nullptr),
    _arena_capacity(arena_bytes > 4096 ? arena_bytes : 4096),
    _current_offset(0),
    _depth(0),
    _owns_scratch(false) {
  std::memset(_frames, 0, sizeof(_frames));
  _scratch_arena = NEW_C_HEAP_ARRAY(uint8_t, _arena_capacity, mtCompiler);
  _owns_scratch  = (_scratch_arena != nullptr);

#if INCLUDE_REDJETGC
  if (UseRedjetGC && Universe::is_fully_initialized() && _owns_scratch) {
    RedjetHeap::heap()->vfs_tree()->record_domain_alloc(
        RedjetMemoryDomain::JAVA_MEMORY, true, _arena_capacity);
  }
#endif
}

VAC_BatchScopeStack::~VAC_BatchScopeStack() {
  if (_owns_scratch && _scratch_arena != nullptr) {
#if INCLUDE_REDJETGC
    if (UseRedjetGC && Universe::is_fully_initialized()) {
      RedjetHeap::heap()->vfs_tree()->record_domain_free(
          RedjetMemoryDomain::JAVA_MEMORY, true, _arena_capacity);
    }
#endif
    FREE_C_HEAP_ARRAY(uint8_t, _scratch_arena);
    _scratch_arena = nullptr;
    _owns_scratch  = false;
  }
}

bool VAC_BatchScopeStack::enter_scope(uint32_t group_hex_id) noexcept {
  if (_depth >= MAX_BATCH_DEPTH || _scratch_arena == nullptr) {
    return false;
  }

  VAC_BatchFrame& f      = _frames[_depth];
  f.scope_depth          = _depth + 1;
  f.group_hex_id         = group_hex_id;
  f.arena_base           = reinterpret_cast<uintptr_t>(_scratch_arena);
  f.watermark_offset     = _current_offset;
  f.bytes_allocated      = 0;
  f.has_escaping_return  = false;
#if INCLUDE_REDJETGC
  f.redjet_turbo_mode    = UseRedjetGC;
#else
  f.redjet_turbo_mode    = false;
#endif

  _depth++;
  _scopes_entered++;
  return true;
}

void* VAC_BatchScopeStack::batch_alloc(size_t bytes, size_t alignment) noexcept {
  if (_depth == 0 || _scratch_arena == nullptr || bytes == 0) {
    return nullptr;
  }
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    alignment = 64;
  }

  uintptr_t base_addr  = reinterpret_cast<uintptr_t>(_scratch_arena);
  uintptr_t cur_addr   = base_addr + _current_offset;
  uintptr_t aligned    = (cur_addr + alignment - 1) & ~(alignment - 1);
  size_t    new_offset = static_cast<size_t>(aligned - base_addr) + bytes;

  if (new_offset > _arena_capacity) {
    return nullptr; // Caller falls back to standard GC TLAB allocation
  }

  size_t delta = new_offset - _current_offset;
  _current_offset = new_offset;
  _frames[_depth - 1].bytes_allocated += delta;

  void* ptr = reinterpret_cast<void*>(aligned);
  std::memset(ptr, 0, bytes);
  return ptr;
}

void* VAC_BatchScopeStack::exit_scope(const void* escaping_result_ptr, size_t escaping_bytes) noexcept {
  if (_depth == 0) {
    return nullptr;
  }

  VAC_BatchFrame& f = _frames[_depth - 1];
  const size_t saved_watermark = f.watermark_offset;
  const size_t reclaimed       = (_current_offset >= saved_watermark)
                                     ? (_current_offset - saved_watermark)
                                     : 0;
  _depth--;

  void* promoted_out = nullptr;
  if (escaping_result_ptr != nullptr && escaping_bytes > 0) {
    f.has_escaping_return = true;
    _escaped_promotions++;

    if (_depth > 0) {
      // Promote escaping value one level up into parent BatchScope!
      _current_offset = saved_watermark;
      promoted_out = batch_alloc(escaping_bytes, 64);
      if (promoted_out != nullptr) {
        std::memcpy(promoted_out, escaping_result_ptr, escaping_bytes);
      }
      _bytes_reclaimed_on_exit += (reclaimed > escaping_bytes ? reclaimed - escaping_bytes : 0);
      return promoted_out;
    } else {
      // Outermost scope escape: promote to heap/native survivor buffer
      uint8_t* survivor = NEW_C_HEAP_ARRAY(uint8_t, escaping_bytes, mtCompiler);
      std::memcpy(survivor, escaping_result_ptr, escaping_bytes);
      promoted_out = survivor;
    }
  }

  // O(1) 0-Cycle Bulk Watermark Rewind (eliminates individual object sweeping across ALL GCs!)
  _current_offset = saved_watermark;
  _zero_cycle_rewinds++;
  _bytes_reclaimed_on_exit += reclaimed;
  return promoted_out;
}

// ============================================================================
// 2. VAC_NestedBatchOps Superloop Flattening & Cache-Blocking Planner
// ============================================================================

VAC_FlattenedNestPlan VAC_NestedBatchOps::plan_nested_loops(const VAC_LoopNestDim* dims,
                                                            size_t depth,
                                                            size_t elem_size_bytes,
                                                            size_t l1d_cache_bytes) noexcept {
  VAC_FlattenedNestPlan plan{};
  if (dims == nullptr || depth == 0) {
    return plan;
  }
  if (depth > VAC_FlattenedNestPlan::MAX_NEST_DEPTH) {
    depth = VAC_FlattenedNestPlan::MAX_NEST_DEPTH;
  }

  plan.original_depth = static_cast<uint32_t>(depth);
  if (elem_size_bytes == 0) elem_size_bytes = 4;
  if (l1d_cache_bytes < 16384) l1d_cache_bytes = 32768;

  uint64_t total_trips = 1;
  bool     overflow    = false;
  for (size_t d = 0; d < depth; ++d) {
    int64_t step = (dims[d].step == 0) ? 1 : dims[d].step;
    int64_t span = dims[d].upper_bound - dims[d].lower_bound;
    uint64_t ext = 0;
    if ((step > 0 && span > 0) || (step < 0 && span < 0)) {
      int64_t abs_span = span < 0 ? -span : span;
      int64_t abs_step = step < 0 ? -step : step;
      ext = static_cast<uint64_t>((abs_span + abs_step - 1) / abs_step);
    }
    plan.dim_extents[d] = ext;
    if (ext == 0) {
      total_trips = 0;
    } else if (total_trips > UINT64_MAX / ext) {
      overflow = true;
    } else {
      total_trips *= ext;
    }
  }

  if (overflow || total_trips == 0) {
    plan.strategy         = VAC_NestStrategy::KEEP_HIERARCHICAL;
    plan.total_trip_count = 0;
    return plan;
  }

  plan.total_trip_count = total_trips;

  // Compute row-major radix divisors from innermost to outermost
  uint64_t running_div = 1;
  for (size_t idx = depth; idx > 0; --idx) {
    size_t d = idx - 1;
    plan.radix_divisors[d] = running_div;
    running_div *= (plan.dim_extents[d] > 0 ? plan.dim_extents[d] : 1);
  }

  // Check if memory layout across dimensions is contiguous unit-stride
  bool unit_stride = (dims[depth - 1].stride_bytes == static_cast<int64_t>(elem_size_bytes));
  for (size_t d = 0; d + 1 < depth && unit_stride; ++d) {
    int64_t expected = static_cast<int64_t>(plan.dim_extents[d + 1]) * dims[d + 1].stride_bytes;
    if (dims[d].stride_bytes != expected) {
      unit_stride = false;
    }
  }
  plan.contiguous_stride_1 = unit_stride;

  const uint64_t working_set_bytes = total_trips * elem_size_bytes;
  const uint64_t inner_trips       = plan.dim_extents[depth - 1];

  if (depth >= 2 && (unit_stride || inner_trips < 64) && working_set_bytes <= l1d_cache_bytes * 4) {
    // Collapse multi-dimensional nest into a single branchless 1D PILF superloop!
    plan.strategy               = VAC_NestStrategy::FLATTEN_1D_SUPERLOOP;
    plan.recommended_tile_elems = static_cast<uint32_t>(std::min<uint64_t>(total_trips, 512));
    plan.recommended_unroll     = (total_trips >= 64) ? 8u : 4u;
  } else if (working_set_bytes > l1d_cache_bytes) {
    // Working set exceeds L1D: apply cache-blocked strip-mining (target 50% of L1D for A+B+C residency)
    size_t tile_bytes = l1d_cache_bytes / 2;
    size_t tile_elems = tile_bytes / elem_size_bytes;
    // Round down to power-of-two multiple of 16 SIMD lanes
    uint32_t pow2_tile = 64;
    while ((pow2_tile << 1) <= tile_elems && pow2_tile < 1024) {
      pow2_tile <<= 1;
    }
    plan.strategy               = VAC_NestStrategy::CACHE_TILED_L1D_BLOCK;
    plan.recommended_tile_elems = pow2_tile;
    plan.recommended_unroll     = 8u;
  } else {
    plan.strategy               = VAC_NestStrategy::KEEP_HIERARCHICAL;
    plan.recommended_tile_elems = static_cast<uint32_t>(std::min<uint64_t>(inner_trips, 128));
    plan.recommended_unroll     = (inner_trips >= 32) ? 4u : 2u;
  }

  return plan;
}

void VAC_NestedBatchOps::reconstruct_coordinates(const VAC_FlattenedNestPlan& plan,
                                                 uint64_t flat_idx,
                                                 uint64_t* out_coords) noexcept {
  if (out_coords == nullptr || plan.original_depth == 0) return;
  uint64_t rem = flat_idx;
  for (uint32_t d = 0; d < plan.original_depth; ++d) {
    uint64_t div = plan.radix_divisors[d];
    if (div > 0) {
      out_coords[d] = rem / div;
      rem           = rem % div;
    } else {
      out_coords[d] = 0;
    }
  }
}
