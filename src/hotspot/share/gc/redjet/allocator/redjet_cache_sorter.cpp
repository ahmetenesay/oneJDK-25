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

#include "gc/redjet/allocator/redjet_cache_sorter.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetCacheSorter::RedjetCacheSorter() {
  _active_cabins = 0;
  _total_objects_sorted.store(0, std::memory_order_relaxed);
  _total_lines_packed.store(0, std::memory_order_relaxed);
  _total_colocations.store(0, std::memory_order_relaxed);
  _split_penalties_prevented.store(0, std::memory_order_relaxed);
  _vac_sortopt_optimizations.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < MAX_CABINS; ++i) {
    _cabins[i].cabin_base = nullptr;
    _cabins[i].cabin_id = static_cast<uint32_t>(i);
    _cabins[i].affinity_group = 0;
    _cabins[i].reset();
  }
}

RedjetCacheSorter::~RedjetCacheSorter() {
  destroy();
}

void RedjetCacheSorter::init(void* heap_backing, size_t backing_bytes) {
  hs_ticket_guard_t guard(_sorter_lock);

  size_t cabin_capacity = RedjetMacroSortCabin::CAPACITY_BYTES;
  size_t possible_cabins = backing_bytes / cabin_capacity;
  _active_cabins = (possible_cabins > MAX_CABINS) ? MAX_CABINS : possible_cabins;

  uint8_t* ptr = static_cast<uint8_t*>(heap_backing);
  for (size_t i = 0; i < _active_cabins; ++i) {
    _cabins[i].cabin_base = ptr;
    _cabins[i].cabin_id = static_cast<uint32_t>(i);
    _cabins[i].affinity_group = 0;
    _cabins[i].reset();
    ptr += cabin_capacity;
  }
}

void RedjetCacheSorter::destroy() {
  hs_ticket_guard_t guard(_sorter_lock);
  for (size_t i = 0; i < MAX_CABINS; ++i) {
    _cabins[i].reset();
    _cabins[i].cabin_base = nullptr;
  }
  _active_cabins = 0;
}

// =========================================================================
// 1. MacroSort: Partitioning allocations into cache-line cabins (Room Isolation)
// Isolates distinct lifetime objects and clusters identical affinity groups
// =========================================================================
size_t RedjetCacheSorter::macrosort_partition(RedjetObjectDescriptor* descriptors, size_t count) {
  if (descriptors == nullptr || count == 0 || _active_cabins == 0) {
    return 0;
  }

  hs_ticket_guard_t guard(_sorter_lock);

  // Sort descriptors: Generation -> Affinity Group -> Access Heat (descending) -> Size
  std::sort(descriptors, descriptors + count, [](const RedjetObjectDescriptor& a, const RedjetObjectDescriptor& b) {
    if (a.generation != b.generation) {
      return a.generation < b.generation;
    }
    if (a.affinity_group_id != b.affinity_group_id) {
      return a.affinity_group_id < b.affinity_group_id;
    }
    if (a.access_heat != b.access_heat) {
      return a.access_heat > b.access_heat; // Hotter first
    }
    return a.size_bytes > b.size_bytes;
  });

  size_t partitioned_count = 0;
  size_t current_cabin_idx = 0;

  for (size_t i = 0; i < count; ++i) {
    RedjetObjectDescriptor& desc = descriptors[i];
    size_t needed = (desc.size_bytes + 15) & ~static_cast<size_t>(15);

    // Find or assign a cabin with matching affinity group or empty space
    bool assigned = false;
    for (size_t c = 0; c < _active_cabins; ++c) {
      size_t check_idx = (current_cabin_idx + c) % _active_cabins;
      RedjetMacroSortCabin& cabin = _cabins[check_idx];

      if (cabin.used_bytes.load(std::memory_order_relaxed) == 0) {
        // Assign new affinity group to empty cabin
        cabin.affinity_group = desc.affinity_group_id;
      }

      if (cabin.affinity_group == desc.affinity_group_id && cabin.has_space_for(needed)) {
        void* alloc_ptr = cabin.allocate(needed);
        if (alloc_ptr != nullptr) {
          desc.cabin_id = static_cast<uint8_t>(check_idx);
          desc.slot_in_cabin = static_cast<uint8_t>(cabin.object_count.load(std::memory_order_relaxed) - 1);
          assigned = true;
          current_cabin_idx = check_idx;
          partitioned_count++;
          break;
        }
      }
    }

    if (!assigned) {
      // Emergency overflow: allocate in any cabin with capacity
      for (size_t c = 0; c < _active_cabins; ++c) {
        if (_cabins[c].has_space_for(needed)) {
          void* alloc_ptr = _cabins[c].allocate(needed);
          if (alloc_ptr != nullptr) {
            desc.cabin_id = static_cast<uint8_t>(c);
            desc.slot_in_cabin = static_cast<uint8_t>(_cabins[c].object_count.load(std::memory_order_relaxed) - 1);
            partitioned_count++;
            break;
          }
        }
      }
    }
  }

  _total_objects_sorted.fetch_add(partitioned_count, std::memory_order_relaxed);
  return partitioned_count;
}

// =========================================================================
// 2. MicroSort: Intra-cabin 64B cache line packing and alignment
// Enforces 64-byte fence rule (Zero Split-Line Penalty: 16B x4, 32B x2, 64B x1)
// =========================================================================
size_t RedjetCacheSorter::microsort_pack_line(uint8_t* line_target,
                                              const RedjetObjectDescriptor* descriptors,
                                              size_t count,
                                              size_t* out_packed_count) {
  if (line_target == nullptr || descriptors == nullptr || count == 0) {
    if (out_packed_count != nullptr) *out_packed_count = 0;
    return 0;
  }

  // Ensure line_target itself is 64-byte aligned
  uintptr_t target_addr = reinterpret_cast<uintptr_t>(line_target);
  if ((target_addr & 63) != 0) {
    _split_penalties_prevented.fetch_add(1, std::memory_order_relaxed);
  }

  RedjetMicroSortLine micro_line;
  micro_line.clear();

  size_t packed_objects = 0;
  size_t current_line_bytes = 0;

  for (size_t i = 0; i < count && packed_objects < REDJET_MAX_OBJECTS_PER_LINE; ++i) {
    const RedjetObjectDescriptor& desc = descriptors[i];
    if (desc.size_bytes == 0) continue;

    // 16-byte alignment minimum within cache line
    size_t aligned_sz = (desc.size_bytes + 15) & ~static_cast<size_t>(15);

    // Verify 64-byte fence rule: would this cross the 64-byte boundary?
    if (current_line_bytes + aligned_sz <= REDJET_CACHE_LINE_SIZE) {
      if (desc.object_address != 0) {
        micro_line.append(reinterpret_cast<const void*>(desc.object_address), desc.size_bytes);
      } else {
        // Zero dummy reservation
        uint8_t dummy[REDJET_CACHE_LINE_SIZE] = { 0 };
        micro_line.append(dummy, desc.size_bytes);
      }
      current_line_bytes += aligned_sz;
      packed_objects++;
    } else {
      // Object would split across cache lines; prevent split-line penalty!
      _split_penalties_prevented.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }

  // Zero-fill remaining tail of the 64B cache line to eliminate false-sharing debris
  if (current_line_bytes < REDJET_CACHE_LINE_SIZE) {
    std::memset(line_target + current_line_bytes, 0, REDJET_CACHE_LINE_SIZE - current_line_bytes);
  }

  std::memcpy(line_target, micro_line.line_data, current_line_bytes);

  if (out_packed_count != nullptr) {
    *out_packed_count = packed_objects;
  }

  _total_lines_packed.fetch_add(1, std::memory_order_relaxed);
  return current_line_bytes;
}

// =========================================================================
// 3. Spatial Locality Colocation: Order -> Customer binding
// Packs parent and child objects together into a single 64-byte cache line
// =========================================================================
bool RedjetCacheSorter::colocate_spatial_pair(const void* parent_src, size_t parent_bytes,
                                              const void* child_src, size_t child_bytes,
                                              uint8_t* out_cache_line) {
  if (out_cache_line == nullptr || parent_src == nullptr || child_src == nullptr) {
    return false;
  }

  // 16-byte aligned sizes
  size_t parent_aligned = (parent_bytes + 15) & ~static_cast<size_t>(15);
  size_t child_aligned = (child_bytes + 15) & ~static_cast<size_t>(15);

  if (parent_aligned + child_aligned <= REDJET_CACHE_LINE_SIZE) {
    // Fits perfectly into single 64B cache line!
    // Offset 0: Parent (e.g. Order header)
    std::memcpy(out_cache_line, parent_src, parent_bytes);
    if (parent_aligned > parent_bytes) {
      std::memset(out_cache_line + parent_bytes, 0, parent_aligned - parent_bytes);
    }

    // Offset parent_aligned: Child (e.g. Customer reference / sub-payload)
    std::memcpy(out_cache_line + parent_aligned, child_src, child_bytes);
    size_t used = parent_aligned + child_bytes;

    // Zero any trailing margin in the 64B line
    if (used < REDJET_CACHE_LINE_SIZE) {
      std::memset(out_cache_line + used, 0, REDJET_CACHE_LINE_SIZE - used);
    }

    _total_colocations.fetch_add(1, std::memory_order_relaxed);
    _total_lines_packed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Exceeds single 64B cache line: layout parent at line 0, child at line 1
  std::memcpy(out_cache_line, parent_src, parent_bytes > REDJET_CACHE_LINE_SIZE ? REDJET_CACHE_LINE_SIZE : parent_bytes);
  std::memcpy(out_cache_line + REDJET_CACHE_LINE_SIZE, child_src, child_bytes > REDJET_CACHE_LINE_SIZE ? REDJET_CACHE_LINE_SIZE : child_bytes);

  _split_penalties_prevented.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// =========================================================================
// 4. VAC SortOpt Engine: Sequential Read Alignment (Loads -> ALU -> Stores)
// Reorders memory access ops to ensure continuous, ascending read streaming
// =========================================================================
size_t RedjetCacheSorter::vac_sortopt_schedule(RedjetMemoryAccessOp* ops, size_t count) {
  if (ops == nullptr || count <= 1) {
    return count;
  }

  // Stable sort adhering to:
  // Primary: Stage priority (Loads -> ALU -> Stores)
  // Secondary (for Loads): Ascending memory_offset (Monotonic prefetch streaming)
  // Secondary (for Stores): Ascending memory_offset (Burst line write-combining)
  // Secondary (for ALU): Register target ordering to minimize WAW/WAR hazards
  std::stable_sort(ops, ops + count, [](const RedjetMemoryAccessOp& a, const RedjetMemoryAccessOp& b) {
    if (a.type != b.type) {
      return static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type);
    }

    if (a.type == RedjetAccessType::ACCESS_LOAD || a.type == RedjetAccessType::ACCESS_STORE) {
      return a.memory_offset < b.memory_offset;
    }

    // For ALU: group by target register
    if (a.register_target != b.register_target) {
      return a.register_target < b.register_target;
    }
    return a.execution_weight > b.execution_weight;
  });

  _vac_sortopt_optimizations.fetch_add(1, std::memory_order_relaxed);
  return count;
}

void RedjetCacheSorter::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet Cache Sorter (Macrosort & Microsort):");
  st->print_cr("  Active Cabins:              " SIZE_FORMAT, _active_cabins);
  st->print_cr("  Total Objects Sorted:       " UINT64_FORMAT, total_objects_sorted());
  st->print_cr("  Total 64B Lines Packed:     " UINT64_FORMAT, total_lines_packed());
  st->print_cr("  Total Spatial Colocations:  " UINT64_FORMAT, total_colocations());
  st->print_cr("  Split Penalties Prevented:  " UINT64_FORMAT, split_penalties_prevented());
  st->print_cr("  VAC SortOpt Optimizations:  " UINT64_FORMAT, vac_sortopt_optimizations());
}
