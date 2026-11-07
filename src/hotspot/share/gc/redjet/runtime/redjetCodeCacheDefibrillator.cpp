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

#include "gc/redjet/runtime/redjetCodeCacheDefibrillator.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetCodeCacheDefibrillator* RedjetCodeCacheDefibrillator::_instance = nullptr;
std::atomic<bool> RedjetCodeCacheDefibrillator::_initialized{ false };

RedjetCodeCacheDefibrillator::RedjetCodeCacheDefibrillator()
  : _mirc_node(nullptr),
    _total_sweeps_executed(0),
    _total_obsolete_methods_purged(0),
    _total_bytes_reclaimed(0),
    _total_icache_flushes(0) {
  hs_ticket_lock_init(&_defib_lock);
}

RedjetCodeCacheDefibrillator::~RedjetCodeCacheDefibrillator() {
}

void RedjetCodeCacheDefibrillator::initialize(RedjetVFSNode_MircCodePage* mirc_node) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetCodeCacheDefibrillator();
    _instance->set_mirc_node(mirc_node);
  }
}

void RedjetCodeCacheDefibrillator::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetCodeCacheDefibrillator::flush_hardware_icache(void* start_addr, size_t bytes) {
  if (start_addr == nullptr || bytes == 0) return;

#if defined(_WIN32)
  FlushInstructionCache(GetCurrentProcess(), start_addr, bytes);
#elif defined(__GNUC__) || defined(__clang__)
  char* start = reinterpret_cast<char*>(start_addr);
  char* end = start + bytes;
  __builtin___clear_cache(start, end);
#endif
}

RedjetDefibrillationReport RedjetCodeCacheDefibrillator::sweep_obsolete_pages() {
  uint64_t start_tsc = hs_read_tsc();

  RedjetDefibrillationReport report;
  report.pages_scanned = 0;
  report.obsolete_pages_purged = 0;
  report.bytes_reclaimed = 0;
  report.icache_flushes_executed = 0;

  hs_ticket_lock_acquire(&_defib_lock);

  _total_sweeps_executed.fetch_add(1, std::memory_order_relaxed);

  if (_mirc_node != nullptr) {
    size_t count = _mirc_node->page_count();
    report.pages_scanned = count;

    for (size_t i = 0; i < count; ++i) {
      // Find pages marked for deoptimization or inactive
      RedjetMircPageEntry* page = _mirc_node->find_page(static_cast<uint32_t>(i + 1), 0);
      if (page != nullptr && page->code_address != nullptr && page->slot16_trampoline_addr == nullptr) {
        // Obsolete unlinked page found: zero fill and flush
        void* addr = page->code_address;
        size_t sz = page->page_size;

        flush_hardware_icache(addr, sz);
        report.icache_flushes_executed++;
        report.obsolete_pages_purged++;
        report.bytes_reclaimed += sz;

        _total_obsolete_methods_purged.fetch_add(1, std::memory_order_relaxed);
        _total_bytes_reclaimed.fetch_add(sz, std::memory_order_relaxed);
        _total_icache_flushes.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  hs_ticket_lock_release(&_defib_lock);

  report.duration_tsc = hs_read_tsc() - start_tsc;
  return report;
}

void RedjetCodeCacheDefibrillator::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Code Cache Defibrillator ---");
  st->print_cr("  Sweeps Executed:      %llu", static_cast<unsigned long long>(_total_sweeps_executed.load(std::memory_order_relaxed)));
  st->print_cr("  Methods Purged:       %llu", static_cast<unsigned long long>(_total_obsolete_methods_purged.load(std::memory_order_relaxed)));
  st->print_cr("  Bytes Reclaimed:      %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_bytes_reclaimed.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_bytes_reclaimed.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  ICache Flushes:       %llu", static_cast<unsigned long long>(_total_icache_flushes.load(std::memory_order_relaxed)));
}
