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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJET_CACHE_SORTER_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJET_CACHE_SORTER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include <algorithm>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// Macrosort & Microsort: 64-Byte Cache-Line Colocation Sorter (PLAN.md Section 8.1.1)
// - Spatial locality binding: Group related objects together into single 64B line
// - Sequential read alignment matching VAC SortOpt: Loads -> ALU -> Stores
// - 64-byte fence rule: Zero Split-Line Penalty (16B x4, 32B x2, 64B x1)
// - Cabin rule enforcement: MacroSort (room isolation) vs MicroSort (intra-room)
// ============================================================================

static constexpr size_t REDJET_CACHE_LINE_SIZE = 64;
static constexpr size_t REDJET_MAX_OBJECTS_PER_LINE = 4; // 16B * 4 = 64B
static constexpr size_t REDJET_CABIN_SLOTS = 256;

// Field / Memory Access Type for VAC SortOpt scheduling
enum class RedjetAccessType : uint8_t {
  ACCESS_LOAD  = 0, // Sequential memory reads
  ACCESS_ALU   = 1, // Computational / arithmetic operations
  ACCESS_STORE = 2  // Write-back memory stores
};

// Memory access operation descriptor for SortOpt scheduling
struct RedjetMemoryAccessOp {
  uint32_t         op_id{ 0 };
  uintptr_t        memory_offset{ 0 };
  size_t           access_size{ 0 };
  RedjetAccessType type{ RedjetAccessType::ACCESS_LOAD };
  uint32_t         execution_weight{ 1 };
  uint32_t         register_target{ 0 };
};

// Object descriptor for colocation and layout optimization
struct RedjetObjectDescriptor {
  uintptr_t  object_address{ 0 };
  size_t     size_bytes{ 0 };
  uint32_t   affinity_group_id{ 0 }; // Objects in same group colocate (e.g. Order & Customer)
  uint32_t   access_heat{ 100 };     // High heat -> colocate in hot line
  uint16_t   generation{ 0 };
  uint8_t    cabin_id{ 0 };
  uint8_t    slot_in_cabin{ 0 };
};

// Cache-line cabin representation for MacroSort (Room Isolation)
struct alignas(64) RedjetMacroSortCabin {
  static constexpr size_t CAPACITY_BYTES = 4096; // 4KB Page-level Cabin Room
  static constexpr size_t LINES_PER_CABIN = CAPACITY_BYTES / REDJET_CACHE_LINE_SIZE; // 64 lines

  uint8_t*                  cabin_base{ nullptr };
  uint32_t                  cabin_id{ 0 };
  uint32_t                  affinity_group{ 0 };
  std::atomic<size_t>       used_bytes{ 0 };
  std::atomic<uint32_t>     object_count{ 0 };
  std::atomic<uint64_t>     total_access_count{ 0 };

  inline bool has_space_for(size_t bytes) const noexcept {
    return (used_bytes.load(std::memory_order_relaxed) + bytes <= CAPACITY_BYTES);
  }

  inline void* allocate(size_t bytes) noexcept {
    // 16-byte alignment minimum to obey 64B fence
    size_t aligned = (bytes + 15) & ~static_cast<size_t>(15);
    size_t prev = used_bytes.fetch_add(aligned, std::memory_order_relaxed);
    if (prev + aligned <= CAPACITY_BYTES) {
      object_count.fetch_add(1, std::memory_order_relaxed);
      return cabin_base + prev;
    }
    used_bytes.fetch_sub(aligned, std::memory_order_relaxed);
    return nullptr;
  }

  inline void reset() noexcept {
    used_bytes.store(0, std::memory_order_relaxed);
    object_count.store(0, std::memory_order_relaxed);
    total_access_count.store(0, std::memory_order_relaxed);
  }
};

// 64-Byte Cache Line packing slot layout for MicroSort
struct alignas(64) RedjetMicroSortLine {
  uint8_t   line_data[REDJET_CACHE_LINE_SIZE];
  uint32_t  object_offsets[REDJET_MAX_OBJECTS_PER_LINE];
  uint16_t  object_sizes[REDJET_MAX_OBJECTS_PER_LINE];
  uint8_t   active_objects{ 0 };
  uint8_t   is_packed{ 0 };

  inline bool fits(size_t bytes) const noexcept {
    if (active_objects >= REDJET_MAX_OBJECTS_PER_LINE) return false;
    size_t current_used = 0;
    for (uint8_t i = 0; i < active_objects; ++i) {
      current_used += object_sizes[i];
    }
    return (current_used + bytes <= REDJET_CACHE_LINE_SIZE);
  }

