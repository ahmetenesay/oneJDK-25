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

#include "gc/redjet/vfs/redjetVFSNode_LogBoot.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <cstdio>

RedjetVFSNode_LogBoot::RedjetVFSNode_LogBoot(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  for (size_t i = 0; i < HOTLOG_CAPACITY; ++i) {
    _hotlog_ring[i].timestamp_tsc = 0;
    _hotlog_ring[i].event_id = 0;
    _hotlog_ring[i].thread_id = 0;
    _hotlog_ring[i].param1 = 0;
    _hotlog_ring[i].param2 = 0;
    _hotlog_ring[i].hex_group_id = 0;
    _hotlog_ring[i].flags = 0;
  }
}

RedjetVFSNode_LogBoot::~RedjetVFSNode_LogBoot() {
  // Empty ring
}

size_t RedjetVFSNode_LogBoot::drain_coldlog_batch(outputStream* st, size_t max_entries) {
  hs_ticket_guard_t guard(_drain_lock);

  uint64_t drain = _drain_cursor.load(std::memory_order_relaxed);
  uint64_t write = _write_cursor.load(std::memory_order_acquire);

  if (drain >= write) {
    return 0; // Nothing to drain
  }

  // Handle potential buffer overrun if writer wrapped around
  if ((write - drain) > HOTLOG_CAPACITY) {
    _buffer_wraparounds.fetch_add(1, std::memory_order_relaxed);
    drain = write - HOTLOG_CAPACITY;
  }

  size_t count = 0;
  while (drain < write && count < max_entries) {
    RedjetHotLogEntry& entry = _hotlog_ring[drain & HOTLOG_MASK];

    if (st != nullptr && entry.flags != 0) {
      st->print_cr("[ColdLog] TSC: " UINT64_FORMAT " | Evt: 0x%04X | Tid: %u | HexGrp: 0x%04X | P1: " UINT64_FORMAT " | P2: " UINT64_FORMAT,
                   entry.timestamp_tsc, entry.event_id, entry.thread_id,
                   entry.hex_group_id, entry.param1, entry.param2);
    }

    entry.flags = 0;
    ++drain;
    ++count;
  }

  _drain_cursor.store(drain, std::memory_order_release);
  _total_drained.fetch_add(count, std::memory_order_relaxed);
  return count;
}

size_t RedjetVFSNode_LogBoot::flush_coldlog(outputStream* st) {
  size_t total_flushed = 0;
  while (pending_log_count() > 0) {
    size_t batch = drain_coldlog_batch(st, 512);
    if (batch == 0) break;
    total_flushed += batch;
  }
  return total_flushed;
}

void RedjetVFSNode_LogBoot::print_on(outputStream* st) const {
  st->print_cr("Redjet Log Boot VFS Node [%s]:", full_path());
  st->print_cr("  HotLog Capacity: %zu entries (256 KB SRAM Ring)", HOTLOG_CAPACITY);
  st->print_cr("  Total Emissions: " UINT64_FORMAT ", Total Drained: " UINT64_FORMAT,
               total_emissions(), total_drained());
  st->print_cr("  Pending in SRAM Ring: %zu, Wraparounds: " UINT64_FORMAT,
               pending_log_count(), _buffer_wraparounds.load(std::memory_order_relaxed));
}
