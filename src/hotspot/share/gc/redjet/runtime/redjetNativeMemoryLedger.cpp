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

#include "gc/redjet/runtime/redjetNativeMemoryLedger.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetNativeMemoryLedger* RedjetNativeMemoryLedger::_instance = nullptr;
std::atomic<bool> RedjetNativeMemoryLedger::_initialized{ false };

RedjetNativeMemoryLedger::RedjetNativeMemoryLedger()
  : _total_reserved(0),
    _total_committed(0),
    _peak_committed(0) {
  hs_ticket_lock_init(&_ledger_lock);
}

RedjetNativeMemoryLedger::~RedjetNativeMemoryLedger() {
}

void RedjetNativeMemoryLedger::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetNativeMemoryLedger();
  }
}

void RedjetNativeMemoryLedger::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetNativeMemoryLedger::record_reserve(RedjetNativeTag tag, size_t bytes) noexcept {
  size_t idx = static_cast<size_t>(tag);
  if (idx >= static_cast<size_t>(RedjetNativeTag::TAG_COUNT)) return;

  _categories[idx].reserved_bytes.fetch_add(bytes, std::memory_order_relaxed);
  _categories[idx].allocation_count.fetch_add(1, std::memory_order_relaxed);
  _total_reserved.fetch_add(bytes, std::memory_order_relaxed);
}

void RedjetNativeMemoryLedger::record_commit(RedjetNativeTag tag, size_t bytes) noexcept {
  size_t idx = static_cast<size_t>(tag);
  if (idx >= static_cast<size_t>(RedjetNativeTag::TAG_COUNT)) return;

  size_t cur = _categories[idx].committed_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;

  // Update category peak
  size_t old_peak = _categories[idx].peak_committed_bytes.load(std::memory_order_relaxed);
  while (cur > old_peak && !_categories[idx].peak_committed_bytes.compare_exchange_weak(old_peak, cur,
                                                                                        std::memory_order_relaxed)) {}

  size_t total_cur = _total_committed.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  size_t global_peak = _peak_committed.load(std::memory_order_relaxed);
  while (total_cur > global_peak && !_peak_committed.compare_exchange_weak(global_peak, total_cur,
                                                                           std::memory_order_relaxed)) {}
}

void RedjetNativeMemoryLedger::record_decommit(RedjetNativeTag tag, size_t bytes) noexcept {
  size_t idx = static_cast<size_t>(tag);
  if (idx >= static_cast<size_t>(RedjetNativeTag::TAG_COUNT)) return;

  size_t cur = _categories[idx].committed_bytes.load(std::memory_order_relaxed);
  if (cur >= bytes) {
    _categories[idx].committed_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  } else {
    _categories[idx].committed_bytes.store(0, std::memory_order_relaxed);
  }

  size_t total_cur = _total_committed.load(std::memory_order_relaxed);
  if (total_cur >= bytes) {
    _total_committed.fetch_sub(bytes, std::memory_order_relaxed);
  } else {
    _total_committed.store(0, std::memory_order_relaxed);
  }
}

void RedjetNativeMemoryLedger::record_release(RedjetNativeTag tag, size_t bytes) noexcept {
  size_t idx = static_cast<size_t>(tag);
  if (idx >= static_cast<size_t>(RedjetNativeTag::TAG_COUNT)) return;

  size_t cur = _categories[idx].reserved_bytes.load(std::memory_order_relaxed);
  if (cur >= bytes) {
    _categories[idx].reserved_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  } else {
    _categories[idx].reserved_bytes.store(0, std::memory_order_relaxed);
  }

  _categories[idx].deallocation_count.fetch_add(1, std::memory_order_relaxed);

  size_t total_cur = _total_reserved.load(std::memory_order_relaxed);
  if (total_cur >= bytes) {
    _total_reserved.fetch_sub(bytes, std::memory_order_relaxed);
  } else {
    _total_reserved.store(0, std::memory_order_relaxed);
  }
}

void RedjetNativeMemoryLedger::print_on(outputStream* st) const {
  if (st == nullptr) return;

  const char* tag_names[] = { "CodeBoot", "LogBoot", "ReservedBoot", "Metadata", "Scratch" };

  st->print_cr("--- Redjet Off-Heap Native Memory Ledger ---");
  st->print_cr("  Total Reserved:       %zu MB", _total_reserved.load(std::memory_order_relaxed) / (1024 * 1024));
  st->print_cr("  Total Committed:      %zu MB (Peak: %zu MB)",
               _total_committed.load(std::memory_order_relaxed) / (1024 * 1024),
               _peak_committed.load(std::memory_order_relaxed) / (1024 * 1024));

  for (size_t i = 0; i < static_cast<size_t>(RedjetNativeTag::TAG_COUNT); ++i) {
    st->print_cr("  [%-12s] Reserved: %zu MB, Committed: %zu MB (Peak: %zu MB), Allocs: %llu, Frees: %llu",
                 tag_names[i],
                 _categories[i].reserved_bytes.load(std::memory_order_relaxed) / (1024 * 1024),
                 _categories[i].committed_bytes.load(std::memory_order_relaxed) / (1024 * 1024),
                 _categories[i].peak_committed_bytes.load(std::memory_order_relaxed) / (1024 * 1024),
                 static_cast<unsigned long long>(_categories[i].allocation_count.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(_categories[i].deallocation_count.load(std::memory_order_relaxed)));
  }
}
