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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_RUNWAYFLEDGLING_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_RUNWAYFLEDGLING_HPP

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
// Runway Motherline Fledgling: Compact Survivor Slab Node (PLAN.md Sec 9.1 & 8.1.1)
// Manages Dockpoint/Puzzler-verified surviving objects with high spatial density
// Features compaction passes, survival age tracking, and promotion to Hotfield
// ============================================================================

struct alignas(64) RedjetSurvivorEntry {
  void*                  oop_address{ nullptr };
  size_t                 object_size{ 0 };
  uint32_t               survival_age{ 1 };
  uint32_t               origin_zone{ 0 };
  uint64_t               admission_epoch{ 0 };
  uint64_t               last_access_tsc{ 0 };
  bool                   is_optimized{ false };
  bool                   marked_for_hotfield{ false };
  RedjetSurvivorEntry*   next{ nullptr };
};

struct alignas(64) RedjetFledglingSlab {
  static constexpr size_t FLEDGLING_SLAB_SIZE = 128 * 1024; // 128 KB Compact Slab

  uint8_t*               slab_base{ nullptr };
  uint8_t*               free_ptr{ nullptr };
  uint8_t*               slab_end{ nullptr };
  uint32_t               slab_id{ 0 };
  std::atomic<size_t>    live_bytes{ 0 };
  std::atomic<uint32_t>  survivor_count{ 0 };
  RedjetSurvivorEntry*   entry_head{ nullptr };
  hs_ticket_lock_t       slab_lock;
  RedjetFledglingSlab*   next{ nullptr };

  inline bool can_fit(size_t bytes) const noexcept {
    return (free_ptr + bytes <= slab_end);
  }

  inline void* append(size_t bytes) noexcept {
    size_t aligned = (bytes + 7) & ~static_cast<size_t>(7);
    if (free_ptr + aligned <= slab_end) {
      void* res = free_ptr;
      free_ptr += aligned;
      live_bytes.fetch_add(aligned, std::memory_order_relaxed);
      return res;
    }
    return nullptr;
  }
};

class RedjetVFSNode_RunwayFledgling : public RedjetVFSNode {
public:
  static constexpr size_t MAX_FLEDGLING_SLABS = 4096;
  static constexpr uint32_t PROMOTION_AGE_THRESHOLD = 3; // Promotes to Hotfield after 3 epochs
  static constexpr size_t DEFAULT_QUOTA = 512ULL * 1024 * 1024; // 512 MB

private:
  RedjetFledglingSlab* _slabs[MAX_FLEDGLING_SLABS];
  std::atomic<size_t>  _slab_count{ 0 };
  hs_ticket_lock_t     _fledgling_lock;

  alignas(64) std::atomic<uint64_t> _fledgling_epoch{ 1 };
  std::atomic<uint64_t>             _total_admissions{ 0 };
  std::atomic<uint64_t>             _total_promotions{ 0 };
  std::atomic<uint64_t>             _total_compactions{ 0 };
  std::atomic<uint64_t>             _bytes_compacted{ 0 };

  RedjetFledglingSlab* allocate_slab();

public:
  RedjetVFSNode_RunwayFledgling(const char* name = "fledgling",
                                const char* full_path = "/heap/on_heap/runway/motherline/fledgling",
                                RedjetVFSNode* parent = nullptr,
                                size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_RunwayFledgling();

  // 1. Admits a survivor object promoted from Egg or verified by Puzzler/Dockpoint
  RJ_API void* admit_survivor(void* original_oop, size_t object_size,
                              bool is_puzzler_optimized = false, uint32_t zone_hint = 0);

  // 2. Compacts fragmented slabs by relocating live survivors into contiguous space
  RJ_API size_t execute_in_slab_compaction();

  // 3. Advances epoch and identifies objects ready for Hotfield promotion
  RJ_API size_t advance_epoch_and_mark_promotions(void** out_promoted_oops, size_t max_out);

  // 4. Sizing & Telemetry
  size_t   slab_count() const noexcept { return _slab_count.load(std::memory_order_relaxed); }
  uint64_t current_epoch() const noexcept { return _fledgling_epoch.load(std::memory_order_relaxed); }
  uint64_t total_admissions() const noexcept { return _total_admissions.load(std::memory_order_relaxed); }
  uint64_t total_promotions() const noexcept { return _total_promotions.load(std::memory_order_relaxed); }
  uint64_t total_compactions() const noexcept { return _total_compactions.load(std::memory_order_relaxed); }
  uint64_t bytes_compacted() const noexcept { return _bytes_compacted.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_RUNWAYFLEDGLING_HPP
