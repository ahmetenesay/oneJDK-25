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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETREFERENCEPROCESSOR_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETREFERENCEPROCESSOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <algorithm>
#include <vector>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/referenceDiscoverer.hpp"
#include "gc/shared/referencePolicy.hpp"
#include "gc/shared/referenceProcessorStats.hpp"
#include "gc/shared/workerThread.hpp"
#include "oops/oopsHierarchy.hpp"
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#else
enum ReferenceType {
  REF_NONE,
  REF_OTHER,
  REF_SOFT,
  REF_WEAK,
  REF_FINAL,
  REF_PHANTOM
};

class ReferenceDiscoverer {
public:
  virtual bool discover_reference(void* obj, ReferenceType type) = 0;
};

struct ReferenceProcessorStats {
  size_t _soft_count{0};
  size_t _weak_count{0};
  size_t _final_count{0};
  size_t _phantom_count{0};
};

typedef void* oop;
class WorkerThreads {};
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;
class RedjetMarkBitMap;

// ============================================================================
// 1. THREAD-LOCAL REFERENCE DISCOVERY RING BUFFER
// 256-slot static ring buffer to eliminate allocation overhead during discovery
// ============================================================================
struct alignas(64) RedjetRefDiscoveryRing {
  static constexpr size_t RING_CAPACITY = 256;
  static constexpr size_t RING_MASK     = RING_CAPACITY - 1;

  oop                   _buffer[RING_CAPACITY];
  alignas(64) std::atomic<uint64_t> _head{ 0 };
  alignas(64) std::atomic<uint64_t> _tail{ 0 };
  std::atomic<uint64_t> _overflow_count{ 0 };

  void reset() noexcept {
    _head.store(0, std::memory_order_relaxed);
    _tail.store(0, std::memory_order_relaxed);
    _overflow_count.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < RING_CAPACITY; ++i) {
      _buffer[i] = nullptr;
    }
  }

  bool push(oop ref) noexcept {
    uint64_t t = _tail.load(std::memory_order_relaxed);
    uint64_t h = _head.load(std::memory_order_acquire);
    if (t - h >= RING_CAPACITY) {
      _overflow_count.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _buffer[t & RING_MASK] = ref;
    _tail.store(t + 1, std::memory_order_release);
    return true;
  }

  bool pop(oop* out_ref) noexcept {
    uint64_t h = _head.load(std::memory_order_relaxed);
    uint64_t t = _tail.load(std::memory_order_acquire);
    if (h >= t) {
      return false;
    }
    *out_ref = _buffer[h & RING_MASK];
    _head.store(h + 1, std::memory_order_release);
    return true;
  }

  size_t count() const noexcept {
    uint64_t t = _tail.load(std::memory_order_relaxed);
    uint64_t h = _head.load(std::memory_order_relaxed);
    return (t >= h) ? static_cast<size_t>(t - h) : 0;
  }
};

// ============================================================================
// 2. THREAD-LOCAL REFERENCE PROCESSING DATA (Cacheline Aligned)
// ============================================================================
struct alignas(64) RedjetRefProcThreadLocal {
  oop    _discovered_soft_head{ nullptr };
  oop    _discovered_weak_head{ nullptr };
  oop    _discovered_final_head{ nullptr };
  oop    _discovered_phantom_head{ nullptr };

  RedjetRefDiscoveryRing _discovery_ring;

  size_t _discovered_counts[5]{ 0, 0, 0, 0, 0 };
  size_t _enqueued_counts[5]{ 0, 0, 0, 0, 0 };
  size_t _cleared_counts[5]{ 0, 0, 0, 0, 0 };
  size_t _retained_counts[5]{ 0, 0, 0, 0, 0 };
  size_t _resurrected_finals{ 0 };

