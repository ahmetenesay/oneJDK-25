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

#include "gc/redjet/engine/redjetBREE.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/runtime/redjetOSAffinity.hpp"
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

// Thread-local LIFO Stack-Tree (16 Derinlikli LIFO Yığını)
thread_local RedjetBREEStackFrame RedjetBREE::tl_hs_batch_stack[REDJET_BREE_STACK_CAPACITY];
thread_local size_t               RedjetBREE::tl_hs_batch_depth = 0;

RedjetBREE g_redjet_bree;

RedjetBREE::RedjetBREE() {
  _global_ring_storage = nullptr;
  _storage_bytes = 0;
  _total_batch_scopes_entered.store(0, std::memory_order_relaxed);
  _total_nested_frames_pushed.store(0, std::memory_order_relaxed);
  _total_evaporations.store(0, std::memory_order_relaxed);
  _total_evaporated_bytes.store(0, std::memory_order_relaxed);
  _total_promotions.store(0, std::memory_order_relaxed);
  _total_safepoint_suppressions.store(0, std::memory_order_relaxed);
}

RedjetBREE::~RedjetBREE() {
  destroy();
}

RedjetBREE& RedjetBREE::instance() {
  return g_redjet_bree;
}

void RedjetBREE::init(void* ring_storage, size_t ring_bytes) {
  hs_ticket_guard_t guard(_bree_lock);
  _global_ring_storage = static_cast<uint8_t*>(ring_storage);
  _storage_bytes = ring_bytes;
}

void RedjetBREE::destroy() {
  hs_ticket_guard_t guard(_bree_lock);
  _global_ring_storage = nullptr;
  _storage_bytes = 0;
}

// =========================================================================
// 1. Nested Scope Lifecycle (Push / Pop 16-Depth LIFO)
// =========================================================================
bool RedjetBREE::enter_batch_scope(uint32_t scope_id,
                                   const char* name,
                                   bool morph,
                                   uint32_t prefetch,
                                   RedjetGroupPriority priority) {
  if (tl_hs_batch_depth >= REDJET_BREE_STACK_CAPACITY) {
    // Stack overflow guard: LIFO capacity reached
    return false;
  }

  RedjetBREEStackFrame& frame = tl_hs_batch_stack[tl_hs_batch_depth];
  frame.reset();
  frame.scope_id = scope_id;
  frame.entry_tsc = sacculus_rdtsc();
  frame.inherited_priority = priority;
  frame.safepoint_suppression_active = true;
  frame.morph_enabled = morph;
  frame.prefetch_distance = prefetch;

  // Allocate unique 4-byte Group Hex ID for this scope
  frame.group_hex_id = RedjetUpclocker::instance().allocate_group_hex_id(
      name, 0, priority);

  // Silicon Affinity: Pin to Performance Cores (P-Cores)
  if (static_cast<uint8_t>(priority) >= static_cast<uint8_t>(RedjetGroupPriority::PRIORITY_HIGH)) {
    hs_bind_current_thread(HS_CORE_TIER_PERFORMANCE);
    frame.turbo_pinned = true;
  }

  // Assign memory slice in batch ring
  if (_global_ring_storage != nullptr) {
    size_t offset = (tl_hs_batch_depth * REDJET_BREE_RING_CAPACITY) % (_storage_bytes > 0 ? _storage_bytes : 1);
    frame.batch_ring_base = _global_ring_storage + offset;
    frame.watermark_bytes = 0;
  }

  tl_hs_batch_depth++;

  _total_batch_scopes_entered.fetch_add(1, std::memory_order_relaxed);
  if (tl_hs_batch_depth > 1) {
    _total_nested_frames_pushed.fetch_add(1, std::memory_order_relaxed);
  }
  _total_safepoint_suppressions.fetch_add(1, std::memory_order_relaxed);

  // Fast binary hotlog emission
  RedjetUpclocker::instance().emit_hex_hotlog(frame.group_hex_id, 0x01, frame.entry_tsc);

  return true;
}

