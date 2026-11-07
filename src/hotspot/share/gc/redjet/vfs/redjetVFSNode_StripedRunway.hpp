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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_STRIPEDRUNWAY_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_STRIPEDRUNWAY_HPP

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
// Striped Runway Controller (/heap/on_heap/runway/striped) (PLAN.md Section 9.1 & 8.4)
// - Striped interleaved memory slab allocator across NUMA nodes
// - 256KB page stripes interleaved across up to 8 NUMA memory channels
// - Thread hash routing eliminating lock and bus contention during massive concurrency
// ============================================================================

static constexpr size_t REDJET_MAX_NUMA_STRIPES = 8;
static constexpr size_t REDJET_STRIPE_PAGE_SIZE = 256 * 1024; // 256 KB Stripe Pages

struct alignas(64) RedjetStripeChannel {
  uint8_t*                  channel_base{ nullptr };
  std::atomic<uint8_t*>     bump_cursor{ nullptr };
  uint8_t*                  channel_limit{ nullptr };
  uint32_t                  stripe_id{ 0 };
  uint32_t                  numa_node_id{ 0 };
  std::atomic<size_t>       bytes_allocated{ 0 };
  std::atomic<size_t>       stripe_cycles{ 0 };

  inline void* allocate(size_t bytes) noexcept {
    size_t aligned = (bytes + 15) & ~static_cast<size_t>(15);
    uint8_t* cur = bump_cursor.load(std::memory_order_relaxed);
    while (cur + aligned <= channel_limit) {
      if (bump_cursor.compare_exchange_weak(cur, cur + aligned,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        bytes_allocated.fetch_add(aligned, std::memory_order_relaxed);
        return cur;
      }
    }
    return nullptr;
  }

  inline void reset() noexcept {
    bump_cursor.store(channel_base, std::memory_order_relaxed);
    bytes_allocated.store(0, std::memory_order_relaxed);
    stripe_cycles.fetch_add(1, std::memory_order_relaxed);
  }
};

class RedjetVFSNode_StripedRunway : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_QUOTA = 1024ULL * 1024 * 1024; // 1 GB default quota

private:
  RedjetStripeChannel   _stripes[REDJET_MAX_NUMA_STRIPES];
  size_t                _active_stripe_count{ 2 };
  std::atomic<uint32_t> _rr_index{ 0 };
  uint8_t*              _storage_pool{ nullptr };
  size_t                _pool_bytes{ 0 };
  hs_ticket_lock_t      _striped_lock;

  // Global telemetry
  std::atomic<uint64_t> _total_striped_allocations{ 0 };
  std::atomic<uint64_t> _total_striped_bytes{ 0 };
  std::atomic<uint64_t> _total_stripe_recycles{ 0 };

public:
  RedjetVFSNode_StripedRunway(const char* name = "striped",
                              const char* full_path = "/heap/on_heap/runway/striped",
                              RedjetVFSNode* parent = nullptr,
                              size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_StripedRunway();

  void init(void* storage, size_t storage_bytes, size_t numa_nodes);
  void destroy();

  // Hash-based striped allocation across NUMA memory channels
  RJ_API void* allocate_striped(uint32_t thread_id, size_t bytes);

  // Round-robin interleaved allocation
  RJ_API void* allocate_round_robin(size_t bytes);

  // Recycles all stripes
  RJ_API void recycle_all_stripes();

  // Telemetry accessors
  size_t   active_stripes() const noexcept { return _active_stripe_count; }
  uint64_t total_allocations() const noexcept { return _total_striped_allocations.load(std::memory_order_relaxed); }
  uint64_t total_bytes() const noexcept { return _total_striped_bytes.load(std::memory_order_relaxed); }
  uint64_t total_recycles() const noexcept { return _total_stripe_recycles.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_STRIPEDRUNWAY_HPP
