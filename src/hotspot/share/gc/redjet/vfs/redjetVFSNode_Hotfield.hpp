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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_HOTFIELD_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_HOTFIELD_HPP

#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <atomic>
#include <cstdint>
#include <cstddef>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Hotfield VFS Node: Order of Heap Footprint (OOH) Size-Segregated Allocator
// (PLAN.md Section 8.1.1 & 9.1)
// 10 Segregated Slab Sizes: 16B, 32B, 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB, 8KB
// 64-Byte Cache-Line Spatial Packing (4x 16B / 2x 32B per line), Zero Fragmentation
// ============================================================================

struct alignas(64) RedjetHotfieldSlot {
  void*               obj_ptr{ nullptr };
  size_t              footprint_bytes{ 0 };
  uint32_t            height_tier{ 0 };
  uint32_t            access_heat{ 100 };
  uint64_t            last_access_tsc{ 0 };
  RedjetHotfieldSlot* next{ nullptr };
};

struct RedjetHotfieldHeightTier {
  size_t               min_bytes{ 0 };
  size_t               max_bytes{ 0 };
  RedjetHotfieldSlot*  head{ nullptr };
  std::atomic<size_t>  slot_count{ 0 };
  std::atomic<size_t>  total_bytes{ 0 };
  hs_ticket_lock_t     tier_lock;
};

// Fixed 64KB OOH Size-Segregated Slab
struct alignas(64) RedjetOOHSlab {
  static constexpr size_t SLAB_PAGE_SIZE = 64 * 1024; // 64 KB page
  static constexpr size_t BITMAP_WORDS = 1024;       // Up to 65536 slots

  uint8_t*                  memory_base{ nullptr };
  size_t                    slot_size{ 0 };
  size_t                    total_slots{ 0 };
  std::atomic<size_t>       free_slots{ 0 };
  std::atomic<uint64_t>     allocation_bitmap[BITMAP_WORDS]{ 0 };
  uint32_t                  class_index{ 0 };
  RedjetOOHSlab*            next_slab{ nullptr };
  RedjetOOHSlab*            prev_slab{ nullptr };
  hs_ticket_lock_t          slab_lock;

  void* allocate_slot() noexcept;
  bool  free_slot(void* ptr) noexcept;
};

struct RedjetOOHClass {
  size_t               slot_size{ 0 };
  size_t               slots_per_slab{ 0 };
  RedjetOOHSlab*       active_slab{ nullptr };
  RedjetOOHSlab*       full_slabs_head{ nullptr };
  std::atomic<size_t>  total_allocated_slots{ 0 };
  std::atomic<size_t>  total_live_bytes{ 0 };
  hs_ticket_lock_t     class_lock;
};

class RedjetVFSNode_Hotfield : public RedjetVFSNode {
public:
  static constexpr size_t NUM_HEIGHT_TIERS = 12;
  static constexpr size_t NUM_OOH_CLASSES = 10;
  static constexpr size_t OOH_SIZES[NUM_OOH_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
  };

private:
  RedjetHotfieldHeightTier _tiers[NUM_HEIGHT_TIERS];
  RedjetOOHClass           _ooh_classes[NUM_OOH_CLASSES];

  std::atomic<uint64_t>    _total_inserts{ 0 };
  std::atomic<uint64_t>    _total_removals{ 0 };
  std::atomic<uint64_t>    _total_heat_touches{ 0 };
  std::atomic<uint64_t>    _total_ooh_allocations{ 0 };
  std::atomic<uint64_t>    _total_ooh_frees{ 0 };

  size_t find_tier_for_size(size_t bytes) const noexcept;
  size_t find_ooh_class_for_size(size_t bytes) const noexcept;
  RedjetOOHSlab* allocate_ooh_slab_page(size_t class_idx);

public:
  RedjetVFSNode_Hotfield(const char* name = "hotfield",
                         const char* full_path = "/heap/on_heap/runway/motherline/egg/young/hotfield",
                         RedjetVFSNode* parent = nullptr,
                         size_t quota_bytes = 0);
  virtual ~RedjetVFSNode_Hotfield();

  // 1. OOH Size-Segregated Slab Allocator (16B, 32B, 64B... 8KB)
  RJ_API void* allocate_ooh(size_t requested_bytes, size_t* out_actual_size = nullptr);
  RJ_API bool  free_ooh(void* ptr, size_t footprint_bytes);

  // 2. Object insertion into sorted height tiers
  RJ_API bool insert_object(void* obj_ptr, size_t footprint, uint32_t initial_heat = 100);
  RJ_API bool remove_object(void* obj_ptr, size_t footprint);
  RJ_API void touch_object(void* obj_ptr, size_t footprint) noexcept;

  // 3. Rebalance, compaction and spatial packing
  RJ_API void rebalance_and_sort_tiers();
  RJ_API void* find_best_fit_slot(size_t needed_bytes, size_t* out_actual_size = nullptr);

  // Inspection & Telemetry
  size_t tier_count() const noexcept { return NUM_HEIGHT_TIERS; }
  void   get_tier_stats(size_t tier_idx, size_t* out_slots, size_t* out_bytes) const noexcept;
  void   get_ooh_stats(size_t class_idx, size_t* out_slots, size_t* out_bytes) const noexcept;

  uint64_t total_inserts() const noexcept { return _total_inserts.load(std::memory_order_relaxed); }
  uint64_t total_removals() const noexcept { return _total_removals.load(std::memory_order_relaxed); }
  uint64_t total_touches() const noexcept { return _total_heat_touches.load(std::memory_order_relaxed); }
  uint64_t total_ooh_allocations() const noexcept { return _total_ooh_allocations.load(std::memory_order_relaxed); }
  uint64_t total_ooh_frees() const noexcept { return _total_ooh_frees.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_HOTFIELD_HPP
