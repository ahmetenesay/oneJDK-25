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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETGCMEMORYMANAGER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETGCMEMORYMANAGER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "services/memoryManager.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcCause.hpp"
#include "utilities/ticks.hpp"
#include "utilities/ostream.hpp"
#else
class GCMemoryManager {
public:
  GCMemoryManager(const char* name) {}
  void add_pool(void* pool) {}
};
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
namespace GCCause {
  enum Cause { _no_gc, _java_lang_system_gc, _full_gc_alot, _allocation_failure, _redjet_seasonal_shift };
  inline const char* to_string(Cause c) { return "RedjetGC"; }
}
class Ticks {
private:
  uint64_t _val{ 0 };
public:
  static Ticks now() { return Ticks(); }
};
class Tickspan {
public:
  uint64_t milliseconds() const { return 1; }
};
#endif

class RedjetHeap;
class RedjetUnifiedHeapPool;
class RedjetRunwayPool;
class RedjetHotfieldPool;
class RedjetLandfillPool;

// ============================================================================
// REDJET GC MEMORY MANAGER (PLAN.md Section 8.10.5)
// JMX Management API, MemoryPool MXBeans, JVM TI, GCId, GCTracer,
// and JFR events (EventGarbageCollection, EventGCPhasePause, EventOldGarbageCollection).
// ============================================================================
class RedjetGCMemoryManager : public GCMemoryManager {
private:
  RedjetHeap*             _heap;
  RedjetUnifiedHeapPool*  _pool_unified;
  RedjetRunwayPool*       _pool_runway;
  RedjetHotfieldPool*     _pool_hotfield;
  RedjetLandfillPool*     _pool_landfill;

  std::atomic<uint64_t>   _gc_id_counter{ 0 };
  std::atomic<uint64_t>   _total_collections{ 0 };
  std::atomic<uint64_t>   _total_pause_time_us{ 0 };
  std::atomic<uint64_t>   _longest_pause_time_us{ 0 };
  std::atomic<uint64_t>   _current_gc_start_tsc{ 0 };
  std::atomic<uint64_t>   _phase_start_tsc{ 0 };

#ifndef REDJET_STANDALONE_BUILD
  Ticks                   _current_gc_start_ticks;
  Ticks                   _phase_start_ticks;
#endif
  GCCause::Cause          _current_cause{ GCCause::_no_gc };

  // Subphase pause metrics
  std::atomic<uint64_t>   _total_subphase_pauses{ 0 };
  std::atomic<uint64_t>   _subphase_pause_time_us{ 0 };

public:
  explicit RedjetGCMemoryManager(RedjetHeap* heap);
  virtual ~RedjetGCMemoryManager() = default;

  // Initialize and register all 4 Redjet memory pools
  void initialize_pools();

  // Lifecycle hooks for GC cycle tracking and JFR events
  void gc_begin(GCCause::Cause cause);
  void gc_end();

  // Sub-phase pause instrumentation for JFR EventGCPhasePause
  void phase_pause_begin(const char* phase_name);
  void phase_pause_end(const char* phase_name);

  // Pool accessors
  RedjetUnifiedHeapPool* pool_unified()  const noexcept { return _pool_unified; }
  RedjetRunwayPool*      pool_runway()   const noexcept { return _pool_runway; }
  RedjetHotfieldPool*    pool_hotfield() const noexcept { return _pool_hotfield; }
  RedjetLandfillPool*    pool_landfill() const noexcept { return _pool_landfill; }

  // Telemetry accessors
  uint64_t total_collections()     const noexcept { return _total_collections.load(std::memory_order_relaxed); }
  uint64_t total_pause_time_us()   const noexcept { return _total_pause_time_us.load(std::memory_order_relaxed); }
  uint64_t longest_pause_time_us() const noexcept { return _longest_pause_time_us.load(std::memory_order_relaxed); }
  uint64_t current_gc_id()         const noexcept { return _gc_id_counter.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETGCMEMORYMANAGER_HPP
