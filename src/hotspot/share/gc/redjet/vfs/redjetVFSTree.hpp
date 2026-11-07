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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSTREE_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSTREE_HPP

#include <atomic>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif
#ifdef REDJET_STANDALONE_BUILD
#include "gc/redjet/vfs/redjetVFSNode.hpp"
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
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/ostream.hpp"
#endif

class RedjetVFSNode_RPSS;
class RedjetVFSNode_CodeBoot;
class RedjetVFSNode_Pitstop;
class RedjetVFSNode_RunwayEgg;
class RedjetVFSNode_RunwayFledgling;
class RedjetVFSNode_Falline;
class RedjetVFSNode_PuzzleArena;
class RedjetVFSNode_Landfill;
class RedjetVFSNode_LogBoot;
class RedjetVFSNode_ReservedBoot;
class RedjetVFSNode_Hotfield;
class RedjetVFSNode_ParkArena;
class RedjetVFSNode_CCDRunway;
class RedjetVFSNode_StripedRunway;
class RedjetVFSNode_HeaderPane;
class RedjetVFSNode_RawCodeCell;
class RedjetVFSNode_VacCodeChunk;
class RedjetVFSNode_MircCodePage;

class RedjetVFSTree : public CHeapObj<mtGC> {
private:
  RedjetVFSNode*                 _root;
  RedjetVFSNode_RPSS*            _rpss_controller{ nullptr };
  RedjetVFSNode_CodeBoot*        _codeboot_controller{ nullptr };
  RedjetVFSNode_Pitstop*         _pitstop_controller{ nullptr };
  RedjetVFSNode_RunwayEgg*       _egg_controller{ nullptr };
  RedjetVFSNode_RunwayFledgling* _fledgling_controller{ nullptr };
  RedjetVFSNode_Falline*         _falline_controller{ nullptr };
  RedjetVFSNode_PuzzleArena*     _puzzle_arena_controller{ nullptr };
  RedjetVFSNode_Landfill*        _landfill_controller{ nullptr };
  RedjetVFSNode_LogBoot*         _logboot_controller{ nullptr };
  RedjetVFSNode_ReservedBoot*    _reserved_boot_controller{ nullptr };
  RedjetVFSNode_Hotfield*        _hotfield_controller{ nullptr };
  RedjetVFSNode_ParkArena*       _park_arena_controller{ nullptr };
  RedjetVFSNode_CCDRunway*       _ccd_runway_controller{ nullptr };
  RedjetVFSNode_StripedRunway*   _striped_runway_controller{ nullptr };
  RedjetVFSNode_HeaderPane*      _header_pane_controller{ nullptr };
  RedjetVFSNode_RawCodeCell*     _rawcode_cell_controller{ nullptr };
  RedjetVFSNode_VacCodeChunk*    _vac_chunk_controller{ nullptr };
  RedjetVFSNode_MircCodePage*    _mirc_codepage_controller{ nullptr };
  std::atomic<bool>              _lock;

  // Real-time O(1) Domain Byte Ledgers: [domain][0 = off_heap, 1 = on_heap]
  std::atomic<size_t>            _java_on_heap_bytes{ 0 };
  std::atomic<size_t>            _java_off_heap_bytes{ 0 };
  std::atomic<size_t>            _hotspot_on_heap_bytes{ 0 };
  std::atomic<size_t>            _hotspot_off_heap_bytes{ 0 };

  inline void lock() {
    while (_lock.exchange(true, std::memory_order_acquire)) {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
      _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
      __yield();
#else
      __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
    }
  }

  inline void unlock() {
    _lock.store(false, std::memory_order_release);
  }

  RedjetVFSNode* _find_node_unlocked(const char* path);
  RedjetVFSNode* _mkdir_p_unlocked(const char* path, size_t quota_bytes = 0);

public:
  RedjetVFSTree();
  ~RedjetVFSTree();

  void init();
  void destroy();

