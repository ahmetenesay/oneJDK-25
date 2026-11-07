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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETPAGEDEFRAG_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETPAGEDEFRAG_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/vfs/redjetVFSNode.hpp"

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
// Autonomous VFS Page Defragmenter & DRAM Compactor (PLAN.md Section 4.3, 7.3)
// - Evaluates physical page fragmentation index across all 40 VFS branches
// - PLUMP Algorithm: Prune, coalesce, and bulk rebate
// - SEQUENTIAL Algorithm: Immediate block-by-block rebate
// - Binary Buddy Allocator coalescer for 4KB, 16KB, 64KB, 256KB, 1MB, 2MB, 1GB pages
// - Multi-threaded concurrent compaction with zero mutator interference
// ============================================================================

enum class RedjetCompactionAlgorithm : uint8_t {
  PLUMP_BULK_REBATE  = 0, // Prune & coalesce before bulk uncommit
  SEQUENTIAL_REBATE  = 1  // Immediate per-block release
};

enum class RedjetPageOrder : uint8_t {
  ORDER_4K   = 0, // 4 KB
  ORDER_16K  = 1, // 16 KB
  ORDER_64K  = 2, // 64 KB
  ORDER_256K = 3, // 256 KB
  ORDER_1M   = 4, // 1 MB
  ORDER_2M   = 5, // 2 MB (Huge Page)
  ORDER_1G   = 6, // 1 GB (Gigantic Page)
  ORDER_MAX  = 7
};

struct RedjetDefragPage {
  void*            page_base{ nullptr };
  size_t           page_size{ 0 };
  RedjetPageOrder  order{ RedjetPageOrder::ORDER_4K };
  bool             is_free{ true };
  uint32_t         live_objects{ 0 };
  size_t           live_bytes{ 0 };
  RedjetDefragPage* next{ nullptr };
  RedjetDefragPage* buddy{ nullptr };
};

struct RedjetDefragReport {
  double   fragmentation_index{ 0.0 };
  size_t   total_managed_bytes{ 0 };
  size_t   total_free_bytes{ 0 };
  size_t   largest_contiguous_free_bytes{ 0 };
  size_t   pages_compacted{ 0 };
  size_t   bytes_rebated{ 0 };
  uint64_t compaction_duration_cycles{ 0 };
};

class RedjetPageDefrag {
public:
  static constexpr size_t MAX_MANAGED_PAGES   = 65536;
  static constexpr double FRAG_THRESHOLD_WARN  = 0.35; // Trigger compaction above 35% fragmentation
  static constexpr double FRAG_THRESHOLD_PANIC = 0.65; // Emergency full PLUMP compaction

private:
  RedjetDefragPage*       _free_lists[static_cast<size_t>(RedjetPageOrder::ORDER_MAX)]{ nullptr };
  RedjetDefragPage        _page_descriptors[MAX_MANAGED_PAGES];
  size_t                  _total_pages{ 0 };
  hs_ticket_lock_t        _defrag_lock;

  void*                   _managed_heap_base{ nullptr };
  size_t                  _managed_heap_size{ 0 };

  // Telemetry
  std::atomic<uint64_t>   _total_defrag_runs{ 0 };
  std::atomic<uint64_t>   _total_pages_coalesced{ 0 };
  std::atomic<uint64_t>   _total_bytes_evacuated{ 0 };
  std::atomic<uint64_t>   _total_bytes_rebated_os{ 0 };

  static inline size_t order_to_bytes(RedjetPageOrder order) noexcept {
    switch (order) {
      case RedjetPageOrder::ORDER_4K:   return 4096;
      case RedjetPageOrder::ORDER_16K:  return 16384;
      case RedjetPageOrder::ORDER_64K:  return 65536;
      case RedjetPageOrder::ORDER_256K: return 262144;
      case RedjetPageOrder::ORDER_1M:   return 1048576;
      case RedjetPageOrder::ORDER_2M:   return 2097152;
      case RedjetPageOrder::ORDER_1G:   return 1073741824;
      default: return 4096;
    }
  }

private:
  static RedjetPageDefrag* _instance;

public:
  RedjetPageDefrag();
  ~RedjetPageDefrag();

  static RedjetPageDefrag* get() noexcept { return _instance; }

  void init(void* heap_base, size_t heap_size);
  void destroy();

  RJ_API void rebate_bulk_pages(size_t num_pages);

  // 1. Fragmentation Evaluation
  RJ_API double calculate_fragmentation_index() noexcept;

  // 2. Buddy Allocator Page Allocation & Release
  RJ_API void* allocate_page(RedjetPageOrder order);
  RJ_API bool  free_page(void* page_addr, RedjetPageOrder order);

  // 3. Compaction Algorithms
  // PLUMP Algorithm: Prune, coalesce, and bulk rebate back to OS / landfill
  RJ_API RedjetDefragReport execute_plump_compaction(size_t target_rebate_bytes);

  // SEQUENTIAL Algorithm: Immediate per-block coalescing and rebate
  RJ_API RedjetDefragReport execute_sequential_compaction();

  // 4. Autonomous Defrag Step (invoked by background scheduler)
  RJ_API bool run_autonomous_cycle();

  // Telemetry
  uint64_t defrag_runs()     const noexcept { return _total_defrag_runs.load(std::memory_order_relaxed); }
  uint64_t pages_coalesced() const noexcept { return _total_pages_coalesced.load(std::memory_order_relaxed); }
  uint64_t bytes_evacuated() const noexcept { return _total_bytes_evacuated.load(std::memory_order_relaxed); }
  uint64_t bytes_rebated()   const noexcept { return _total_bytes_rebated_os.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETPAGEDEFRAG_HPP
