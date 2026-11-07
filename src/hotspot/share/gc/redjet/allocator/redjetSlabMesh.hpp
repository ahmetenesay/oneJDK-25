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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETSLABMESH_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETSLABMESH_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
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
// Redjet Multi-Dimensional Slab Mesh (PLAN.md Section 4.2, 8.4)
// - Interconnects Hotfield OOH size-segregated slabs across CCDs and NUMA nodes
// - Lock-free work-stealing and slab lending queue
// - Prevents thread starvation and memory fragmentation across NUMA domains
// - Zero remote-NUMA penalty routing: prioritizes intra-CCD loans
// - Micro-bitmap slot allocator with hardware trailing zero count (_tzcnt_u64)
// ============================================================================

enum class RedjetSlabClass : uint8_t {
  CLASS_16B  = 0,
  CLASS_32B  = 1,
  CLASS_64B  = 2,
  CLASS_128B = 3,
  CLASS_256B = 4,
  CLASS_512B = 5,
  CLASS_1KB  = 6,
  CLASS_2KB  = 7,
  CLASS_4KB  = 8,
  CLASS_8KB  = 9,
  CLASS_COUNT = 10
};

// Size helper mapping enum to byte size
inline size_t redjet_slab_class_to_size(RedjetSlabClass sc) noexcept {
  switch (sc) {
    case RedjetSlabClass::CLASS_16B:  return 16;
    case RedjetSlabClass::CLASS_32B:  return 32;
    case RedjetSlabClass::CLASS_64B:  return 64;
    case RedjetSlabClass::CLASS_128B: return 128;
    case RedjetSlabClass::CLASS_256B: return 256;
    case RedjetSlabClass::CLASS_512B: return 512;
    case RedjetSlabClass::CLASS_1KB:  return 1024;
    case RedjetSlabClass::CLASS_2KB:  return 2048;
    case RedjetSlabClass::CLASS_4KB:  return 4096;
    case RedjetSlabClass::CLASS_8KB:  return 8192;
    default: return 64;
  }
}

// 64-byte aligned Mesh Slab Header
struct alignas(64) RedjetMeshSlab {
  static constexpr size_t BITMAP_QWORDS = 8; // 8 * 64 = 512 slots capacity per slab

  void*            slab_base{ nullptr };
  size_t           slab_size{ 64 * 1024 }; // Standard 64KB slab
  RedjetSlabClass  slab_class{ RedjetSlabClass::CLASS_64B };
  size_t           slot_size_bytes{ 64 };
  uint32_t         owning_ccd_id{ 0 };
  uint32_t         owning_thread_id{ 0 };
  uint32_t         free_slots{ 0 };
  uint32_t         total_slots{ 0 };
  bool             is_lent{ false };
  uint64_t         last_loan_tsc{ 0 };
  RedjetMeshSlab*  next{ nullptr };

  // Lockless allocation bitmap: bit = 1 indicates slot is FREE
  alignas(64) std::atomic<uint64_t> free_bitmap[BITMAP_QWORDS];

  RedjetMeshSlab() {
    std::memset(this, 0, sizeof(*this));
  }

  void init(void* base, size_t size, RedjetSlabClass sc, uint32_t ccd, uint32_t thread_id);
  void* allocate_slot();
  bool  free_slot(void* ptr);
  bool  is_full() const noexcept { return free_slots == 0; }
  bool  is_empty() const noexcept { return free_slots == total_slots; }
};

// Thread-Local Slab Cache (TLSC) for lockless ultra-fast slot allocation
struct alignas(64) RedjetThreadLocalSlabCache {
  RedjetMeshSlab* active_slabs[static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)]{ nullptr };
  uint32_t        thread_id{ 0 };
  uint32_t        ccd_id{ 0 };
  uint64_t        fast_path_allocations{ 0 };
  uint64_t        slow_path_borrows{ 0 };
  uint64_t        slabs_exhausted{ 0 };

  RedjetThreadLocalSlabCache() {
    std::memset(this, 0, sizeof(*this));
  }
};

