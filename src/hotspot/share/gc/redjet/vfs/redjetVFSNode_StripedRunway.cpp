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

#include "gc/redjet/vfs/redjetVFSNode_StripedRunway.hpp"
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

RedjetVFSNode_StripedRunway::RedjetVFSNode_StripedRunway(const char* name,
                                                         const char* full_path,
                                                         RedjetVFSNode* parent,
                                                         size_t quota_bytes)
  : RedjetVFSNode(name, full_path, parent, quota_bytes),
    _active_stripe_count(2),
    _rr_index(0),
    _storage_pool(nullptr),
    _pool_bytes(0),
    _total_striped_allocations(0),
    _total_striped_bytes(0),
    _total_stripe_recycles(0) {

  for (size_t i = 0; i < REDJET_MAX_NUMA_STRIPES; ++i) {
    _stripes[i].channel_base = nullptr;
    _stripes[i].bump_cursor.store(nullptr, std::memory_order_relaxed);
    _stripes[i].channel_limit = nullptr;
    _stripes[i].stripe_id = static_cast<uint32_t>(i);
    _stripes[i].numa_node_id = static_cast<uint32_t>(i);
    _stripes[i].bytes_allocated.store(0, std::memory_order_relaxed);
    _stripes[i].stripe_cycles.store(0, std::memory_order_relaxed);
  }
}

RedjetVFSNode_StripedRunway::~RedjetVFSNode_StripedRunway() {
  destroy();
}

void RedjetVFSNode_StripedRunway::init(void* storage, size_t storage_bytes, size_t numa_nodes) {
  hs_ticket_guard_t guard(_striped_lock);
  _storage_pool = static_cast<uint8_t*>(storage);
  _pool_bytes = storage_bytes;
  _active_stripe_count = (numa_nodes > 0 && numa_nodes <= REDJET_MAX_NUMA_STRIPES) ? numa_nodes : 2;

  if (_storage_pool != nullptr && _pool_bytes > 0) {
    size_t bytes_per_stripe = _pool_bytes / _active_stripe_count;
    uint8_t* ptr = _storage_pool;

    for (size_t i = 0; i < _active_stripe_count; ++i) {
      _stripes[i].channel_base = ptr;
      _stripes[i].bump_cursor.store(ptr, std::memory_order_relaxed);
      _stripes[i].channel_limit = ptr + bytes_per_stripe;
      _stripes[i].stripe_id = static_cast<uint32_t>(i);
      _stripes[i].numa_node_id = static_cast<uint32_t>(i);
      _stripes[i].reset();
      ptr += bytes_per_stripe;
    }
  }
}

void RedjetVFSNode_StripedRunway::destroy() {
  hs_ticket_guard_t guard(_striped_lock);
  for (size_t i = 0; i < REDJET_MAX_NUMA_STRIPES; ++i) {
    _stripes[i].reset();
    _stripes[i].channel_base = nullptr;
    _stripes[i].bump_cursor.store(nullptr, std::memory_order_relaxed);
    _stripes[i].channel_limit = nullptr;
  }
  _storage_pool = nullptr;
  _pool_bytes = 0;
}

// Hash-based striped allocation across NUMA memory channels
void* RedjetVFSNode_StripedRunway::allocate_striped(uint32_t thread_id, size_t bytes) {
  if (bytes == 0) return nullptr;

  // Golden ratio hash for uniform stripe spreading
  uint32_t hash = thread_id * 2654435761U;
  size_t target_stripe = hash % _active_stripe_count;

  void* ptr = _stripes[target_stripe].allocate(bytes);
  if (ptr != nullptr) {
    size_t aligned = (bytes + 15) & ~static_cast<size_t>(15);
    add_used(aligned);
    _total_striped_allocations.fetch_add(1, std::memory_order_relaxed);
    _total_striped_bytes.fetch_add(aligned, std::memory_order_relaxed);
    return ptr;
  }

  // Target stripe exhausted: try round-robin fallback across remaining stripes
  return allocate_round_robin(bytes);
}

void* RedjetVFSNode_StripedRunway::allocate_round_robin(size_t bytes) {
  if (bytes == 0) return nullptr;

  size_t aligned = (bytes + 15) & ~static_cast<size_t>(15);
  uint32_t start_idx = _rr_index.fetch_add(1, std::memory_order_relaxed) % _active_stripe_count;

  for (size_t i = 0; i < _active_stripe_count; ++i) {
    size_t idx = (start_idx + i) % _active_stripe_count;
    void* ptr = _stripes[idx].allocate(bytes);
    if (ptr != nullptr) {
      add_used(aligned);
      _total_striped_allocations.fetch_add(1, std::memory_order_relaxed);
      _total_striped_bytes.fetch_add(aligned, std::memory_order_relaxed);
      return ptr;
    }
  }

  // All stripes exhausted: recycle and retry
  recycle_all_stripes();
  void* retry_ptr = _stripes[start_idx].allocate(bytes);
  if (retry_ptr != nullptr) {
    add_used(aligned);
    _total_striped_allocations.fetch_add(1, std::memory_order_relaxed);
    _total_striped_bytes.fetch_add(aligned, std::memory_order_relaxed);
    return retry_ptr;
  }

  return nullptr; // Out of memory in striped runway
}

void RedjetVFSNode_StripedRunway::recycle_all_stripes() {
  hs_ticket_guard_t guard(_striped_lock);
  _total_stripe_recycles.fetch_add(1, std::memory_order_relaxed);

  for (size_t i = 0; i < _active_stripe_count; ++i) {
    _stripes[i].reset();
  }
}

void RedjetVFSNode_StripedRunway::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet VFS Striped Runway Controller (/heap/on_heap/runway/striped):");
  st->print_cr("  Active NUMA Stripes:        " SIZE_FORMAT, _active_stripe_count);
  st->print_cr("  Total Allocations:          " UINT64_FORMAT, total_allocations());
  st->print_cr("  Total Bytes Allocated:      " UINT64_FORMAT, total_bytes());
  st->print_cr("  Total Stripe Recycles:      " UINT64_FORMAT, total_recycles());
  st->print_cr("  Current VFS Used:           " SIZE_FORMAT " bytes", used());
}
