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

#include "gc/redjet/api/redjetJavaAPIBridge.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
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

RedjetJavaAPIBridge g_redjet_java_api_bridge;

RedjetJavaAPIBridge::RedjetJavaAPIBridge() {
  _total_tube_feeds.store(0, std::memory_order_relaxed);
  _total_precision_scopes.store(0, std::memory_order_relaxed);
  _total_batch_scopes.store(0, std::memory_order_relaxed);
  _total_gc_hints_handled.store(0, std::memory_order_relaxed);
  _total_offheap_immediate_freed.store(0, std::memory_order_relaxed);
  _total_offheap_flow_watched.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < MAX_OFFHEAP_TRACKED; ++i) {
    _offheap_table[i].address = 0;
    _offheap_table[i].size_bytes = 0;
    _offheap_table[i].mode = RedjetOffHeapMode::IMMEDIATE_EVICTION;
    _offheap_table[i].registration_tsc = 0;
    _offheap_table[i].is_active.store(false, std::memory_order_relaxed);
  }
}

RedjetJavaAPIBridge::~RedjetJavaAPIBridge() {
  destroy();
}

RedjetJavaAPIBridge& RedjetJavaAPIBridge::instance() {
  return g_redjet_java_api_bridge;
}

void RedjetJavaAPIBridge::init() {
  _tube_feeder.init();
  sweep_offheap_immediate_entries();
}

void RedjetJavaAPIBridge::destroy() {
  _tube_feeder.destroy();
  sweep_offheap_immediate_entries();
}

// =========================================================================
// 1. RedjetTube Streaming API (jdk.gc.redjet.RedjetTube)
// =========================================================================
uint32_t RedjetJavaAPIBridge::open_streaming_tube(const void* array_base, size_t element_count, size_t stride) {
  _total_tube_feeds.fetch_add(1, std::memory_order_relaxed);
  return _tube_feeder.open_channel(array_base, element_count, stride);
}

void RedjetJavaAPIBridge::step_streaming_tube(uint32_t channel_id, size_t chunk_size) {
  _tube_feeder.advance_prefetch(channel_id, chunk_size);
}

void RedjetJavaAPIBridge::close_streaming_tube(uint32_t channel_id) {
  _tube_feeder.close_channel(channel_id);
}

// =========================================================================
// 2. PrecisionTarget Scope API (jdk.gc.redjet.PrecisionScope)
// =========================================================================
void RedjetJavaAPIBridge::enter_precision_scope(bool exact_mode) {
  (void)exact_mode;
  _total_precision_scopes.fetch_add(1, std::memory_order_relaxed);
  hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, 0, exact_mode ? 1 : 0, 0, 0, 0, 0, 0,
                 "PrecisionScopeEntered");
}

void RedjetJavaAPIBridge::exit_precision_scope(bool exact_mode) {
  (void)exact_mode;
  hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, 0, exact_mode ? 1 : 0, 0, 0, 0, 0, 0,
                 "PrecisionScopeExited");
}

// =========================================================================
// 3. BatchExec Scope API (jdk.gc.redjet.RedjetBatchExec)
// =========================================================================
bool RedjetJavaAPIBridge::enter_batch_execution(uint32_t scope_id, const char* name, bool morph, uint32_t prefetch) {
  _total_batch_scopes.fetch_add(1, std::memory_order_relaxed);
  return RedjetBREE::instance().enter_batch_scope(scope_id, name, morph, prefetch);
}

bool RedjetJavaAPIBridge::exit_batch_execution(uint32_t scope_id) {
  return RedjetBREE::instance().exit_batch_scope(scope_id);
}

