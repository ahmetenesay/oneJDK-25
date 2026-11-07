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

#ifndef SHARE_GC_REDJET_REDJETROOTSCANNER_HPP
#define SHARE_GC_REDJET_REDJETROOTSCANNER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <cstring>
#include "utilities/macros.hpp"
#include "gc/redjet/simd/redjetMarkBitMap.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "memory/iterator.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/javaThread.hpp"
#include "code/codeBlob.hpp"
#include "classfile/classLoaderData.hpp"
#include "utilities/ostream.hpp"
#else
class OopClosure {};
class CLDClosure {};
class CodeBlobClosure {};
typedef void* oop;
typedef void* narrowOop;
class ClassLoaderData {};
class CodeBlob {};
class JavaThread {};
class Thread {};
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;
class RedjetPointCoordinator;

// ============================================================================
// 1. ROOT SCAN PHASE IDENTIFIERS (11 Universal VM & Thread Phases)
// ============================================================================
enum class RedjetRootPhase : uint8_t {
  THREAD_STACKS     = 0,
  JNI_HANDLES       = 1,
  VM_GLOBAL         = 2,
  JVMTI_HANDLES     = 3,
  SYSTEM_DICTIONARY = 4,
  UNIVERSE          = 5,
  MANAGEMENT        = 6,
  CODE_BLOBS        = 7,
  AOT_ROOTS         = 8,
  STRING_TABLE      = 9,
  OBJECT_MONITORS   = 10,
  COUNT             = 11
};

// ============================================================================
// 2. PER-PHASE ROOT SCAN DETAILED STATISTICS
// ============================================================================
struct RedjetRootPhaseStats {
  size_t   roots_found{ 0 };
  size_t   objects_marked{ 0 };
  uint64_t elapsed_us{ 0 };
  uint64_t tsc_cycles{ 0 };
  uint64_t parkpoint_checks{ 0 };
  uint64_t dockpoint_yields{ 0 };
  size_t   cache_lines_dirtied{ 0 };
  size_t   deep_pointers_discovered{ 0 };
};

// ============================================================================
// 3. THREAD STACK CHUNK SLICER FOR DOMESTIC PARALLELISM
// Partitions Java thread stack execution frames into cacheline-aligned slices
// to prevent L1/L2 cacheline ping-pong between Domestic worker cores.
// ============================================================================
struct alignas(64) RedjetThreadStackSlice {
  static constexpr size_t MAX_THREADS_PER_SLICE = 64;

  JavaThread* threads[MAX_THREADS_PER_SLICE]{ nullptr };
  size_t      thread_count{ 0 };
  size_t      total_frames_visited{ 0 };
  size_t      oops_extracted{ 0 };
  uint64_t    slice_duration_tsc{ 0 };

  void reset() noexcept {
    for (size_t i = 0; i < MAX_THREADS_PER_SLICE; ++i) {
      threads[i] = nullptr;
    }
    thread_count = 0;
    total_frames_visited = 0;
    oops_extracted = 0;
    slice_duration_tsc = 0;
  }

  bool add_thread(JavaThread* jt) noexcept {
    if (thread_count < MAX_THREADS_PER_SLICE) {
      threads[thread_count++] = jt;
      return true;
    }
    return false;
  }
};

// ============================================================================
// 4. TRANSITIVE CLOSURE WORKLIST RING BUFFER
// Fixed-size, cacheline-padded ring buffer per worker thread for 0-lock tracing.
// ============================================================================
class alignas(64) RedjetTransitiveRingBuffer {
public:
  static constexpr size_t RING_CAPACITY = 16384;
  static constexpr size_t RING_MASK     = RING_CAPACITY - 1;

private:
  oop                   _ring[RING_CAPACITY];
  alignas(64) std::atomic<uint64_t> _head{ 0 };
  alignas(64) std::atomic<uint64_t> _tail{ 0 };
  std::atomic<uint64_t> _overflow_count{ 0 };
  std::atomic<uint64_t> _underflow_count{ 0 };

public:
  RedjetTransitiveRingBuffer() noexcept {
    reset();
  }

  void reset() noexcept {
    _head.store(0, std::memory_order_relaxed);
    _tail.store(0, std::memory_order_relaxed);
    _overflow_count.store(0, std::memory_order_relaxed);
    _underflow_count.store(0, std::memory_order_relaxed);
    std::memset(_ring, 0, sizeof(_ring));
  }