bool RedjetBREE::exit_batch_scope(uint32_t scope_id) {
  if (tl_hs_batch_depth == 0) {
    return false;
  }

  // Pop top frame
  size_t top_idx = tl_hs_batch_depth - 1;
  RedjetBREEStackFrame& frame = tl_hs_batch_stack[top_idx];

  if (frame.scope_id != scope_id && scope_id != UINT32_MAX) {
    // Unmatched scope pop; proceed with top anyway for exception safety
  }

  // Single-cycle evaporation of all unpromoted memory allocated in this scope
  size_t evaporated = frame.watermark_bytes;
  frame.watermark_bytes = 0;
  _total_evaporations.fetch_add(1, std::memory_order_relaxed);
  _total_evaporated_bytes.fetch_add(evaporated, std::memory_order_relaxed);

  // Release Group Hex ID
  RedjetUpclocker::instance().release_group_hex_id(frame.group_hex_id);

  tl_hs_batch_depth--;

  // =========================================================================
  // ANTI-PREMATURE RESET INVARIANT:
  // If tl_hs_batch_depth > 0, an outer parent scope is still active!
  // We NEVER reset P-Core affinity or turbo boost early.
  // We only restore normal affinity when the outermost scope exits (depth == 0).
  // =========================================================================
  if (tl_hs_batch_depth == 0) {
    // Outermost scope exited: restore normal thread affinity and CPU power state
    hs_bind_current_thread(HS_CORE_TIER_ALL);
  }

  return true;
}

// =========================================================================
// 2. Batch Memory Bump Allocation & Fast Evaporation
// =========================================================================
void* RedjetBREE::allocate_in_batch_ring(size_t bytes) {
  if (tl_hs_batch_depth == 0) {
    return nullptr;
  }

  RedjetBREEStackFrame& frame = tl_hs_batch_stack[tl_hs_batch_depth - 1];
  if (frame.batch_ring_base == nullptr) {
    return nullptr;
  }

  // 16-byte alignment minimum
  size_t aligned = (bytes + 15) & ~static_cast<size_t>(15);
  if (frame.watermark_bytes + aligned <= REDJET_BREE_RING_CAPACITY) {
    void* result = frame.batch_ring_base + frame.watermark_bytes;
    frame.watermark_bytes += aligned;
    return result;
  }

  return nullptr; // Batch ring slice exhausted
}

size_t RedjetBREE::evaporate_current_scope() {
  if (tl_hs_batch_depth == 0) {
    return 0;
  }
  RedjetBREEStackFrame& frame = tl_hs_batch_stack[tl_hs_batch_depth - 1];
  size_t reclaimed = frame.watermark_bytes;
  frame.watermark_bytes = 0; // Single-cycle evaporation (top = 0)
  _total_evaporations.fetch_add(1, std::memory_order_relaxed);
  _total_evaporated_bytes.fetch_add(reclaimed, std::memory_order_relaxed);
  return reclaimed;
}

bool RedjetBREE::promote_object_to_parent(void* obj_ptr, size_t size_bytes) {
  if (obj_ptr == nullptr || size_bytes == 0 || tl_hs_batch_depth <= 1) {
    return false;
  }

  // Copy object to parent stack frame (depth - 2)
  RedjetBREEStackFrame& parent = tl_hs_batch_stack[tl_hs_batch_depth - 2];
  if (parent.batch_ring_base == nullptr) {
    return false;
  }

  size_t aligned = (size_bytes + 15) & ~static_cast<size_t>(15);
  if (parent.watermark_bytes + aligned <= REDJET_BREE_RING_CAPACITY) {
    void* dst = parent.batch_ring_base + parent.watermark_bytes;
    std::memcpy(dst, obj_ptr, size_bytes);
    parent.watermark_bytes += aligned;
    parent.promoted_objects_count++;
    _total_promotions.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

void RedjetBREE::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet BREE (Batch Register & Entangle Execution) Engine:");
  st->print_cr("  Total Scopes Entered:       " UINT64_FORMAT, total_batch_scopes());
  st->print_cr("  Total Nested Frames Pushed: " UINT64_FORMAT, total_nested_frames());
  st->print_cr("  Total Evaporations:         " UINT64_FORMAT, total_evaporations());
  st->print_cr("  Total Evaporated Bytes:     " UINT64_FORMAT, total_evaporated_bytes());
  st->print_cr("  Total Objects Promoted:     " UINT64_FORMAT, total_promotions());
  st->print_cr("  Safepoint Suppressions:     " UINT64_FORMAT, total_safepoint_suppressions());
}
