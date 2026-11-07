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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_CCDRUNWAY_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_CCDRUNWAY_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
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
// CCD Runway Controller (/heap/on_heap/runway/ccd_runways) (PLAN.md Section 9.1 & 8.4)
// - Dedicated per-CCD local bump allocation runways for multi-die CPUs (AMD Threadripper/EPYC)
// - Binds allocation to local L3 cache slices, eliminating inter-die fabric traffic
// - Automatic cross-CCD load rebalancing and telemetry
// ============================================================================

static constexpr size_t REDJET_MAX_CCDS = 16;
static constexpr size_t REDJET_CCD_SLAB_SIZE = 64 * 1024; // 64 KB per local CCD bump slab

struct alignas(64) RedjetCCDSlab {
  uint8_t*                  slab_base{ nullptr };
  std::atomic<uint8_t*>     bump_top{ nullptr };
  uint8_t*                  slab_end{ nullptr };
  uint32_t                  ccd_id{ 0 };
  uint32_t                  numa_node{ 0 };
  std::atomic<size_t>       allocated_bytes{ 0 };
  std::atomic<size_t>       allocation_count{ 0 };

  inline void* allocate(size_t bytes) noexcept {
    size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);
    uint8_t* cur = bump_top.load(std::memory_order_relaxed);
    while (cur + aligned <= slab_end) {
      if (bump_top.compare_exchange_weak(cur, cur + aligned,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        allocated_bytes.fetch_add(aligned, std::memory_order_relaxed);
        allocation_count.fetch_add(1, std::memory_order_relaxed);
        return cur;
      }
    }
    return nullptr;
  }

  inline void reset() noexcept {
    bump_top.store(slab_base, std::memory_order_relaxed);
    allocated_bytes.store(0, std::memory_order_relaxed);
    allocation_count.store(0, std::memory_order_relaxed);
  }
};

class RedjetVFSNode_CCDRunway : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_QUOTA = 1024ULL * 1024 * 1024; // 1 GB default quota

private:
  RedjetCCDSlab         _ccd_slabs[REDJET_MAX_CCDS];
  size_t                _active_ccd_count{ 1 };
  uint8_t*              _backing_storage{ nullptr };
  size_t                _backing_capacity{ 0 };
  hs_ticket_lock_t      _ccd_lock;

  // Global telemetry
  std::atomic<uint64_t> _total_ccd_allocations{ 0 };
  std::atomic<uint64_t> _total_ccd_bytes_allocated{ 0 };
  std::atomic<uint64_t> _cross_ccd_overflows{ 0 };
  std::atomic<uint64_t> _ccd_rebalance_events{ 0 };

public:
  RedjetVFSNode_CCDRunway(const char* name = "ccd_runways",
                          const char* full_path = "/heap/on_heap/runway/ccd_runways",
                          RedjetVFSNode* parent = nullptr,
                          size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_CCDRunway();

  void init(void* backing_memory, size_t capacity_bytes, size_t detected_ccds);
  void destroy();

  // Fast allocation pinned to thread's local CCD
  RJ_API void* allocate_on_ccd(uint32_t ccd_id, size_t bytes);

  // Cross-CCD overflow fallback allocation
  RJ_API void* allocate_cross_ccd_fallback(uint32_t preferred_ccd, size_t bytes);

  // Rebalance: flushes full slabs and resets bump pointers
  RJ_API void rebalance_all_ccds();

  // Telemetry
  size_t   active_ccds() const noexcept { return _active_ccd_count; }
  uint64_t total_allocations() const noexcept { return _total_ccd_allocations.load(std::memory_order_relaxed); }
  uint64_t total_bytes() const noexcept { return _total_ccd_bytes_allocated.load(std::memory_order_relaxed); }
  uint64_t cross_ccd_overflows() const noexcept { return _cross_ccd_overflows.load(std::memory_order_relaxed); }
  uint64_t rebalance_events() const noexcept { return _ccd_rebalance_events.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_CCDRUNWAY_HPP