  inline bool push(oop obj) noexcept {
    uint64_t t = _tail.load(std::memory_order_relaxed);
    uint64_t h = _head.load(std::memory_order_acquire);
    if (t - h >= RING_CAPACITY) {
      _overflow_count.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _ring[t & RING_MASK] = obj;
    _tail.store(t + 1, std::memory_order_release);
    return true;
  }

  inline bool pop(oop* out_obj) noexcept {
    uint64_t h = _head.load(std::memory_order_relaxed);
    uint64_t t = _tail.load(std::memory_order_acquire);
    if (h >= t) {
      _underflow_count.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    *out_obj = _ring[h & RING_MASK];
    _ring[h & RING_MASK] = nullptr;
    _head.store(h + 1, std::memory_order_release);
    return true;
  }

  inline bool is_empty() const noexcept {
    return _head.load(std::memory_order_relaxed) >= _tail.load(std::memory_order_relaxed);
  }

  inline size_t size() const noexcept {
    uint64_t t = _tail.load(std::memory_order_relaxed);
    uint64_t h = _head.load(std::memory_order_relaxed);
    return (t >= h) ? static_cast<size_t>(t - h) : 0;
  }

  uint64_t overflow_count()  const noexcept { return _overflow_count.load(std::memory_order_relaxed); }
  uint64_t underflow_count() const noexcept { return _underflow_count.load(std::memory_order_relaxed); }
};

// ============================================================================
// 5. INTER-REGION CARD TABLE SCANNER & MARKING ASSISTANT
// Analyzes cross-slab and cross-generation pointer densities during root tracing.
// ============================================================================
class RedjetRootCardScanner {
private:
  RedjetHeap*           _heap;
  std::atomic<uint64_t> _dirty_cards_scanned{ 0 };
  std::atomic<uint64_t> _cross_slab_ptrs_found{ 0 };
  std::atomic<uint64_t> _cards_cleared{ 0 };

public:
  explicit RedjetRootCardScanner(RedjetHeap* heap) noexcept : _heap(heap) {}

  void scan_dirty_cards(OopClosure* oop_cl);
  void clear_processed_cards();

  uint64_t dirty_cards_scanned()   const noexcept { return _dirty_cards_scanned.load(std::memory_order_relaxed); }
  uint64_t cross_slab_ptrs_found() const noexcept { return _cross_slab_ptrs_found.load(std::memory_order_relaxed); }
  uint64_t cards_cleared()         const noexcept { return _cards_cleared.load(std::memory_order_relaxed); }
};

// ============================================================================
// 6. REDJET ROOT SCANNER (PLAN.md Section 8.10.4)
// Universal 2-Points compliant root scanning closures (OopIterateClosure,
// CLDToOopClosure, Thread stacks, JNI handles, JVM TI, SystemDictionary,
// Universe, CodeBlobs, AOT, Management) with transitive tracing, 2-Points sync,
// and domestic worker parallel slicing.
// ============================================================================
class RedjetRootScanner {
public:
  static constexpr uint32_t MAX_WORKERS = 128;

private:
  RedjetHeap*                 _heap;
  RedjetMarkBitMap*           _bitmap;
  uint32_t                    _active_workers;
  size_t                      _roots_scanned;
  size_t                      _objects_marked;
  RedjetRootPhaseStats        _phase_stats[static_cast<size_t>(RedjetRootPhase::COUNT)];
  RedjetTransitiveRingBuffer  _worker_rings[MAX_WORKERS];
  RedjetRootCardScanner       _card_scanner;

  // 2-Points cooperative yield & synchronization counters
  std::atomic<uint64_t> _parkpoint_yields{ 0 };
  std::atomic<uint64_t> _dockpoint_checks{ 0 };
  std::atomic<uint64_t> _transitive_nodes_visited{ 0 };
  std::atomic<uint64_t> _transitive_spill_nodes{ 0 };
  std::atomic<uint64_t> _simd_prefetch_triggers{ 0 };
  std::atomic<uint64_t> _total_scan_duration_tsc{ 0 };

  // Internal phase scanners (11 Phases)
  void scan_thread_stacks(OopClosure* oop_cl, CodeBlobClosure* code_cl, uint32_t worker_id);
  void scan_jni_handles(OopClosure* oop_cl);
  void scan_vm_global(OopClosure* oop_cl);
  void scan_jvmti_handles(OopClosure* oop_cl);
  void scan_system_dictionary(OopClosure* oop_cl);
  void scan_universe_roots(OopClosure* oop_cl, CLDClosure* cld_cl);
  void scan_management_roots(OopClosure* oop_cl);
  void scan_code_blobs(CodeBlobClosure* code_cl);
  void scan_aot_roots(OopClosure* oop_cl);
  void scan_string_table(OopClosure* oop_cl);
  void scan_object_monitors(OopClosure* oop_cl);

  // Segmented parallel stack scanning partitioner
  void partition_and_scan_stacks(OopClosure* oop_cl, CodeBlobClosure* code_cl);

  // Transitive closure worklist drain with 2-Points cooperative checks
  void drain_worklist_transitive(std::vector<oop>& spill_worklist, OopClosure* oop_cl, uint32_t worker_id);

  // Cooperative 2-Points polling kernel
  inline void check_2points_cooperative(uint32_t& local_counter);

public:
  RedjetRootScanner(RedjetHeap* heap, RedjetMarkBitMap* bitmap, uint32_t active_workers = 8);
  ~RedjetRootScanner() = default;

  // Scan all HotSpot VM & Thread roots and trace live reachable graph (2-Points compliant)
  size_t scan_roots_and_trace();

  // Root iteration without marking (e.g. for verification, heap inspection)
  void roots_do(OopClosure* oop_cl, CLDClosure* cld_cl = nullptr, CodeBlobClosure* code_cl = nullptr);

  // Telemetry & metrics accessors
  size_t   roots_scanned()             const noexcept { return _roots_scanned; }
  size_t   objects_marked()            const noexcept { return _objects_marked; }
  uint64_t parkpoint_yields()          const noexcept { return _parkpoint_yields.load(std::memory_order_relaxed); }
  uint64_t dockpoint_checks()          const noexcept { return _dockpoint_checks.load(std::memory_order_relaxed); }
  uint64_t transitive_nodes_visited()  const noexcept { return _transitive_nodes_visited.load(std::memory_order_relaxed); }
  uint64_t transitive_spill_nodes()    const noexcept { return _transitive_spill_nodes.load(std::memory_order_relaxed); }
  uint64_t simd_prefetch_triggers()    const noexcept { return _simd_prefetch_triggers.load(std::memory_order_relaxed); }
  uint64_t total_scan_duration_tsc()   const noexcept { return _total_scan_duration_tsc.load(std::memory_order_relaxed); }
  uint32_t active_workers()            const noexcept { return _active_workers; }

  const RedjetRootPhaseStats& phase_stats(RedjetRootPhase phase) const noexcept {
    return _phase_stats[static_cast<size_t>(phase)];
  }

  const RedjetCardTableScanner& card_scanner() const noexcept { return _card_scanner; }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_REDJETROOTSCANNER_HPP
