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

#include "gc/redjet/runtime/redjetGCMemoryManager.hpp"
#include "gc/redjet/runtime/redjetMemoryPool.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include <chrono>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <immintrin.h>
#include <intrin.h>
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <x86intrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

static inline uint64_t rj_manager_read_tsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
  return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t val;
  asm volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#else
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

RedjetGCMemoryManager::RedjetGCMemoryManager(RedjetHeap* heap)
  : GCMemoryManager("Redjet GC Memory Manager"),
    _heap(heap),
    _pool_unified(nullptr),
    _pool_runway(nullptr),
    _pool_hotfield(nullptr),
    _pool_landfill(nullptr),
    _gc_id_counter(0),
    _total_collections(0),
    _total_pause_time_us(0),
    _longest_pause_time_us(0),
    _current_gc_start_tsc(0),
    _current_cause(GCCause::_no_gc),
    _total_subphase_pauses(0),
    _subphase_pause_time_us(0) {
}

void RedjetGCMemoryManager::initialize_pools() {
  _pool_unified  = new RedjetUnifiedHeapPool(_heap);
  _pool_runway   = new RedjetRunwayPool(_heap);
  _pool_hotfield = new RedjetHotfieldPool(_heap);
  _pool_landfill = new RedjetLandfillPool(_heap);

  add_pool(_pool_unified);
  add_pool(_pool_runway);
  add_pool(_pool_hotfield);
  add_pool(_pool_landfill);
}

void RedjetGCMemoryManager::gc_begin(GCCause::Cause cause) {
  _current_cause = cause;
  _current_gc_start_tsc.store(rj_manager_read_tsc(), std::memory_order_relaxed);

#ifndef REDJET_STANDALONE_BUILD
  _current_gc_start_ticks = Ticks::now();
  uint32_t gc_id = GCId::create();
  _gc_id_counter.store(gc_id, std::memory_order_relaxed);
#else
  _gc_id_counter.fetch_add(1, std::memory_order_relaxed);
#endif
}

void RedjetGCMemoryManager::gc_end() {
  uint64_t end_tsc = rj_manager_read_tsc();
  uint64_t start_tsc = _current_gc_start_tsc.load(std::memory_order_relaxed);
  uint64_t elapsed_cycles = (end_tsc > start_tsc) ? (end_tsc - start_tsc) : 0;
  uint64_t elapsed_us = elapsed_cycles / 3000; // ~3.0 GHz microsecond tick arithmetic
  if (elapsed_us == 0 && elapsed_cycles > 0) {
    elapsed_us = 1;
  }

#ifndef REDJET_STANDALONE_BUILD
  Ticks end_ticks = Ticks::now();
  Tickspan duration = end_ticks - _current_gc_start_ticks;

  // Publish JFR EventGarbageCollection
  EventGarbageCollection event;
  if (event.should_commit()) {
    event.set_gcId(_gc_id_counter.load(std::memory_order_relaxed));
    event.set_name("RedjetGC");
    event.set_cause(static_cast<u2>(_current_cause));
    event.set_sumOfPauses(duration);
    event.set_longestPause(duration);
    event.commit();
  }
#endif

  _total_collections.fetch_add(1, std::memory_order_relaxed);
  _total_pause_time_us.fetch_add(elapsed_us, std::memory_order_relaxed);

  // Update longest pause
  uint64_t cur_longest = _longest_pause_time_us.load(std::memory_order_relaxed);
  while (elapsed_us > cur_longest &&
         !_longest_pause_time_us.compare_exchange_weak(cur_longest, elapsed_us, std::memory_order_relaxed)) {
  }

  log_info(gc)("Redjet GC Cycle #%llu finished in %llu us (longest=%llu us)",
               (unsigned long long)_total_collections.load(std::memory_order_relaxed),
               (unsigned long long)elapsed_us,
               (unsigned long long)_longest_pause_time_us.load(std::memory_order_relaxed));
}

void RedjetGCMemoryManager::phase_pause_begin(const char* phase_name) {
  _phase_start_tsc.store(rj_manager_read_tsc(), std::memory_order_relaxed);
#ifndef REDJET_STANDALONE_BUILD
  _phase_start_ticks = Ticks::now();
#endif
}

void RedjetGCMemoryManager::phase_pause_end(const char* phase_name) {
  uint64_t end_tsc = rj_manager_read_tsc();
  uint64_t start_tsc = _phase_start_tsc.load(std::memory_order_relaxed);
  uint64_t phase_cycles = (end_tsc > start_tsc) ? (end_tsc - start_tsc) : 0;
  uint64_t phase_us = phase_cycles / 3000;
  if (phase_us == 0 && phase_cycles > 0) {
    phase_us = 1;
  }

#ifndef REDJET_STANDALONE_BUILD
  Ticks end_ticks = Ticks::now();
  Tickspan duration = end_ticks - _phase_start_ticks;

  // Publish JFR EventGCPhasePause
  EventGCPhasePause event;
  if (event.should_commit()) {
    event.set_gcId(_gc_id_counter.load(std::memory_order_relaxed));
    event.set_name(phase_name);
    event.set_duration(duration);
    event.commit();
  }
#endif

  _total_subphase_pauses.fetch_add(1, std::memory_order_relaxed);
  _subphase_pause_time_us.fetch_add(phase_us, std::memory_order_relaxed);
}

void RedjetGCMemoryManager::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet GC Memory Manager ---");
  st->print_cr("  Total GC Cycles:         %llu", (unsigned long long)_total_collections.load(std::memory_order_relaxed));
  st->print_cr("  Total Pause Time:        %llu us", (unsigned long long)_total_pause_time_us.load(std::memory_order_relaxed));
  st->print_cr("  Longest Pause Time:      %llu us", (unsigned long long)_longest_pause_time_us.load(std::memory_order_relaxed));
  st->print_cr("  Subphase Pauses:         %llu (Total: %llu us)",
               (unsigned long long)_total_subphase_pauses.load(std::memory_order_relaxed),
               (unsigned long long)_subphase_pause_time_us.load(std::memory_order_relaxed));
  st->print_cr("  Current Cause:           %s", GCCause::to_string(_current_cause));
}