  void reset() noexcept {
    _discovered_soft_head    = nullptr;
    _discovered_weak_head    = nullptr;
    _discovered_final_head   = nullptr;
    _discovered_phantom_head = nullptr;
    _discovery_ring.reset();
    _resurrected_finals      = 0;
    for (int i = 0; i < 5; ++i) {
      _discovered_counts[i] = 0;
      _enqueued_counts[i]   = 0;
      _cleared_counts[i]    = 0;
      _retained_counts[i]   = 0;
    }
  }
};

// ============================================================================
// 3. DYNAMIC SOFT REFERENCE AGING CLOCK
// Adapts soft reference retention based on free RAM ratio, allocation rate,
// and GC pause history using an adaptive sigmoid damping curve.
// ============================================================================
class RedjetDynamicSoftRefClock {
private:
  jlong    _timestamp_ms{ 0 };
  uint64_t _ms_per_mb{ 1000 };
  double   _free_ram_ratio{ 1.0 };
  double   _allocation_pressure_factor{ 1.0 };
  uint64_t _target_retention_window_ms{ 60000 };
  uint64_t _clock_epoch{ 0 };

  std::atomic<uint64_t> _soft_references_cleared{ 0 };
  std::atomic<uint64_t> _soft_references_retained{ 0 };
  std::atomic<uint64_t> _soft_clock_ticks{ 0 };

public:
  RedjetDynamicSoftRefClock();

  void update_clock(size_t free_ram_bytes, size_t total_ram_bytes, double alloc_mbps = 0.0);
  bool should_clear_soft_reference(jlong clock_timestamp, jlong ref_timestamp, bool clear_all);

  uint64_t ms_per_mb()                  const noexcept { return _ms_per_mb; }
  double   free_ram_ratio()            const noexcept { return _free_ram_ratio; }
  double   allocation_pressure_factor()const noexcept { return _allocation_pressure_factor; }
  uint64_t target_retention_window_ms() const noexcept { return _target_retention_window_ms; }
  uint64_t clock_epoch()                const noexcept { return _clock_epoch; }
  jlong    timestamp_ms()               const noexcept { return _timestamp_ms; }

  uint64_t soft_references_cleared()  const noexcept { return _soft_references_cleared.load(std::memory_order_relaxed); }
  uint64_t soft_references_retained() const noexcept { return _soft_references_retained.load(std::memory_order_relaxed); }
  uint64_t soft_clock_ticks()         const noexcept { return _soft_clock_ticks.load(std::memory_order_relaxed); }
};

// ============================================================================
// 4. @RJGC OFF-HEAP REGISTRATION ENTRY
// Tracks native memory segments and off-heap direct buffers pinned by Java refs.
// ============================================================================
struct RedjetOffHeapEntry {
  uintptr_t native_address{ 0 };
  size_t    size_bytes{ 0 };
  uint32_t  flags{ 0 };
  uint64_t  registration_epoch{ 0 };
  oop       weak_owner_ref{ nullptr };
};

// ============================================================================
// 5. REDJET REFERENCE PROCESSOR (PLAN.md Section 8.10.1)
// HotSpot Standard GC Parity: Full reference processing lifecycle
// (SoftReference, WeakReference, Finalizer, PhantomReference), dynamic aging,
// JNI Weak Global handles unlinking, and @RJGC off-heap synchronization.
// ============================================================================
class RedjetReferenceProcessor : public ReferenceDiscoverer {
public:
  static constexpr uint32_t MAX_WORKERS = 128;
  static constexpr size_t   MAX_OFFHEAP_ENTRIES = 4096;

private:
  RedjetHeap*                 _heap;
  RedjetMarkBitMap*           _bitmap;
  uint32_t                    _active_workers;
  RedjetRefProcThreadLocal    _worker_data[MAX_WORKERS];
  RedjetDynamicSoftRefClock   _soft_clock;

  oop                         _pending_list_head;
  std::atomic<bool>           _discovery_enabled{ false };

  // Off-heap registry
  RedjetOffHeapEntry          _offheap_table[MAX_OFFHEAP_ENTRIES];
  std::atomic<size_t>         _offheap_count{ 0 };