  inline bool append(const void* src, size_t bytes) noexcept {
    if (!fits(bytes) || bytes == 0) return false;
    size_t current_used = 0;
    for (uint8_t i = 0; i < active_objects; ++i) {
      current_used += object_sizes[i];
    }
    // Check 64-byte fence rule: object cannot cross 64-byte boundary
    if (current_used + bytes > REDJET_CACHE_LINE_SIZE) return false;

    std::memcpy(line_data + current_used, src, bytes);
    object_offsets[active_objects] = static_cast<uint32_t>(current_used);
    object_sizes[active_objects] = static_cast<uint16_t>(bytes);
    active_objects++;
    if (current_used + bytes == REDJET_CACHE_LINE_SIZE) {
      is_packed = 1;
    }
    return true;
  }

  inline void clear() noexcept {
    std::memset(line_data, 0, REDJET_CACHE_LINE_SIZE);
    std::memset(object_offsets, 0, sizeof(object_offsets));
    std::memset(object_sizes, 0, sizeof(object_sizes));
    active_objects = 0;
    is_packed = 0;
  }
};

class RedjetCacheSorter : public CHeapObj<mtGC> {
public:
  static constexpr size_t MAX_CABINS = 64;
  static constexpr size_t MAX_TRACKED_DESCRIPTORS = 2048;

private:
  RedjetMacroSortCabin  _cabins[MAX_CABINS];
  size_t                _active_cabins{ 0 };
  hs_ticket_lock_t      _sorter_lock;

  // Telemetry counters
  std::atomic<uint64_t> _total_objects_sorted{ 0 };
  std::atomic<uint64_t> _total_lines_packed{ 0 };
  std::atomic<uint64_t> _total_colocations{ 0 };
  std::atomic<uint64_t> _split_penalties_prevented{ 0 };
  std::atomic<uint64_t> _vac_sortopt_optimizations{ 0 };

public:
  RedjetCacheSorter();
  ~RedjetCacheSorter();

  void init(void* heap_backing, size_t backing_bytes);
  void destroy();

  // =========================================================================
  // 1. MacroSort: Partitioning allocations into cache-line cabins (Room Isolation)
  // Isolates distinct lifetime objects and clusters identical affinity groups
  // =========================================================================
  RJ_API size_t macrosort_partition(RedjetObjectDescriptor* descriptors, size_t count);

  // =========================================================================
  // 2. MicroSort: Intra-cabin 64B cache line packing and alignment
  // Enforces 64-byte fence rule (Zero Split-Line Penalty: 16B x4, 32B x2, 64B x1)
  // =========================================================================
  RJ_API size_t microsort_pack_line(uint8_t* line_target,
                                    const RedjetObjectDescriptor* descriptors,
                                    size_t count,
                                    size_t* out_packed_count);

  // =========================================================================
  // 3. Spatial Locality Colocation: Order -> Customer binding
  // Packs parent and child objects together into a single 64-byte cache line
  // =========================================================================
  RJ_API bool colocate_spatial_pair(const void* parent_src, size_t parent_bytes,
                                    const void* child_src, size_t child_bytes,
                                    uint8_t* out_cache_line);

  // =========================================================================
  // 4. VAC SortOpt Engine: Sequential Read Alignment (Loads -> ALU -> Stores)
  // Reorders memory access ops to ensure continuous, ascending read streaming
  // =========================================================================
  RJ_API size_t vac_sortopt_schedule(RedjetMemoryAccessOp* ops, size_t count);

  // =========================================================================
  // 5. 64-Byte Fence Validator: Checks if any allocation violates 64B boundary
  // =========================================================================
  RJ_API static bool violates_64b_fence(uintptr_t addr, size_t size_bytes) noexcept {
    uintptr_t start_line = addr / REDJET_CACHE_LINE_SIZE;
    uintptr_t end_line = (addr + size_bytes - 1) / REDJET_CACHE_LINE_SIZE;
    return (start_line != end_line);
  }

  // Telemetry accessors
  uint64_t total_objects_sorted() const noexcept { return _total_objects_sorted.load(std::memory_order_relaxed); }
  uint64_t total_lines_packed() const noexcept { return _total_lines_packed.load(std::memory_order_relaxed); }
  uint64_t total_colocations() const noexcept { return _total_colocations.load(std::memory_order_relaxed); }
  uint64_t split_penalties_prevented() const noexcept { return _split_penalties_prevented.load(std::memory_order_relaxed); }
  uint64_t vac_sortopt_optimizations() const noexcept { return _vac_sortopt_optimizations.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJET_CACHE_SORTER_HPP