// =========================================================================
// 4. Explicit GC Hint API (jdk.gc.redjet.JDK.ThrowGCHint)
// =========================================================================
void RedjetJavaAPIBridge::on_explicit_gc_hint(uint32_t hint_flags) {
  _total_gc_hints_handled.fetch_add(1, std::memory_order_relaxed);

  // Invoke non-STW explicit cleaning hints
  RedjetExplicitGC::request_explicit_gc(hint_flags);

  hs_hotlog_emit(HS_Ebin0_HOOK, HS_Ebin1_INFO, hint_flags, 0, 0, 0, 0, 0, 0,
                 "ExplicitGCHintHandled");
}

// =========================================================================
// 5. @RJGC Off-Heap Management (jdk.gc.annotation.RJGC)
// =========================================================================
bool RedjetJavaAPIBridge::register_offheap_memory(void* ptr, size_t size_bytes, RedjetOffHeapMode mode) {
  if (ptr == nullptr || size_bytes == 0) return false;

  hs_ticket_guard_t guard(_bridge_lock);
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

  for (size_t i = 0; i < MAX_OFFHEAP_TRACKED; ++i) {
    if (!_offheap_table[i].is_active.load(std::memory_order_relaxed)) {
      _offheap_table[i].address = addr;
      _offheap_table[i].size_bytes = size_bytes;
      _offheap_table[i].mode = mode;
      _offheap_table[i].registration_tsc = sacculus_rdtsc();
      _offheap_table[i].is_active.store(true, std::memory_order_release);

      if (mode == RedjetOffHeapMode::IMMEDIATE_EVICTION) {
        _total_offheap_immediate_freed.fetch_add(1, std::memory_order_relaxed);
      } else {
        _total_offheap_flow_watched.fetch_add(1, std::memory_order_relaxed);
      }
      return true;
    }
  }

  return false; // Table full
}

bool RedjetJavaAPIBridge::release_offheap_memory(void* ptr) {
  if (ptr == nullptr) return false;

  hs_ticket_guard_t guard(_bridge_lock);
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

  for (size_t i = 0; i < MAX_OFFHEAP_TRACKED; ++i) {
    if (_offheap_table[i].is_active.load(std::memory_order_relaxed) &&
        _offheap_table[i].address == addr) {
      _offheap_table[i].is_active.store(false, std::memory_order_release);
      _offheap_table[i].address = 0;
      _offheap_table[i].size_bytes = 0;
      return true;
    }
  }

  return false;
}

size_t RedjetJavaAPIBridge::sweep_offheap_immediate_entries() {
  hs_ticket_guard_t guard(_bridge_lock);
  size_t swept = 0;

  for (size_t i = 0; i < MAX_OFFHEAP_TRACKED; ++i) {
    if (_offheap_table[i].is_active.load(std::memory_order_relaxed) &&
        _offheap_table[i].mode == RedjetOffHeapMode::IMMEDIATE_EVICTION) {
      _offheap_table[i].is_active.store(false, std::memory_order_release);
      _offheap_table[i].address = 0;
      _offheap_table[i].size_bytes = 0;
      swept++;
    }
  }

  return swept;
}

void RedjetJavaAPIBridge::register_natives(JNIEnv* env) {
  (void)env;
  // JNI registration hook for jdk.gc.redjet package classes
  hs_hotlog_emit(HS_Ebin0_HOOK, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0, 0,
                 "RedjetJavaAPINativesRegistered");
}

void RedjetJavaAPIBridge::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet Java API Bridge (jdk.gc.redjet.* Runtime):");
  st->print_cr("  Total Tube Feeds:           " UINT64_FORMAT, total_tube_feeds());
  st->print_cr("  Total Precision Scopes:     " UINT64_FORMAT, total_precision_scopes());
  st->print_cr("  Total Batch Scopes:         " UINT64_FORMAT, total_batch_scopes());
  st->print_cr("  Total Explicit GC Hints:    " UINT64_FORMAT, total_gc_hints());
  st->print_cr("  Off-Heap Immediate Evicted: " UINT64_FORMAT, total_offheap_immediate_freed());
  st->print_cr("  Off-Heap Flow Watched:      " UINT64_FORMAT, total_offheap_flow_watched());
}
