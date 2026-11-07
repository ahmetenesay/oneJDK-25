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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_LANDFILL_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_LANDFILL_HPP

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
// Landfill VFS Node Controller (/heap/on_heap/landfill) (PLAN.md Sec 9.1 & 8.1.1)
// - Trash Pit Matured: OS physical memory uncommit engine
// - Trash Pit Rehab: Resurrected reference staging and rehabilitation
// - Header Pane: Isolated 8-byte compact metaspace header storage
// ============================================================================

struct alignas(64) RedjetCompactHeaderEntry {
  uint64_t                  compact_mark_word{ 0 };
  uint32_t                  compressed_klass_id{ 0 };
  uint32_t                  flags{ 0 };
  void*                     associated_oop{ nullptr };
  RedjetCompactHeaderEntry* next_free{ nullptr };
};

struct alignas(64) RedjetHeaderPaneBlock {
  static constexpr size_t ENTRIES_PER_BLOCK = 4096;

  RedjetCompactHeaderEntry  entries[ENTRIES_PER_BLOCK];
  std::atomic<size_t>       allocated_entries{ 0 };
  RedjetCompactHeaderEntry* free_list{ nullptr };
  RedjetHeaderPaneBlock*    next_block{ nullptr };
  hs_ticket_lock_t          block_lock;

  void init() {
    allocated_entries.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < ENTRIES_PER_BLOCK - 1; ++i) {
      entries[i].next_free = &entries[i + 1];
    }
    entries[ENTRIES_PER_BLOCK - 1].next_free = nullptr;
    free_list = &entries[0];
  }

  RedjetCompactHeaderEntry* allocate() {
    hs_ticket_guard_t guard(block_lock);
    if (free_list != nullptr) {
      RedjetCompactHeaderEntry* e = free_list;
      free_list = e->next_free;
      e->next_free = nullptr;
      allocated_entries.fetch_add(1, std::memory_order_relaxed);
      return e;
    }
    return nullptr;
  }

  void free_entry(RedjetCompactHeaderEntry* e) {
    hs_ticket_guard_t guard(block_lock);
    e->associated_oop = nullptr;
    e->compact_mark_word = 0;
    e->next_free = free_list;
    free_list = e;
    allocated_entries.fetch_sub(1, std::memory_order_relaxed);
  }
};

class RedjetVFSNode_Landfill : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_QUOTA = 1024ULL * 1024 * 1024; // 1 GB
  static constexpr size_t MAX_UNCOMMIT_BATCH = 1024;

private:
  // Matured Uncommit State
  std::atomic<uint64_t> _total_uncommit_operations{ 0 };
  std::atomic<uint64_t> _total_bytes_uncommitted{ 0 };

  // Rehab State
  std::atomic<uint64_t> _total_rehab_admissions{ 0 };
  std::atomic<uint64_t> _total_rehab_returns{ 0 };

  // Header Pane State
  RedjetHeaderPaneBlock* _header_blocks_head{ nullptr };
  std::atomic<size_t>    _header_block_count{ 0 };
  std::atomic<uint64_t>  _total_header_allocations{ 0 };
  std::atomic<uint64_t>  _total_header_frees{ 0 };
  hs_ticket_lock_t       _landfill_lock;

  RedjetHeaderPaneBlock* allocate_header_block();

public:
  RedjetVFSNode_Landfill(const char* name = "landfill",
                         const char* full_path = "/heap/on_heap/landfill",
                         RedjetVFSNode* parent = nullptr,
                         size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_Landfill();

  // 1. Trash Pit Matured: Uncommits condemned physical pages back to OS
  RJ_API size_t uncommit_matured_pages(void* base_addr, size_t num_bytes);

  // 2. Trash Pit Rehab: Stages and returns resurrected references
  RJ_API void stage_rehab_reference(void* oop, size_t sz);
  RJ_API bool dispatch_rehab_return(void* oop, void** out_destination_slot);

  // 3. Header Pane: Compact Metaspace Header Allocator & Deallocator
  RJ_API RedjetCompactHeaderEntry* allocate_compact_header(void* oop, uint32_t klass_id, uint64_t mark_word);
  RJ_API void free_compact_header(RedjetCompactHeaderEntry* header);

  // Sizing & Telemetry
  uint64_t total_uncommits()        const noexcept { return _total_uncommit_operations.load(std::memory_order_relaxed); }
  uint64_t total_bytes_uncommitted() const noexcept { return _total_bytes_uncommitted.load(std::memory_order_relaxed); }
  uint64_t total_rehab_admissions() const noexcept { return _total_rehab_admissions.load(std::memory_order_relaxed); }
  uint64_t total_rehab_returns()    const noexcept { return _total_rehab_returns.load(std::memory_order_relaxed); }
  uint64_t total_headers_active()   const noexcept {
    uint64_t a = _total_header_allocations.load(std::memory_order_relaxed);
    uint64_t f = _total_header_frees.load(std::memory_order_relaxed);
    return (a > f) ? (a - f) : 0;
  }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_LANDFILL_HPP
