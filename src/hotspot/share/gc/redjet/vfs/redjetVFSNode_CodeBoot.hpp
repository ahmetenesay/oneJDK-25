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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_CODEBOOT_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_CODEBOOT_HPP

#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/vfs/redjetVFSCrypto.hpp"
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

/* ========================================================================= */
/* 1. CODEBOOT PAGE DESCRIPTORS & W^X PERMISSION TIERS                       */
/* ========================================================================= */

enum class RedjetCodePageTier : uint8_t {
  RAWCODE_CELL            = 0, // Bytecode cells sealed with KEY_VAC_READ
  VAC_PROCESSED_CHUNK     = 1, // Intermediate chunks sealed with KEY_MIRC_READ
  MIRC_READY_PAGE_LVL1    = 2, // Tier-1 fast JIT code page (W^X protected)
  MIRC_READY_PAGE_LVL2    = 3  // Tier-2 optimizing JIT code page (W^X protected)
};

enum class RedjetWXState : uint8_t {
  WX_WRITABLE   = 0, // PAGE_READWRITE (compilation/emission in progress)
  WX_EXECUTABLE = 1, // PAGE_EXECUTE_READ (locked for execution, writes forbidden)
  WX_SEALED_RO  = 2  // PAGE_READONLY (immutable bytecode/metadata)
};

struct alignas(64) RedjetCodePageDesc {
  void*               page_address{ nullptr };
  size_t              page_size{ 0 };
  size_t              used_bytes{ 0 };
  RedjetCodePageTier  tier{ RedjetCodePageTier::RAWCODE_CELL };
  RedjetWXState       wx_state{ RedjetWXState::WX_WRITABLE };
  uint64_t            auth_seal{ 0 };
  uint32_t            method_id{ 0 };
  uint64_t            alloc_tsc{ 0 };
  RedjetCodePageDesc* next{ nullptr };
};

/* ========================================================================= */
/* 2. CODEBOOT VFS NODE CONTROLLER (/heap/off_heap/code_boot)                */
/* ========================================================================= */

class RedjetVFSNode_CodeBoot : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_CODE_QUOTA = 128ULL * 1024 * 1024; // 128 MB off-heap
  static constexpr size_t SYSTEM_PAGE_SIZE   = 4096;

private:
  RedjetCodePageDesc*   _pages_head{ nullptr };
  hs_ticket_lock_t      _page_lock;

  // Dedicated sub-partitions
  RedjetVFSNode*        _node_rawcode_cell{ nullptr };
  RedjetVFSNode*        _node_vac_processed{ nullptr };
  RedjetVFSNode*        _node_mirc_ready{ nullptr };
  RedjetVFSNode*        _node_mirc_lvl1{ nullptr };
  RedjetVFSNode*        _node_mirc_lvl2{ nullptr };

  // Telemetry
  std::atomic<uint64_t> _total_pages_allocated{ 0 };
  std::atomic<uint64_t> _total_wx_transitions{ 0 };
  std::atomic<uint64_t> _total_crypto_seals_verified{ 0 };
  std::atomic<uint64_t> _total_unauthorized_access_trapped{ 0 };

  // Internal physical memory allocators
  static void* allocate_os_pages(size_t bytes, bool executable);
  static bool  protect_os_pages(void* addr, size_t bytes, RedjetWXState target_state);
  static void  free_os_pages(void* addr, size_t bytes);

public:
  RedjetVFSNode_CodeBoot(const char* name = "code_boot",
                         const char* full_path = "/heap/off_heap/code_boot",
                         RedjetVFSNode* parent = nullptr,
                         size_t quota_bytes = DEFAULT_CODE_QUOTA);
  virtual ~RedjetVFSNode_CodeBoot();

  // Allocates raw bytecode cell sealed with KEY_VAC_READ
  RJ_API void* alloc_rawcode_cell(size_t bytes, uint32_t method_id, uint64_t vac_key);

  // Allocates VAC intermediate processed chunk sealed with KEY_MIRC_READ
  RJ_API void* alloc_vac_processed_chunk(size_t bytes, uint32_t method_id, uint64_t mirc_key);

  // Allocates MIRC JIT execution-ready page (starts writable, transitions to executable)
  RJ_API void* alloc_mirc_ready_page(size_t bytes, uint32_t method_id, int mirc_level, uint64_t jit_key);

  // Enforces W^X transition: flips page from Writable to Executable (or vice versa)
  RJ_API bool transition_wx(void* page_addr, size_t bytes, RedjetWXState target_state, uint64_t auth_key);

  // Verifies 7-cipher cryptographic integrity seal on a code page
  RJ_API bool verify_page_seal(const void* page_addr, uint64_t expected_seal) noexcept;

  // Frees and releases code page memory back to OS
  RJ_API bool free_code_page(void* page_addr);

  // Sub-node accessors
  RedjetVFSNode* rawcode_node()    const noexcept { return _node_rawcode_cell; }
  RedjetVFSNode* vac_chunk_node()  const noexcept { return _node_vac_processed; }
  RedjetVFSNode* mirc_ready_node() const noexcept { return _node_mirc_ready; }
  RedjetVFSNode* mirc_lvl1_node()  const noexcept { return _node_mirc_lvl1; }
  RedjetVFSNode* mirc_lvl2_node()  const noexcept { return _node_mirc_lvl2; }

  // Telemetry accessors
  uint64_t pages_allocated()      const noexcept { return _total_pages_allocated.load(std::memory_order_relaxed); }
  uint64_t wx_transitions()       const noexcept { return _total_wx_transitions.load(std::memory_order_relaxed); }
  uint64_t seals_verified()       const noexcept { return _total_crypto_seals_verified.load(std::memory_order_relaxed); }
  uint64_t access_trapped()       const noexcept { return _total_unauthorized_access_trapped.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_CODEBOOT_HPP