  // Telemetry & stats
  ReferenceProcessorStats     _stats;
  std::atomic<uint64_t>       _total_references_processed{ 0 };
  std::atomic<uint64_t>       _total_jni_weak_unlinked{ 0 };
  std::atomic<uint64_t>       _total_rjgc_offheap_synced{ 0 };
  std::atomic<uint64_t>       _total_vm_weak_unlinked{ 0 };
  std::atomic<uint64_t>       _total_resurrected_finals{ 0 };
  std::atomic<uint64_t>       _total_processing_cycles{ 0 };
  std::atomic<uint64_t>       _total_processing_time_us{ 0 };
  std::atomic<uint64_t>       _transitive_resurrection_marks{ 0 };

  // Core processing steps
  void process_soft_references(bool clear_all);
  void process_weak_references();
  void process_final_references();
  void process_phantom_references();
  void enqueue_references();

  // Transitive closure tracing for resurrected final references
  void trace_resurrected_referent(oop obj);

  // Low-level oop inspect/manipulate
  bool is_referent_alive(oop obj) const;
  oop  referent(oop ref_obj) const;
  void clear_referent(oop ref_obj);
  oop  next_discovered(oop ref_obj) const;
  void set_next_discovered(oop ref_obj, oop next);

  // Unlink JVM internal weak tables (StringTable, SymbolTable, ResolvedMethodTable)
  size_t unlink_vm_weak_tables();

public:
  RedjetReferenceProcessor(RedjetHeap* heap, RedjetMarkBitMap* bitmap, uint32_t active_workers = 8);
  virtual ~RedjetReferenceProcessor() = default;

  // ReferenceDiscoverer override
  bool discover_reference(oop obj, ReferenceType type) override;

  // Lifecycle control
  void enable_discovery();
  void disable_discovery();
  bool is_discovery_enabled() const noexcept { return _discovery_enabled.load(std::memory_order_relaxed); }

  // Execute full standard reference processing phase
  void process_discovered_references(bool clear_all_soft_refs, WorkerThreads* workers = nullptr);

  // Unlink JNI Weak Global handles & VM weak roots
  size_t unlink_jni_weak_roots();

  // Synchronize with @RJGC off-heap smart cleaner and EmptyBox caches
  size_t sync_with_rjgc_offheap();

  // Register native buffer with @RJGC tracking table
  bool register_offheap_segment(uintptr_t addr, size_t size, oop owner);

  // Clock update
  void update_soft_reference_clock(size_t free_ram_bytes, size_t total_ram_bytes, double alloc_mbps = 0.0) {
    _soft_clock.update_clock(free_ram_bytes, total_ram_bytes, alloc_mbps);
  }

  const ReferenceProcessorStats& reference_process_stats() const noexcept { return _stats; }
  const RedjetDynamicSoftRefClock& soft_clock() const noexcept { return _soft_clock; }

  uint64_t total_references_processed()    const noexcept { return _total_references_processed.load(std::memory_order_relaxed); }
  uint64_t total_jni_weak_unlinked()       const noexcept { return _total_jni_weak_unlinked.load(std::memory_order_relaxed); }
  uint64_t total_rjgc_offheap_synced()     const noexcept { return _total_rjgc_offheap_synced.load(std::memory_order_relaxed); }
  uint64_t total_vm_weak_unlinked()        const noexcept { return _total_vm_weak_unlinked.load(std::memory_order_relaxed); }
  uint64_t total_resurrected_finals()      const noexcept { return _total_resurrected_finals.load(std::memory_order_relaxed); }
  uint64_t total_processing_cycles()       const noexcept { return _total_processing_cycles.load(std::memory_order_relaxed); }
  uint64_t total_processing_time_us()      const noexcept { return _total_processing_time_us.load(std::memory_order_relaxed); }
  uint64_t transitive_resurrection_marks() const noexcept { return _transitive_resurrection_marks.load(std::memory_order_relaxed); }
  size_t   offheap_tracked_count()         const noexcept { return _offheap_count.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETREFERENCEPROCESSOR_HPP
