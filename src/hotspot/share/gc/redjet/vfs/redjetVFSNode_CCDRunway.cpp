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

#include "gc/redjet/vfs/redjetVFSNode_CCDRunway.hpp"
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

RedjetVFSNode_CCDRunway::RedjetVFSNode_CCDRunway(const char* name,
                                                 const char* full_path,
                                                 RedjetVFSNode* parent,
                                                 size_t quota_bytes)
  : RedjetVFSNode(name, full_path, parent, quota_bytes),
    _active_ccd_count(1),
    _backing_storage(nullptr),
    _backing_capacity(0),
    _total_ccd_allocations(0),
    _total_ccd_bytes_allocated(0),
    _cross_ccd_overflows(0),
    _ccd_rebalance_events(0) {

  for (size_t i = 0; i < REDJET_MAX_CCDS; ++i) {
    _ccd_slabs[i].slab_base = nullptr;
    _ccd_slabs[i].bump_top.store(nullptr, std::memory_order_relaxed);
    _ccd_slabs[i].slab_end = nullptr;
    _ccd_slabs[i].ccd_id = static_cast<uint32_t>(i);
    _ccd_slabs[i].numa_node = 0;
    _ccd_slabs[i].allocated_bytes.store(0, std::memory_order_relaxed);
    _ccd_slabs[i].allocation_count.store(0, std::memory_order_relaxed);
  }
}

RedjetVFSNode_CCDRunway::~RedjetVFSNode_CCDRunway() {
  destroy();
}

void RedjetVFSNode_CCDRunway::init(void* backing_memory, size_t capacity_bytes, size_t detected_ccds) {
  hs_ticket_guard_t guard(_ccd_lock);
  _backing_storage = static_cast<uint8_t*>(backing_memory);
  _backing_capacity = capacity_bytes;
  _active_ccd_count = (detected_ccds > 0 && detected_ccds <= REDJET_MAX_CCDS) ? detected_ccds : 1;

  if (_backing_storage != nullptr && _backing_capacity > 0) {
    size_t bytes_per_ccd = _backing_capacity / _active_ccd_count;
    uint8_t* ptr = _backing_storage;

    for (size_t i = 0; i < _active_ccd_count; ++i) {
      _ccd_slabs[i].slab_base = ptr;
      _ccd_slabs[i].bump_top.store(ptr, std::memory_order_relaxed);
      _ccd_slabs[i].slab_end = ptr + bytes_per_ccd;
      _ccd_slabs[i].ccd_id = static_cast<uint32_t>(i);
      _ccd_slabs[i].numa_node = static_cast<uint32_t>(i / 4); // 4 CCDs per NUMA node typical
      _ccd_slabs[i].reset();
      ptr += bytes_per_ccd;
    }
  }
}

void RedjetVFSNode_CCDRunway::destroy() {
  hs_ticket_guard_t guard(_ccd_lock);
  for (size_t i = 0; i < REDJET_MAX_CCDS; ++i) {
    _ccd_slabs[i].reset();
    _ccd_slabs[i].slab_base = nullptr;
    _ccd_slabs[i].bump_top.store(nullptr, std::memory_order_relaxed);
    _ccd_slabs[i].slab_end = nullptr;
  }
  _backing_storage = nullptr;
  _backing_capacity = 0;
}

// Fast allocation pinned to thread's local CCD
void* RedjetVFSNode_CCDRunway::allocate_on_ccd(uint32_t ccd_id, size_t bytes) {
  if (bytes == 0) return nullptr;

  size_t idx = ccd_id % _active_ccd_count;
  void* ptr = _ccd_slabs[idx].allocate(bytes);

  if (ptr != nullptr) {
    size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);
    add_used(aligned);
    _total_ccd_allocations.fetch_add(1, std::memory_order_relaxed);
    _total_ccd_bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
    return ptr;
  }

  // Local CCD slab full: cross-CCD overflow fallback
  return allocate_cross_ccd_fallback(ccd_id, bytes);
}

void* RedjetVFSNode_CCDRunway::allocate_cross_ccd_fallback(uint32_t preferred_ccd, size_t bytes) {
  _cross_ccd_overflows.fetch_add(1, std::memory_order_relaxed);

  size_t start_idx = preferred_ccd % _active_ccd_count;
  size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);

  // Scan adjacent CCDs for available capacity
  for (size_t offset = 1; offset < _active_ccd_count; ++offset) {
    size_t check_idx = (start_idx + offset) % _active_ccd_count;
    void* ptr = _ccd_slabs[check_idx].allocate(bytes);
    if (ptr != nullptr) {
      add_used(aligned);
      _total_ccd_allocations.fetch_add(1, std::memory_order_relaxed);
      _total_ccd_bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
      return ptr;
    }
  }

  // All CCD slabs full: trigger rebalance and try one more time
  rebalance_all_ccds();
  void* retry_ptr = _ccd_slabs[start_idx].allocate(bytes);
  if (retry_ptr != nullptr) {
    add_used(aligned);
    _total_ccd_allocations.fetch_add(1, std::memory_order_relaxed);
    _total_ccd_bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
    return retry_ptr;
  }

  return nullptr; // Out of CCD capacity
}

void RedjetVFSNode_CCDRunway::rebalance_all_ccds() {
  hs_ticket_guard_t guard(_ccd_lock);
  _ccd_rebalance_events.fetch_add(1, std::memory_order_relaxed);

  for (size_t i = 0; i < _active_ccd_count; ++i) {
    _ccd_slabs[i].reset();
  }
}

void RedjetVFSNode_CCDRunway::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet VFS CCD Runway Controller (/heap/on_heap/runway/ccd_runways):");
  st->print_cr("  Active CCDs:                " SIZE_FORMAT, _active_ccd_count);
  st->print_cr("  Total Allocations:          " UINT64_FORMAT, total_allocations());
  st->print_cr("  Total Bytes Allocated:      " UINT64_FORMAT, total_bytes());
  st->print_cr("  Cross-CCD Overflows:        " UINT64_FORMAT, cross_ccd_overflows());
  st->print_cr("  Rebalance Events:           " UINT64_FORMAT, rebalance_events());
  st->print_cr("  Current VFS Used:           " SIZE_FORMAT " bytes", used());
}
