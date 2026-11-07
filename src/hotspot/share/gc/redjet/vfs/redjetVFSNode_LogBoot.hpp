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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_LOGBOOT_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_LOGBOOT_HPP

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
// Log Boot VFS Node Controller (/heap/off_heap/log_boot) (PLAN.md Sec 9.1 & 7.4)
// - HotLog: 0.1 ns binary ring buffer in L1/SRAM cache (no string formatting)
// - ColdLog: Asynchronous drain engine to disk / console
// ============================================================================

struct alignas(32) RedjetHotLogEntry {
  uint64_t timestamp_tsc{ 0 };
  uint32_t event_id{ 0 };
  uint32_t thread_id{ 0 };
  uint64_t param1{ 0 };
  uint64_t param2{ 0 };
  uint32_t hex_group_id{ 0 }; // Hex group id (0x1000..0xFFFF) from PLAN.md Sec 8.8
  uint32_t flags{ 0 };
};

class RedjetVFSNode_LogBoot : public RedjetVFSNode {
public:
  static constexpr size_t HOTLOG_CAPACITY = 8192; // 8192 slots * 32 bytes = 256 KB SRAM footprint
  static constexpr size_t HOTLOG_MASK = HOTLOG_CAPACITY - 1;
  static constexpr size_t DEFAULT_QUOTA = 64ULL * 1024 * 1024; // 64 MB

private:
  RedjetHotLogEntry                 _hotlog_ring[HOTLOG_CAPACITY];
  alignas(64) std::atomic<uint64_t> _write_cursor{ 0 };
  alignas(64) std::atomic<uint64_t> _drain_cursor{ 0 };

  // Telemetry
  std::atomic<uint64_t> _total_emissions{ 0 };
  std::atomic<uint64_t> _total_drained{ 0 };
  std::atomic<uint64_t> _buffer_wraparounds{ 0 };
  hs_ticket_lock_t      _drain_lock;

public:
  RedjetVFSNode_LogBoot(const char* name = "log_boot",
                        const char* full_path = "/heap/off_heap/log_boot",
                        RedjetVFSNode* parent = nullptr,
                        size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_LogBoot();

  // 1. HotLog: Lockless, 0.1 ns binary emission in L1/SRAM
  inline void emit_hotlog(uint32_t event_id, uint32_t thread_id,
                          uint64_t p1 = 0, uint64_t p2 = 0, uint32_t hex_group = 0) noexcept {
    uint64_t idx = _write_cursor.fetch_add(1, std::memory_order_relaxed);
    RedjetHotLogEntry& entry = _hotlog_ring[idx & HOTLOG_MASK];

    entry.timestamp_tsc = sacculus_rdtsc();
    entry.event_id = event_id;
    entry.thread_id = thread_id;
    entry.param1 = p1;
    entry.param2 = p2;
    entry.hex_group_id = hex_group;
    entry.flags = 1;

    _total_emissions.fetch_add(1, std::memory_order_relaxed);
  }

  // 2. ColdLog: Asynchronous batched drain to outputStream or file
  RJ_API size_t drain_coldlog_batch(outputStream* st, size_t max_entries = 512);

  // 3. Flush all pending hotlog entries
  RJ_API size_t flush_coldlog(outputStream* st);

  // Sizing & Telemetry
  size_t   pending_log_count() const noexcept {
    uint64_t w = _write_cursor.load(std::memory_order_relaxed);
    uint64_t d = _drain_cursor.load(std::memory_order_relaxed);
    return (w > d) ? static_cast<size_t>(w - d) : 0;
  }
  uint64_t total_emissions() const noexcept { return _total_emissions.load(std::memory_order_relaxed); }
  uint64_t total_drained()   const noexcept { return _total_drained.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_LOGBOOT_HPP