  RedjetVFSNode* mkdir_p(const char* path, size_t quota_bytes = 0);
  RedjetVFSNode* find_node(const char* path);
  size_t get_used_bytes(const char* path);

  // Logical Domain Accounting (`java_memory` vs `hotspot_memory` across `on_heap` & `off_heap`)
  void   record_domain_alloc(RedjetMemoryDomain domain, bool on_heap, size_t bytes) noexcept;
  void   record_domain_free(RedjetMemoryDomain domain, bool on_heap, size_t bytes) noexcept;
  void   record_path_alloc(const char* path, size_t bytes) noexcept;
  void   record_path_free(const char* path, size_t bytes) noexcept;
  size_t domain_used_bytes(RedjetMemoryDomain domain, bool on_heap) const noexcept;
  size_t total_domain_used_bytes(RedjetMemoryDomain domain) const noexcept {
    return domain_used_bytes(domain, true) + domain_used_bytes(domain, false);
  }
  void   print_domain_summary_on(outputStream* st) const;

  RedjetVFSNode_RPSS*            rpss_node()          const noexcept { return _rpss_controller; }
  RedjetVFSNode_CodeBoot*        codeboot_node()      const noexcept { return _codeboot_controller; }
  RedjetVFSNode_Pitstop*         pitstop_node()       const noexcept { return _pitstop_controller; }
  RedjetVFSNode_RunwayEgg*       egg_node()           const noexcept { return _egg_controller; }
  RedjetVFSNode_RunwayFledgling* fledgling_node()     const noexcept { return _fledgling_controller; }
  RedjetVFSNode_Falline*         falline_node()       const noexcept { return _falline_controller; }
  RedjetVFSNode_PuzzleArena*     puzzle_arena_node()  const noexcept { return _puzzle_arena_controller; }
  RedjetVFSNode_Landfill*        landfill_node()      const noexcept { return _landfill_controller; }
  RedjetVFSNode_LogBoot*         logboot_node()       const noexcept { return _logboot_controller; }
  RedjetVFSNode_ReservedBoot*    reserved_boot_node() const noexcept { return _reserved_boot_controller; }
  RedjetVFSNode_Hotfield*        hotfield_node()      const noexcept { return _hotfield_controller; }
  RedjetVFSNode_ParkArena*       park_arena_node()    const noexcept { return _park_arena_controller; }
  RedjetVFSNode_CCDRunway*       ccd_runway_node()    const noexcept { return _ccd_runway_controller; }
  RedjetVFSNode_StripedRunway*   striped_runway_node()const noexcept { return _striped_runway_controller; }
  RedjetVFSNode_HeaderPane*      header_pane_node()   const noexcept { return _header_pane_controller; }
  RedjetVFSNode_RawCodeCell*     rawcode_cell_node()  const noexcept { return _rawcode_cell_controller; }
  RedjetVFSNode_VacCodeChunk*    vac_chunk_node()     const noexcept { return _vac_chunk_controller; }
  RedjetVFSNode_MircCodePage*    mirc_codepage_node() const noexcept { return _mirc_codepage_controller; }

  // PLAN.md Section 9.2: 7-Key Cryptographic Derivation
  static uint64_t derive_crypto_key(const uint64_t K[7]);
  static const uint64_t KEY_VAC_READ;
  static const uint64_t KEY_MIRC_READ;

  // PLAN.md Section 9.2: W^X Memory Protection Check
  bool check_wx_access(const char* path, uint64_t access_key);

  // PLAN.md Section 9.3: Tetrasexagesimal 64-Bit Recovery Matrix and Verification
  static const uint64_t GOLDEN_VFS_TETRASEXAGESIMAL_MATRIX[43];
  bool verify_and_restore_golden_vfs();
  RedjetVFSNode* resolve_or_heal_branch(const char* path);

  void print_on(outputStream* st) const;
  void print_node_recursive(outputStream* st, RedjetVFSNode* node, int indent) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSTREE_HPP