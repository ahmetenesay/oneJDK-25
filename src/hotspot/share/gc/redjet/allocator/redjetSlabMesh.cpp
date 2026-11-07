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

#include "gc/redjet/allocator/redjetSlabMesh.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

// Fast bit scan forward (CTZ)
static inline uint32_t redjet_ctz64(uint64_t val) {
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanForward64(&index, val);
  return static_cast<uint32_t>(index);
#elif defined(__GNUC__) || defined(__clang__)
  return static_cast<uint32_t>(__builtin_ctzll(val));
#else
  uint32_t count = 0;
  while ((val & 1) == 0 && count < 64) {
    val >>= 1;
    count++;
  }
  return count;
#endif
}

// ============================================================================
// RedjetMeshSlab Implementation
// ============================================================================

void RedjetMeshSlab::init(void* base, size_t size, RedjetSlabClass sc, uint32_t ccd, uint32_t thread_id) {
  slab_base = base;
  slab_size = size;
  slab_class = sc;
  slot_size_bytes = redjet_slab_class_to_size(sc);
  owning_ccd_id = ccd;
  owning_thread_id = thread_id;
  is_lent = false;
  last_loan_tsc = 0;
  next = nullptr;

  size_t computed_slots = size / slot_size_bytes;
  size_t max_bitmap_slots = BITMAP_QWORDS * 64;
  total_slots = static_cast<uint32_t>(std::min(computed_slots, max_bitmap_slots));
  free_slots = total_slots;

  // Initialize bitmap: set bits 0..(total_slots - 1) to 1 (all free)
  uint32_t remaining = total_slots;
  for (size_t q = 0; q < BITMAP_QWORDS; ++q) {
    if (remaining >= 64) {
      free_bitmap[q].store(~0ULL, std::memory_order_relaxed);
      remaining -= 64;
    } else if (remaining > 0) {
      uint64_t mask = (1ULL << remaining) - 1ULL;
      free_bitmap[q].store(mask, std::memory_order_relaxed);
      remaining = 0;
    } else {
      free_bitmap[q].store(0, std::memory_order_relaxed);
    }
  }
}

void* RedjetMeshSlab::allocate_slot() {
  if (free_slots == 0) return nullptr;

  for (size_t q = 0; q < BITMAP_QWORDS; ++q) {
    uint64_t current = free_bitmap[q].load(std::memory_order_relaxed);
    while (current != 0) {
      uint32_t bit = redjet_ctz64(current);
      uint64_t mask = 1ULL << bit;
      // Attempt lockless CAS to claim slot bit
      if (free_bitmap[q].compare_exchange_weak(current, current & ~mask,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
        if (free_slots > 0) free_slots--;
        uint32_t slot_idx = static_cast<uint32_t>(q * 64 + bit);
        uint8_t* base_ptr = reinterpret_cast<uint8_t*>(slab_base);
        return base_ptr + (slot_idx * slot_size_bytes);
      }
      // CAS failed, re-read current in loop
    }
  }
  return nullptr; // No slot available
}

bool RedjetMeshSlab::free_slot(void* ptr) {
  if (ptr == nullptr || ptr < slab_base) return false;

  uint8_t* base_ptr = reinterpret_cast<uint8_t*>(slab_base);
  uint8_t* slot_ptr = reinterpret_cast<uint8_t*>(ptr);
  size_t byte_offset = static_cast<size_t>(slot_ptr - base_ptr);

  if (byte_offset >= slab_size) return false;

  uint32_t slot_idx = static_cast<uint32_t>(byte_offset / slot_size_bytes);
  if (slot_idx >= total_slots) return false;

  size_t q = slot_idx / 64;
  uint32_t bit = slot_idx % 64;
  uint64_t mask = 1ULL << bit;

  // Atomic bit set back to 1
  uint64_t prev = free_bitmap[q].fetch_or(mask, std::memory_order_acq_rel);
  if ((prev & mask) == 0) {
    if (free_slots < total_slots) free_slots++;
    return true;
  }
  return false; // Slot was already marked free (double free avoided)
}

// ============================================================================
// RedjetSlabMesh Implementation
// ============================================================================

RedjetSlabMesh* RedjetSlabMesh::_instance = nullptr;
std::atomic<bool> RedjetSlabMesh::_initialized{ false };

RedjetSlabMesh::RedjetSlabMesh()
  : _total_slabs_registered(0),
    _total_slabs_lent(0),
    _total_slabs_reclaimed(0),
    _total_intra_ccd_loans(0),
    _total_inter_ccd_loans(0),
    _total_remote_steals_blocked(0),
    _total_slots_allocated(0),
    _total_slots_freed(0) {
  for (size_t c = 0; c < MAX_CCDS_SUPPORTED; ++c) {
    hs_ticket_lock_init(&_mesh_locks[c]);
    for (size_t s = 0; s < static_cast<size_t>(RedjetSlabClass::CLASS_COUNT); ++s) {
      _mesh_table[c][s] = nullptr;
    }
  }
  hs_ticket_lock_init(&_registry_lock);
  _registered_count.store(0, std::memory_order_relaxed);
  init_distance_matrix();
}

RedjetSlabMesh::~RedjetSlabMesh() {
}

void RedjetSlabMesh::init_distance_matrix() {
  // NUMA distance cost matrix:
  // Intra-CCD latency: 10 ns (shared L3 cache)
  // Intra-Socket adjacent CCD latency: 35 ns (on-die interconnect / Infinity Fabric)
  // Remote Socket cross-NUMA latency: 85 ns (cross-socket UPI / external fabric)
  for (size_t i = 0; i < MAX_CCDS_SUPPORTED; ++i) {
    for (size_t j = 0; j < MAX_CCDS_SUPPORTED; ++j) {
      if (i == j) {
        _numa_distance_matrix[i][j] = 10;
      } else if ((i / 4) == (j / 4)) {
        // Same NUMA domain (4 CCDs per NUMA node)
        _numa_distance_matrix[i][j] = 35;
      } else {
        // Cross-NUMA / Cross-Socket
        _numa_distance_matrix[i][j] = 85;
      }
    }
  }
}

void RedjetSlabMesh::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetSlabMesh();
  }
}

void RedjetSlabMesh::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetSlabMesh::register_slab(RedjetMeshSlab* slab, uint32_t ccd_id, RedjetSlabClass sclass) {
  if (slab == nullptr || ccd_id >= MAX_CCDS_SUPPORTED) return;

  size_t class_idx = static_cast<size_t>(sclass);
  if (class_idx >= static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)) return;

  hs_ticket_lock_acquire(&_mesh_locks[ccd_id]);

  slab->owning_ccd_id = ccd_id;
  slab->slab_class = sclass;
  slab->is_lent = false;
  slab->next = _mesh_table[ccd_id][class_idx];
  _mesh_table[ccd_id][class_idx] = slab;

  _total_slabs_registered.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_mesh_locks[ccd_id]);

  // Track in global registry
  hs_ticket_lock_acquire(&_registry_lock);
  size_t count = _registered_count.load(std::memory_order_relaxed);
  if (count < MAX_SLABS_PER_TIER) {
    _registered_slabs[count] = slab;
    _registered_count.store(count + 1, std::memory_order_release);
  }
  hs_ticket_lock_release(&_registry_lock);
}

RedjetMeshSlab* RedjetSlabMesh::borrow_slab(uint32_t requesting_ccd, RedjetSlabClass sclass) {
  if (requesting_ccd >= MAX_CCDS_SUPPORTED) return nullptr;

  size_t class_idx = static_cast<size_t>(sclass);
  if (class_idx >= static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)) return nullptr;

  // 1. Try local CCD first (10ns latency)
  hs_ticket_lock_acquire(&_mesh_locks[requesting_ccd]);
  RedjetMeshSlab* local_slab = _mesh_table[requesting_ccd][class_idx];
  if (local_slab != nullptr && !local_slab->is_lent) {
    _mesh_table[requesting_ccd][class_idx] = local_slab->next;
    local_slab->next = nullptr;
    local_slab->is_lent = true;
    local_slab->last_loan_tsc = sacculus_rdtsc();
    _total_slabs_lent.fetch_add(1, std::memory_order_relaxed);
    _total_intra_ccd_loans.fetch_add(1, std::memory_order_relaxed);
    hs_ticket_lock_release(&_mesh_locks[requesting_ccd]);
    return local_slab;
  }
  hs_ticket_lock_release(&_mesh_locks[requesting_ccd]);

  // 2. Try adjacent CCDs within the same NUMA node (35ns latency, 4 CCDs per NUMA node)
  uint32_t numa_base = (requesting_ccd / 4) * 4;
  for (uint32_t c = numa_base; c < numa_base + 4 && c < MAX_CCDS_SUPPORTED; ++c) {
    if (c == requesting_ccd) continue;

    hs_ticket_lock_acquire(&_mesh_locks[c]);
    RedjetMeshSlab* peer_slab = _mesh_table[c][class_idx];
    if (peer_slab != nullptr && !peer_slab->is_lent) {
      _mesh_table[c][class_idx] = peer_slab->next;
      peer_slab->next = nullptr;
      peer_slab->is_lent = true;
      peer_slab->last_loan_tsc = sacculus_rdtsc();
      _total_slabs_lent.fetch_add(1, std::memory_order_relaxed);
      _total_inter_ccd_loans.fetch_add(1, std::memory_order_relaxed);
      hs_ticket_lock_release(&_mesh_locks[c]);
      return peer_slab;
    }
    hs_ticket_lock_release(&_mesh_locks[c]);
  }

  // 3. Block remote cross-NUMA / cross-socket steals (85ns penalty!)
  _total_remote_steals_blocked.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void RedjetSlabMesh::return_slab(RedjetMeshSlab* slab) {
  if (slab == nullptr) return;

  uint32_t ccd_id = slab->owning_ccd_id;
  if (ccd_id >= MAX_CCDS_SUPPORTED) return;

  size_t class_idx = static_cast<size_t>(slab->slab_class);
  if (class_idx >= static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)) return;

  hs_ticket_lock_acquire(&_mesh_locks[ccd_id]);

  slab->is_lent = false;
  slab->next = _mesh_table[ccd_id][class_idx];
  _mesh_table[ccd_id][class_idx] = slab;

  _total_slabs_reclaimed.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_mesh_locks[ccd_id]);
}

void* RedjetSlabMesh::allocate_from_tlsc(RedjetThreadLocalSlabCache* tlsc, RedjetSlabClass sclass) {
  if (tlsc == nullptr) return nullptr;

  size_t class_idx = static_cast<size_t>(sclass);
  if (class_idx >= static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)) return nullptr;

  RedjetMeshSlab* active = tlsc->active_slabs[class_idx];

  // Fast path: allocate from current cached slab
  if (active != nullptr && !active->is_full()) {
    void* ptr = active->allocate_slot();
    if (ptr != nullptr) {
      tlsc->fast_path_allocations++;
      _total_slots_allocated.fetch_add(1, std::memory_order_relaxed);
      return ptr;
    }
  }

  // Slow path: slab exhausted or null, borrow from mesh
  if (active != nullptr && active->is_full()) {
    tlsc->slabs_exhausted++;
    return_slab(active);
    tlsc->active_slabs[class_idx] = nullptr;
  }

  RedjetMeshSlab* borrowed = borrow_slab(tlsc->ccd_id, sclass);
  if (borrowed != nullptr) {
    tlsc->slow_path_borrows++;
    tlsc->active_slabs[class_idx] = borrowed;
    void* ptr = borrowed->allocate_slot();
    if (ptr != nullptr) {
      _total_slots_allocated.fetch_add(1, std::memory_order_relaxed);
      return ptr;
    }
  }

  return nullptr; // Mesh exhausted for this class
}

bool RedjetSlabMesh::free_to_tlsc(RedjetThreadLocalSlabCache* tlsc, void* ptr, RedjetSlabClass sclass) {
  if (ptr == nullptr) return false;

  size_t class_idx = static_cast<size_t>(sclass);
  if (class_idx >= static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)) return false;

  if (tlsc != nullptr && tlsc->active_slabs[class_idx] != nullptr) {
    if (tlsc->active_slabs[class_idx]->free_slot(ptr)) {
      _total_slots_freed.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  // Fallback: search registered slabs
  hs_ticket_lock_acquire(&_registry_lock);
  size_t count = _registered_count.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    RedjetMeshSlab* s = _registered_slabs[i];
    if (s != nullptr && s->slab_class == sclass) {
      if (s->free_slot(ptr)) {
        _total_slots_freed.fetch_add(1, std::memory_order_relaxed);
        hs_ticket_lock_release(&_registry_lock);
        return true;
      }
    }
  }
  hs_ticket_lock_release(&_registry_lock);
  return false;
}

double RedjetSlabMesh::calculate_fragmentation_ratio() const noexcept {
  size_t cap = total_capacity_bytes();
  size_t used = total_used_bytes();
  if (cap == 0) return 0.0;
  double ratio = 1.0 - (static_cast<double>(used) / static_cast<double>(cap));
  return (ratio < 0.0) ? 0.0 : ((ratio > 1.0) ? 1.0 : ratio);
}

size_t RedjetSlabMesh::total_capacity_bytes() const noexcept {
  size_t count = _registered_count.load(std::memory_order_relaxed);
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    if (_registered_slabs[i] != nullptr) {
      total += _registered_slabs[i]->slab_size;
    }
  }
  return total;
}

size_t RedjetSlabMesh::total_used_bytes() const noexcept {
  size_t count = _registered_count.load(std::memory_order_relaxed);
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    RedjetMeshSlab* s = _registered_slabs[i];
    if (s != nullptr) {
      uint32_t occupied = s->total_slots - s->free_slots;
      used += (occupied * s->slot_size_bytes);
    }
  }
  return used;
}

void RedjetSlabMesh::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet Multi-Dimensional Slab Mesh Telemetry ===");
  st->print_cr("  Registered Slabs: " UINT64_FORMAT, slabs_registered());
  st->print_cr("  Total Slabs Lent: " UINT64_FORMAT, slabs_lent());
  st->print_cr("  Total Slabs Reclaimed: " UINT64_FORMAT, slabs_reclaimed());
  st->print_cr("  Intra-CCD Loans (10ns): " UINT64_FORMAT, intra_ccd_loans());
  st->print_cr("  Inter-CCD Loans (35ns): " UINT64_FORMAT, inter_ccd_loans());
  st->print_cr("  Remote Steals Blocked (85ns): " UINT64_FORMAT, remote_steals_blocked());
  st->print_cr("  Slots Allocated: " UINT64_FORMAT, slots_allocated());
  st->print_cr("  Slots Freed: " UINT64_FORMAT, slots_freed());
  st->print_cr("  Capacity: " SIZE_FORMAT " bytes, Used: " SIZE_FORMAT " bytes",
               total_capacity_bytes(), total_used_bytes());
  st->print_cr("  Fragmentation Ratio: %.2f %%", calculate_fragmentation_ratio() * 100.0);
}
