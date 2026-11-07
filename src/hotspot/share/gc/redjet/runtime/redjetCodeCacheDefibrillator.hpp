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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETCODECACHEDEFIBRILLATOR_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETCODECACHEDEFIBRILLATOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/vfs/redjetVFSNode_MircCodePage.hpp"

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
// Redjet Dynamic Code Cache Defibrillator (PLAN.md Section 9.1, 9.2)
// - Sweeps obsolete/deoptimized JIT code pages from off-heap code_boot
// - Zeroes dead code blocks to eliminate ghost execution vectors
// - Flushes multi-core L1-I cache to prevent stale instruction fetches
// - Defragments MIRC execution pages for maximum spatial locality
// ============================================================================

struct RedjetDefibrillationReport {
  size_t   pages_scanned{ 0 };
  size_t   obsolete_pages_purged{ 0 };
  size_t   bytes_reclaimed{ 0 };
  uint64_t icache_flushes_executed{ 0 };
  uint64_t duration_tsc{ 0 };
};

class RedjetCodeCacheDefibrillator {
private:
  static RedjetCodeCacheDefibrillator* _instance;
  static std::atomic<bool>             _initialized;

  RedjetVFSNode_MircCodePage*          _mirc_node{ nullptr };
  hs_ticket_lock_t                     _defib_lock;

  // Telemetry
  std::atomic<uint64_t>                _total_sweeps_executed{ 0 };
  std::atomic<uint64_t>                _total_obsolete_methods_purged{ 0 };
  std::atomic<uint64_t>                _total_bytes_reclaimed{ 0 };
  std::atomic<uint64_t>                _total_icache_flushes{ 0 };

public:
  RedjetCodeCacheDefibrillator();
  ~RedjetCodeCacheDefibrillator();

  static void initialize(RedjetVFSNode_MircCodePage* mirc_node = nullptr);
  static void destroy();
  static RedjetCodeCacheDefibrillator* get() noexcept { return _instance; }

  void set_mirc_node(RedjetVFSNode_MircCodePage* node) noexcept { _mirc_node = node; }

  // 1. Perform Defibrillation Sweep
  RJ_API RedjetDefibrillationReport sweep_obsolete_pages();

  // 2. Hardware Multi-Core Instruction Cache Synchronizer
  RJ_API static void flush_hardware_icache(void* start_addr, size_t bytes);

  // Telemetry
  uint64_t sweeps_executed() const noexcept { return _total_sweeps_executed.load(std::memory_order_relaxed); }
  uint64_t methods_purged()  const noexcept { return _total_obsolete_methods_purged.load(std::memory_order_relaxed); }
  uint64_t bytes_reclaimed() const noexcept { return _total_bytes_reclaimed.load(std::memory_order_relaxed); }
  uint64_t icache_flushes()  const noexcept { return _total_icache_flushes.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETCODECACHEDEFIBRILLATOR_HPP