class RedjetSlabMesh {
public:
  static constexpr size_t MAX_CCDS_SUPPORTED = 32;
  static constexpr size_t MAX_NUMA_NODES     = 8;
  static constexpr size_t MAX_SLABS_PER_TIER = 1024;

private:
  static RedjetSlabMesh*  _instance;
  static std::atomic<bool> _initialized;

  // Multi-tier steal queue: [_ccd_id][_slab_class]
  RedjetMeshSlab*         _mesh_table[MAX_CCDS_SUPPORTED][static_cast<size_t>(RedjetSlabClass::CLASS_COUNT)];
  hs_ticket_lock_t        _mesh_locks[MAX_CCDS_SUPPORTED];

  // NUMA distance cost matrix (nanoseconds of latency)
  uint32_t                _numa_distance_matrix[MAX_CCDS_SUPPORTED][MAX_CCDS_SUPPORTED];

  // Global slab registry tracking
  RedjetMeshSlab*         _registered_slabs[MAX_SLABS_PER_TIER];
  std::atomic<size_t>     _registered_count{ 0 };
  hs_ticket_lock_t        _registry_lock;

  // Telemetry counters
  std::atomic<uint64_t>   _total_slabs_registered{ 0 };
  std::atomic<uint64_t>   _total_slabs_lent{ 0 };
  std::atomic<uint64_t>   _total_slabs_reclaimed{ 0 };
  std::atomic<uint64_t>   _total_intra_ccd_loans{ 0 };
  std::atomic<uint64_t>   _total_inter_ccd_loans{ 0 };
  std::atomic<uint64_t>   _total_remote_steals_blocked{ 0 };
  std::atomic<uint64_t>   _total_slots_allocated{ 0 };
  std::atomic<uint64_t>   _total_slots_freed{ 0 };

  void init_distance_matrix();

public:
  RedjetSlabMesh();
  ~RedjetSlabMesh();

  static void initialize();
  static void destroy();
  static RedjetSlabMesh* get() noexcept { return _instance; }

  // 1. Register Slab in Mesh
  RJ_API void register_slab(RedjetMeshSlab* slab, uint32_t ccd_id, RedjetSlabClass sclass);

  // 2. Lend / Borrow Slab (attempts intra-CCD first, then adjacent CCD within same NUMA)
  RJ_API RedjetMeshSlab* borrow_slab(uint32_t requesting_ccd, RedjetSlabClass sclass);

  // 3. Return borrowed slab back to owner
  RJ_API void return_slab(RedjetMeshSlab* slab);

  // 4. Thread-Local Allocation Interface
  RJ_API void* allocate_from_tlsc(RedjetThreadLocalSlabCache* tlsc, RedjetSlabClass sclass);
  RJ_API bool  free_to_tlsc(RedjetThreadLocalSlabCache* tlsc, void* ptr, RedjetSlabClass sclass);

  // 5. Memory Fragmentation & Footprint Evaluation
  // Returns fragmentation ratio Phi = 1.0 - (UsedBytes / TotalCapacityBytes)
  RJ_API double calculate_fragmentation_ratio() const noexcept;
  RJ_API size_t total_capacity_bytes() const noexcept;
  RJ_API size_t total_used_bytes() const noexcept;

  // Telemetry queries
  uint64_t slabs_registered()       const noexcept { return _total_slabs_registered.load(std::memory_order_relaxed); }
  uint64_t slabs_lent()             const noexcept { return _total_slabs_lent.load(std::memory_order_relaxed); }
  uint64_t slabs_reclaimed()        const noexcept { return _total_slabs_reclaimed.load(std::memory_order_relaxed); }
  uint64_t intra_ccd_loans()        const noexcept { return _total_intra_ccd_loans.load(std::memory_order_relaxed); }
  uint64_t inter_ccd_loans()        const noexcept { return _total_inter_ccd_loans.load(std::memory_order_relaxed); }
  uint64_t remote_steals_blocked()  const noexcept { return _total_remote_steals_blocked.load(std::memory_order_relaxed); }
  uint64_t slots_allocated()        const noexcept { return _total_slots_allocated.load(std::memory_order_relaxed); }
  uint64_t slots_freed()            const noexcept { return _total_slots_freed.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETSLABMESH_HPP
