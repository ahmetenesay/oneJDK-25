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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_RUNWAYEGG_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_RUNWAYEGG_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Runway Motherline Egg: 64KB Bump Allocator Node (PLAN.md Section 9.1 & 8.1.1)
// Ultra-low latency newborn allocation bay (0-2 ns bump pointer)
// Manages zones: Tank, Demand, Middle, Hotfield across Young/Adolescent/Senior
// ============================================================================

enum class RedjetEggZone : uint8_t {
  ZONE_TANK      = 0, // Bulk linear arrays / payload buffers
  ZONE_DEMAND    = 1, // High-throughput immediate temporary objects
  ZONE_MIDDLE    = 2, // Medium-lived generational staging objects
  ZONE_HOTFIELD  = 3  // Highly mutated cache-hot scalar clusters
};

enum class RedjetEggGeneration : uint8_t {
  GEN_YOUNG      = 0,
  GEN_ADOLESCENT = 1,
  GEN_SENIOR     = 2
};

struct alignas(64) RedjetEggSlabHeader {
  static constexpr size_t SLAB_CAPACITY_BYTES = 64 * 1024; // 64 KB Bump Slab

  uint8_t*                  slab_base{ nullptr };
  std::atomic<uint8_t*>     bump_top{ nullptr };
  uint8_t*                  slab_end{ nullptr };
  uint32_t                  slab_id{ 0 };
  uint32_t                  owner_thread_id{ 0 };
  RedjetEggZone             zone{ RedjetEggZone::ZONE_DEMAND };
  RedjetEggGeneration       generation{ RedjetEggGeneration::GEN_YOUNG };
  uint32_t                  allocation_epoch{ 1 };
  std::atomic<size_t>       allocated_bytes{ 0 };
  std::atomic<size_t>       reclaimed_bytes{ 0 };
  RedjetEggSlabHeader*      next_slab{ nullptr };
  RedjetEggSlabHeader*      prev_slab{ nullptr };

  inline bool has_space(size_t bytes) const noexcept {
    uint8_t* top = bump_top.load(std::memory_order_relaxed);
    return (top + bytes <= slab_end);
  }

  inline void* allocate(size_t bytes) noexcept {
    // 8-byte alignment enforcement
    size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);
    uint8_t* cur = bump_top.load(std::memory_order_relaxed);
    while (cur + aligned <= slab_end) {
      if (bump_top.compare_exchange_weak(cur, cur + aligned,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        allocated_bytes.fetch_add(aligned, std::memory_order_relaxed);
        return cur;
      }
    }
    return nullptr; // Slab full
  }

  inline void reset() noexcept {
    bump_top.store(slab_base, std::memory_order_relaxed);
    allocated_bytes.store(0, std::memory_order_relaxed);
    reclaimed_bytes.store(0, std::memory_order_relaxed);
  }
};

struct RedjetThreadEggTLAB {
  RedjetEggSlabHeader* current_slab{ nullptr };
  uint8_t*             thread_bump_ptr{ nullptr };
  uint8_t*             thread_end_ptr{ nullptr };
  size_t               thread_allocated_bytes{ 0 };
};

class RedjetVFSNode_RunwayEgg : public RedjetVFSNode {
public:
  static constexpr size_t MAX_TRACKED_SLABS = 16384;
  static constexpr size_t MAX_CACHED_THREADS = 1024;
  static constexpr size_t DEFAULT_QUOTA = 512ULL * 1024 * 1024; // 512 MB

private:
  RedjetEggSlabHeader* _slabs[MAX_TRACKED_SLABS];
  std::atomic<size_t>  _slab_count{ 0 };
  hs_ticket_lock_t     _slab_lock;

  RedjetEggSlabHeader* _free_slab_head{ nullptr };
  hs_ticket_lock_t     _free_lock;

  RedjetThreadEggTLAB  _thread_tlabs[MAX_CACHED_THREADS];
  hs_ticket_lock_t     _tlab_lock;

  alignas(64) std::atomic<uint64_t> _current_epoch{ 1 };
  std::atomic<uint64_t>             _total_allocations{ 0 };
  std::atomic<uint64_t>             _total_bytes_allocated{ 0 };
  std::atomic<uint64_t>             _total_slabs_recycled{ 0 };
  std::atomic<uint64_t>             _total_promotions_to_fledgling{ 0 };

  RedjetEggSlabHeader* allocate_new_slab(RedjetEggZone zone, RedjetEggGeneration gen, uint32_t tid);
  void free_slab_internal(RedjetEggSlabHeader* slab);

public:
  RedjetVFSNode_RunwayEgg(const char* name = "egg",
                          const char* full_path = "/heap/on_heap/runway/motherline/egg",
                          RedjetVFSNode* parent = nullptr,
                          size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_RunwayEgg();

  // 1. Fast Thread-Local Bump Allocation (0-2 ns, lockless fast path)
  RJ_API void* allocate_fast(size_t bytes, uint32_t thread_id,
                             RedjetEggZone zone = RedjetEggZone::ZONE_DEMAND,
                             RedjetEggGeneration gen = RedjetEggGeneration::GEN_YOUNG);

  // 2. Slow path TLAB slab replenishment
  RJ_API void* refill_and_allocate(size_t bytes, uint32_t thread_id,
                                   RedjetEggZone zone, RedjetEggGeneration gen);

  // 3. Epoch rollover: recycles empty slabs or advances surviving slabs
  RJ_API size_t advance_epoch_and_recycle();

  // 4. Sizing & Telemetry
  size_t   total_slabs() const noexcept { return _slab_count.load(std::memory_order_relaxed); }
  uint64_t current_epoch() const noexcept { return _current_epoch.load(std::memory_order_relaxed); }
  uint64_t total_allocations() const noexcept { return _total_allocations.load(std::memory_order_relaxed); }
  uint64_t total_bytes_allocated() const noexcept { return _total_bytes_allocated.load(std::memory_order_relaxed); }
  uint64_t total_slabs_recycled() const noexcept { return _total_slabs_recycled.load(std::memory_order_relaxed); }
  uint64_t total_promotions() const noexcept { return _total_promotions_to_fledgling.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_RUNWAYEGG_HPP
