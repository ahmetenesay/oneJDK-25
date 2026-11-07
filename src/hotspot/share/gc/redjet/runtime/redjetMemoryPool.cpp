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

#include "gc/redjet/runtime/redjetMemoryPool.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include <algorithm>

// ============================================================================
// RedjetUnifiedHeapPool Implementation
// ============================================================================

RedjetUnifiedHeapPool::RedjetUnifiedHeapPool(RedjetHeap* heap)
  : CollectedMemoryPool("Redjet Unified Heap",
                        heap != nullptr ? heap->initial_capacity() : 128 * 1024 * 1024,
                        heap != nullptr ? heap->max_capacity() : 1024 * 1024 * 1024,
                        true),
    _heap(heap),
    _peak_used(0),
    _usage_queries(0),
    _warning_threshold(0),
    _critical_threshold(0),
    _threshold_crossings(0) {
}

size_t RedjetUnifiedHeapPool::committed_in_bytes() {
  return _heap != nullptr ? _heap->capacity() : 128 * 1024 * 1024;
}

size_t RedjetUnifiedHeapPool::used_in_bytes() {
  _usage_queries.fetch_add(1, std::memory_order_relaxed);
  size_t u = _heap != nullptr ? _heap->used() : 0;

  // Track peak
  size_t cur_peak = _peak_used.load(std::memory_order_relaxed);
  while (u > cur_peak && !_peak_used.compare_exchange_weak(cur_peak, u, std::memory_order_relaxed)) {
    // CAS spin
  }

  size_t warn = _warning_threshold.load(std::memory_order_relaxed);
  if (warn > 0 && u >= warn) {
    _threshold_crossings.fetch_add(1, std::memory_order_relaxed);
  }

  return u;
}

size_t RedjetUnifiedHeapPool::max_size() const {
  return _heap != nullptr ? _heap->max_capacity() : 1024 * 1024 * 1024;
}

MemoryUsage RedjetUnifiedHeapPool::get_memory_usage() {
  size_t init_sz = _heap != nullptr ? _heap->initial_capacity() : 128 * 1024 * 1024;
  size_t used_sz = used_in_bytes();
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, used_sz, comm_sz, max_sz);
}

MemoryUsage RedjetUnifiedHeapPool::get_peak_memory_usage() {
  size_t init_sz = _heap != nullptr ? _heap->initial_capacity() : 128 * 1024 * 1024;
  size_t peak_sz = _peak_used.load(std::memory_order_relaxed);
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, peak_sz, comm_sz, max_sz);
}

void RedjetUnifiedHeapPool::reset_peak_memory_usage() {
  _peak_used.store(used_in_bytes(), std::memory_order_relaxed);
}

// ============================================================================
// RedjetRunwayPool Implementation
// ============================================================================

RedjetRunwayPool::RedjetRunwayPool(RedjetHeap* heap)
  : CollectedMemoryPool("Redjet Runway (Young)",
                        heap != nullptr ? heap->initial_capacity() / 4 : 32 * 1024 * 1024,
                        heap != nullptr ? heap->max_capacity() / 2 : 512 * 1024 * 1024,
                        true),
    _heap(heap),
    _peak_used(0),
    _usage_queries(0),
    _warning_threshold(0),
    _critical_threshold(0),
    _threshold_crossings(0) {
}

size_t RedjetRunwayPool::committed_in_bytes() {
  return _heap != nullptr ? (_heap->capacity() / 3) : 32 * 1024 * 1024;
}

size_t RedjetRunwayPool::used_in_bytes() {
  _usage_queries.fetch_add(1, std::memory_order_relaxed);
  size_t u = _heap != nullptr ? (_heap->used() / 3) : 0;

  size_t cur_peak = _peak_used.load(std::memory_order_relaxed);
  while (u > cur_peak && !_peak_used.compare_exchange_weak(cur_peak, u, std::memory_order_relaxed)) {
  }

  size_t warn = _warning_threshold.load(std::memory_order_relaxed);
  if (warn > 0 && u >= warn) {
    _threshold_crossings.fetch_add(1, std::memory_order_relaxed);
  }

  return u;
}

size_t RedjetRunwayPool::max_size() const {
  return _heap != nullptr ? (_heap->max_capacity() / 2) : 512 * 1024 * 1024;
}

MemoryUsage RedjetRunwayPool::get_memory_usage() {
  size_t init_sz = _heap != nullptr ? (_heap->initial_capacity() / 4) : 32 * 1024 * 1024;
  size_t used_sz = used_in_bytes();
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, used_sz, comm_sz, max_sz);
}

MemoryUsage RedjetRunwayPool::get_peak_memory_usage() {
  size_t init_sz = _heap != nullptr ? (_heap->initial_capacity() / 4) : 32 * 1024 * 1024;
  size_t peak_sz = _peak_used.load(std::memory_order_relaxed);
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, peak_sz, comm_sz, max_sz);
}

void RedjetRunwayPool::reset_peak_memory_usage() {
  _peak_used.store(used_in_bytes(), std::memory_order_relaxed);
}

// ============================================================================
// RedjetHotfieldPool Implementation
// ============================================================================

RedjetHotfieldPool::RedjetHotfieldPool(RedjetHeap* heap)
  : CollectedMemoryPool("Redjet Hotfield (Survivor)",
                        heap != nullptr ? heap->initial_capacity() / 4 : 32 * 1024 * 1024,
                        heap != nullptr ? heap->max_capacity() / 2 : 512 * 1024 * 1024,
                        true),
    _heap(heap),
    _peak_used(0),
    _usage_queries(0),
    _warning_threshold(0),
    _critical_threshold(0),
    _threshold_crossings(0) {
}

size_t RedjetHotfieldPool::committed_in_bytes() {
  return _heap != nullptr ? (_heap->capacity() / 4) : 32 * 1024 * 1024;
}

size_t RedjetHotfieldPool::used_in_bytes() {
  _usage_queries.fetch_add(1, std::memory_order_relaxed);
  size_t u = _heap != nullptr ? (_heap->used() / 4) : 0;

  size_t cur_peak = _peak_used.load(std::memory_order_relaxed);
  while (u > cur_peak && !_peak_used.compare_exchange_weak(cur_peak, u, std::memory_order_relaxed)) {
  }

  size_t warn = _warning_threshold.load(std::memory_order_relaxed);
  if (warn > 0 && u >= warn) {
    _threshold_crossings.fetch_add(1, std::memory_order_relaxed);
  }

  return u;
}

size_t RedjetHotfieldPool::max_size() const {
  return _heap != nullptr ? (_heap->max_capacity() / 2) : 512 * 1024 * 1024;
}

MemoryUsage RedjetHotfieldPool::get_memory_usage() {
  size_t init_sz = _heap != nullptr ? (_heap->initial_capacity() / 4) : 32 * 1024 * 1024;
  size_t used_sz = used_in_bytes();
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, used_sz, comm_sz, max_sz);
}

MemoryUsage RedjetHotfieldPool::get_peak_memory_usage() {
  size_t init_sz = _heap != nullptr ? (_heap->initial_capacity() / 4) : 32 * 1024 * 1024;
  size_t peak_sz = _peak_used.load(std::memory_order_relaxed);
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, peak_sz, comm_sz, max_sz);
}

void RedjetHotfieldPool::reset_peak_memory_usage() {
  _peak_used.store(used_in_bytes(), std::memory_order_relaxed);
}

// ============================================================================
// RedjetLandfillPool Implementation
// ============================================================================

RedjetLandfillPool::RedjetLandfillPool(RedjetHeap* heap)
  : CollectedMemoryPool("Redjet Landfill (Tenured)",
                        heap != nullptr ? heap->initial_capacity() / 2 : 64 * 1024 * 1024,
                        heap != nullptr ? heap->max_capacity() : 1024 * 1024 * 1024,
                        true),
    _heap(heap),
    _peak_used(0),
    _usage_queries(0),
    _warning_threshold(0),
    _critical_threshold(0),
    _threshold_crossings(0) {
}

size_t RedjetLandfillPool::committed_in_bytes() {
  return _heap != nullptr ? (_heap->capacity() * 5 / 12) : 64 * 1024 * 1024;
}

size_t RedjetLandfillPool::used_in_bytes() {
  _usage_queries.fetch_add(1, std::memory_order_relaxed);
  size_t u = _heap != nullptr ? (_heap->used() * 5 / 12) : 0;

  size_t cur_peak = _peak_used.load(std::memory_order_relaxed);
  while (u > cur_peak && !_peak_used.compare_exchange_weak(cur_peak, u, std::memory_order_relaxed)) {
  }

  size_t warn = _warning_threshold.load(std::memory_order_relaxed);
  if (warn > 0 && u >= warn) {
    _threshold_crossings.fetch_add(1, std::memory_order_relaxed);
  }

  return u;
}

size_t RedjetLandfillPool::max_size() const {
  return _heap != nullptr ? _heap->max_capacity() : 1024 * 1024 * 1024;
}

MemoryUsage RedjetLandfillPool::get_memory_usage() {
  size_t init_sz = _heap != nullptr ? (_heap->initial_capacity() / 2) : 64 * 1024 * 1024;
  size_t used_sz = used_in_bytes();
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, used_sz, comm_sz, max_sz);
}

MemoryUsage RedjetLandfillPool::get_peak_memory_usage() {
  size_t init_sz = _heap != nullptr ? (_heap->initial_capacity() / 2) : 64 * 1024 * 1024;
  size_t peak_sz = _peak_used.load(std::memory_order_relaxed);
  size_t comm_sz = committed_in_bytes();
  size_t max_sz  = max_size();
  return MemoryUsage(init_sz, peak_sz, comm_sz, max_sz);
}

void RedjetLandfillPool::reset_peak_memory_usage() {
  _peak_used.store(used_in_bytes(), std::memory_order_relaxed);
}
