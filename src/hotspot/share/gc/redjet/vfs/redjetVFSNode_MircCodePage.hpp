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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_MIRCCODEPAGE_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_MIRCCODEPAGE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/vfs/redjetVFSCrypto.hpp"
#include "gc/redjet/vfs/redjetVFSNode_CodeBoot.hpp"
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
// MIRC Execution Ready Page Controller (/heap/off_heap/code_boot/mirc_execution_ready_page)
// (PLAN.md Section 9.1 & 9.2)
// - Cryptographically secured W^X machine code execution pages
// - Multi-tier execution: Level 1 (fast JIT) & Level 2 (optimizing superloop)
// - Slot 16 Trampoline hot-swapper with atomic instruction patching
// - Full telemetry and hardware cache flush synchronization
// ============================================================================

struct alignas(64) RedjetMircPageEntry {
  void*            code_address{ nullptr };
  size_t           page_size{ 0 };
  size_t           code_bytes{ 0 };
  uint32_t         method_id{ 0 };
  uint8_t          opt_level{ 1 };     // 1 = lvl1, 2 = lvl2
  RedjetWXState    wx_state{ RedjetWXState::WX_WRITABLE };
  uint64_t         crypto_seal{ 0 };
  uint64_t         entry_tsc{ 0 };
  void*            slot16_trampoline_addr{ nullptr };
  uint8_t          saved_trampoline_bytes[16]{ 0 };
};

class RedjetVFSNode_MircCodePage : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_PAGE_QUOTA = 128ULL * 1024 * 1024; // 128 MB
  static constexpr size_t MAX_MIRC_PAGES     = 16384;

private:
  RedjetMircPageEntry   _pages[MAX_MIRC_PAGES];
  size_t                _page_count{ 0 };
  hs_ticket_lock_t      _mirc_lock;

  // Dedicated sub-nodes
  RedjetVFSNode*        _node_lvl1{ nullptr };
  RedjetVFSNode*        _node_lvl2{ nullptr };

  uint64_t              _mirc_read_key{ 0 };

  // Telemetry
  std::atomic<uint64_t> _total_pages_created{ 0 };
  std::atomic<uint64_t> _total_wx_executable_promotions{ 0 };
  std::atomic<uint64_t> _total_slot16_trampoline_swaps{ 0 };
  std::atomic<uint64_t> _total_pages_deoptimized{ 0 };

public:
  RedjetVFSNode_MircCodePage(const char* name = "mirc_execution_ready_page",
                             const char* full_path = "/heap/off_heap/code_boot/mirc_execution_ready_page",
                             RedjetVFSNode* parent = nullptr,
                             size_t quota_bytes = DEFAULT_PAGE_QUOTA);
  virtual ~RedjetVFSNode_MircCodePage();

  void init(uint64_t mirc_read_key = 0);
  void destroy();

  // 1. Code Page Allocation (starts as Writable)
  RJ_API RedjetMircPageEntry* allocate_code_page(uint32_t method_id, uint8_t opt_level, size_t code_size);

  // 2. Machine Code Emission Commit & W^X Transition to Executable
  RJ_API bool commit_and_make_executable(RedjetMircPageEntry* entry, uint64_t auth_key);

  // 3. Slot 16 Atomic Trampoline Hot-Swap
  // Patches the method's 16-byte invocation trampoline to jump directly to this MIRC page
  RJ_API bool patch_slot16_trampoline(RedjetMircPageEntry* entry, void* trampoline_addr);

  // 4. Reverts Trampoline back to baseline (Deoptimization)
  RJ_API bool rollback_slot16_trampoline(RedjetMircPageEntry* entry);

  // 5. Lookup page by method and level
  RJ_API RedjetMircPageEntry* find_page(uint32_t method_id, uint8_t opt_level) noexcept;

  // Sub-nodes
  RedjetVFSNode* lvl1_node() const noexcept { return _node_lvl1; }
  RedjetVFSNode* lvl2_node() const noexcept { return _node_lvl2; }

  // Telemetry
  size_t   page_count()        const noexcept { return _page_count; }
  uint64_t pages_created()     const noexcept { return _total_pages_created.load(std::memory_order_relaxed); }
  uint64_t wx_promotions()     const noexcept { return _total_wx_executable_promotions.load(std::memory_order_relaxed); }
  uint64_t trampoline_swaps()  const noexcept { return _total_slot16_trampoline_swaps.load(std::memory_order_relaxed); }
  uint64_t deoptimizations()   const noexcept { return _total_pages_deoptimized.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_MIRCCODEPAGE_HPP
