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

#include "gc/redjet/runtime/redjetReferenceProcessor.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/simd/redjetMarkBitMap.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/api/redjetOffHeapReclaimer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifndef REDJET_STANDALONE_BUILD
#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/oopStorageSetParState.inline.hpp"
#include "logging/log.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/instanceRefKlass.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#define log_trace(...) if (false) printf
#endif

// ============================================================================
// RedjetDynamicSoftRefClock Implementation
// ============================================================================

RedjetDynamicSoftRefClock::RedjetDynamicSoftRefClock()
  : _timestamp_ms(0),
    _ms_per_mb(1000),
    _free_ram_ratio(1.0),
    _allocation_pressure_factor(1.0),
    _target_retention_window_ms(60000),
    _clock_epoch(0),
    _soft_references_cleared(0),
    _soft_references_retained(0),
    _soft_clock_ticks(0) {
}

void RedjetDynamicSoftRefClock::update_clock(size_t free_ram_bytes, size_t total_ram_bytes, double alloc_mbps) {
  _soft_clock_ticks.fetch_add(1, std::memory_order_relaxed);
  _clock_epoch++;

  if (total_ram_bytes > 0) {
    _free_ram_ratio = static_cast<double>(free_ram_bytes) / static_cast<double>(total_ram_bytes);
    if (_free_ram_ratio > 1.0) _free_ram_ratio = 1.0;
    if (_free_ram_ratio < 0.0) _free_ram_ratio = 0.0;
  } else {
    _free_ram_ratio = 1.0;
  }

  // Allocation pressure damping factor (500 MB/s baseline saturation)
  if (alloc_mbps > 0.0) {
    _allocation_pressure_factor = 1.0 + (alloc_mbps / 500.0);
  } else {
    _allocation_pressure_factor = 1.0;
  }

  // Sigmoid adaptive curve for soft reference retention:
  // When free RAM is high (> 60%), retain soft references generously (up to 8000 ms/MB).
  // When free RAM enters the warning zone (< 20%), clamp down exponentially to 50 ms/MB to prevent OOM.
  double sigmoid_arg = 10.0 * (_free_ram_ratio - 0.40);
  double sigmoid_val = 1.0 / (1.0 + std::exp(-sigmoid_arg)); // Range: (0, 1)

  double base_ms = 100.0 + (7900.0 * sigmoid_val);
  double damped_ms = base_ms / _allocation_pressure_factor;

  if (damped_ms < 50.0)    damped_ms = 50.0;
  if (damped_ms > 10000.0) damped_ms = 10000.0;

  _ms_per_mb = static_cast<uint64_t>(damped_ms);

  // Target retention window in ms
  size_t free_mb = free_ram_bytes / (1024 * 1024);
  _target_retention_window_ms = _ms_per_mb * (free_mb > 0 ? free_mb : 1);

  auto now = std::chrono::steady_clock::now().time_since_epoch();
  _timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool RedjetDynamicSoftRefClock::should_clear_soft_reference(jlong clock_timestamp, jlong ref_timestamp, bool clear_all) {
  if (clear_all) {
    _soft_references_cleared.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  jlong interval = clock_timestamp - ref_timestamp;
  if (interval < 0) interval = 0;

  // Maximum allowed interval based on dynamic policy and free memory
  jlong max_interval = static_cast<jlong>(_ms_per_mb * 64); // 64 MB baseline allocation allowance
  if (interval > max_interval) {
    _soft_references_cleared.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  _soft_references_retained.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// ============================================================================
// RedjetReferenceProcessor Implementation
// ============================================================================

RedjetReferenceProcessor::RedjetReferenceProcessor(RedjetHeap* heap, RedjetMarkBitMap* bitmap, uint32_t active_workers)
  : _heap(heap),
    _bitmap(bitmap),
    _active_workers(std::max<uint32_t>(1, std::min<uint32_t>(active_workers, MAX_WORKERS))),
    _pending_list_head(nullptr),
    _discovery_enabled(false),
    _offheap_count(0),
    _total_references_processed(0),
    _total_jni_weak_unlinked(0),
    _total_rjgc_offheap_synced(0),
    _total_vm_weak_unlinked(0),
    _total_resurrected_finals(0),
    _total_processing_cycles(0),
    _total_processing_time_us(0),
    _transitive_resurrection_marks(0) {
  for (uint32_t i = 0; i < MAX_WORKERS; ++i) {
    _worker_data[i].reset();
  }
  for (size_t i = 0; i < MAX_OFFHEAP_ENTRIES; ++i) {
    _offheap_table[i].native_address = 0;
    _offheap_table[i].size_bytes = 0;
    _offheap_table[i].flags = 0;
    _offheap_table[i].registration_epoch = 0;
    _offheap_table[i].weak_owner_ref = nullptr;
  }
}

void RedjetReferenceProcessor::enable_discovery() {
  for (uint32_t i = 0; i < _active_workers; ++i) {
    _worker_data[i].reset();
  }
  _discovery_enabled.store(true, std::memory_order_release);
}

void RedjetReferenceProcessor::disable_discovery() {
  _discovery_enabled.store(false, std::memory_order_release);
}

bool RedjetReferenceProcessor::is_referent_alive(oop obj) const {
  if (obj == nullptr) return false;
#ifndef REDJET_STANDALONE_BUILD
  return _heap->is_in(obj) && _bitmap->is_marked((HeapWord*)obj);
#else
  return true;
#endif
}

oop RedjetReferenceProcessor::referent(oop ref_obj) const {
  if (ref_obj == nullptr) return nullptr;
#ifndef REDJET_STANDALONE_BUILD
  return java_lang_ref_Reference::referent_no_keepalive(ref_obj);
#else
  return nullptr;
#endif
}

void RedjetReferenceProcessor::clear_referent(oop ref_obj) {
  if (ref_obj == nullptr) return;
#ifndef REDJET_STANDALONE_BUILD
  java_lang_ref_Reference::clear_referent(ref_obj);
#endif
}

oop RedjetReferenceProcessor::next_discovered(oop ref_obj) const {
  if (ref_obj == nullptr) return nullptr;
#ifndef REDJET_STANDALONE_BUILD
  return java_lang_ref_Reference::discovered(ref_obj);
#else
  return nullptr;
#endif
}

void RedjetReferenceProcessor::set_next_discovered(oop ref_obj, oop next) {
  if (ref_obj == nullptr) return;
#ifndef REDJET_STANDALONE_BUILD
  java_lang_ref_Reference::set_discovered(ref_obj, next);
#endif
}

bool RedjetReferenceProcessor::discover_reference(oop obj, ReferenceType type) {
  if (!_discovery_enabled.load(std::memory_order_relaxed) || obj == nullptr) {
    return false;
  }

#ifndef REDJET_STANDALONE_BUILD
  // Skip if already discovered
  if (next_discovered(obj) != nullptr) {
    return false;
  }

  oop ref_target = referent(obj);
  if (ref_target == nullptr) {
    return false; // Inactive / cleared
  }

  // If referent is strongly marked, no need to discover unless phantom/final
  if (type != REF_PHANTOM && type != REF_FINAL && is_referent_alive(ref_target)) {
    return false;
  }

  uint32_t worker_id = 0;
  Thread* thr = Thread::current_or_null();
  if (thr != nullptr && thr->is_Worker_thread()) {
    worker_id = thr->as_Worker_thread()->id() % _active_workers;
  }

  RedjetRefProcThreadLocal& local = _worker_data[worker_id];

  // Fast path: enqueue to thread-local discovery ring
  local._discovery_ring.push(obj);

  switch (type) {
    case REF_SOFT:
      set_next_discovered(obj, local._discovered_soft_head);
      local._discovered_soft_head = obj;
      local._discovered_counts[REF_SOFT]++;
      break;
    case REF_WEAK:
      set_next_discovered(obj, local._discovered_weak_head);
      local._discovered_weak_head = obj;
      local._discovered_counts[REF_WEAK]++;
      break;
    case REF_FINAL:
      set_next_discovered(obj, local._discovered_final_head);
      local._discovered_final_head = obj;
      local._discovered_counts[REF_FINAL]++;
      break;
    case REF_PHANTOM:
      set_next_discovered(obj, local._discovered_phantom_head);
      local._discovered_phantom_head = obj;
      local._discovered_counts[REF_PHANTOM]++;
      break;
    default:
      return false;
  }
  return true;
#else
  return true;
#endif
}

void RedjetReferenceProcessor::trace_resurrected_referent(oop obj) {
  if (obj == nullptr) return;

#ifndef REDJET_STANDALONE_BUILD
  // Marking closure that marks and queues unmarked children
  class RedjetResurrectClosure : public BasicOopIterateClosure {
  private:
    RedjetHeap*            _heap;
    RedjetMarkBitMap*      _bitmap;
    std::vector<oop>&      _worklist;
    std::atomic<uint64_t>& _resurrect_marks;

  public:
    RedjetResurrectClosure(RedjetHeap* heap, RedjetMarkBitMap* bitmap,
                           std::vector<oop>& worklist,
                           std::atomic<uint64_t>& resurrect_marks)
      : _heap(heap), _bitmap(bitmap), _worklist(worklist), _resurrect_marks(resurrect_marks) {}

    template <class T>
    inline void process_oop(T* p) {
      T o = RawAccess<>::oop_load(p);
      if (!CompressedOops::is_null(o)) {
        oop child = CompressedOops::decode_not_null(o);
        if (_heap->is_in(child) && _bitmap->par_mark((HeapWord*)child)) {
          _resurrect_marks.fetch_add(1, std::memory_order_relaxed);
          _worklist.push_back(child);
        }
      }
    }

    void do_oop(oop* p) override { process_oop(p); }
    void do_oop(narrowOop* p) override { process_oop(p); }
  };

  std::vector<oop> worklist;
  worklist.reserve(1024);

  if (_bitmap->par_mark((HeapWord*)obj)) {
    _transitive_resurrection_marks.fetch_add(1, std::memory_order_relaxed);
    worklist.push_back(obj);
  }

  RedjetResurrectClosure cl(_heap, _bitmap, worklist, _transitive_resurrection_marks);

  while (!worklist.empty()) {
    oop curr = worklist.back();
    worklist.pop_back();
    curr->oop_iterate(&cl);
  }
#endif
}

void RedjetReferenceProcessor::process_soft_references(bool clear_all) {
  _stats._soft_count = 0;

  for (uint32_t w = 0; w < _active_workers; ++w) {
    RedjetRefProcThreadLocal& local = _worker_data[w];
    oop current = local._discovered_soft_head;
    local._discovered_soft_head = nullptr;

    while (current != nullptr) {
      oop next = next_discovered(current);
      oop ref = referent(current);

      if (ref != nullptr && !is_referent_alive(ref)) {
#ifndef REDJET_STANDALONE_BUILD
        jlong clock_ts = java_lang_ref_SoftReference::clock();
        jlong ref_ts = java_lang_ref_SoftReference::timestamp(current);
        if (_soft_clock.should_clear_soft_reference(clock_ts, ref_ts, clear_all)) {
          clear_referent(current);
          set_next_discovered(current, local._discovered_soft_head);
          local._discovered_soft_head = current;
          local._enqueued_counts[REF_SOFT]++;
          local._cleared_counts[REF_SOFT]++;
          _stats._soft_count++;
        } else {
          // Soft reference retained: mark referent and transitive closure as live
          trace_resurrected_referent(ref);
          local._retained_counts[REF_SOFT]++;
        }
#else
        clear_referent(current);
        _stats._soft_count++;
#endif
      }
      current = next;
    }
  }
}

void RedjetReferenceProcessor::process_weak_references() {
  _stats._weak_count = 0;

  for (uint32_t w = 0; w < _active_workers; ++w) {
    RedjetRefProcThreadLocal& local = _worker_data[w];
    oop current = local._discovered_weak_head;
    local._discovered_weak_head = nullptr;

    while (current != nullptr) {
      oop next = next_discovered(current);
      oop ref = referent(current);

      if (ref != nullptr && !is_referent_alive(ref)) {
        clear_referent(current);
        set_next_discovered(current, local._discovered_weak_head);
        local._discovered_weak_head = current;
        local._enqueued_counts[REF_WEAK]++;
        local._cleared_counts[REF_WEAK]++;
        _stats._weak_count++;
      }
      current = next;
    }
  }
}

void RedjetReferenceProcessor::process_final_references() {
  _stats._final_count = 0;

  for (uint32_t w = 0; w < _active_workers; ++w) {
    RedjetRefProcThreadLocal& local = _worker_data[w];
    oop current = local._discovered_final_head;
    local._discovered_final_head = nullptr;

    while (current != nullptr) {
      oop next = next_discovered(current);
      oop ref = referent(current);

      if (ref != nullptr && !is_referent_alive(ref)) {
        // Resurrect referent and trace its entire reachable graph so finalize() is safe
        trace_resurrected_referent(ref);
        _total_resurrected_finals.fetch_add(1, std::memory_order_relaxed);
        local._resurrected_finals++;

        set_next_discovered(current, local._discovered_final_head);
        local._discovered_final_head = current;
        local._enqueued_counts[REF_FINAL]++;
        _stats._final_count++;
      }
      current = next;
    }
  }
}

void RedjetReferenceProcessor::process_phantom_references() {
  _stats._phantom_count = 0;

  for (uint32_t w = 0; w < _active_workers; ++w) {
    RedjetRefProcThreadLocal& local = _worker_data[w];
    oop current = local._discovered_phantom_head;
    local._discovered_phantom_head = nullptr;

    while (current != nullptr) {
      oop next = next_discovered(current);
      oop ref = referent(current);

      if (ref != nullptr && !is_referent_alive(ref)) {
        clear_referent(current);
        set_next_discovered(current, local._discovered_phantom_head);
        local._discovered_phantom_head = current;
        local._enqueued_counts[REF_PHANTOM]++;
        local._cleared_counts[REF_PHANTOM]++;
        _stats._phantom_count++;
      }
      current = next;
    }
  }
}

void RedjetReferenceProcessor::enqueue_references() {
#ifndef REDJET_STANDALONE_BUILD
  for (uint32_t w = 0; w < _active_workers; ++w) {
    RedjetRefProcThreadLocal& local = _worker_data[w];
    oop lists[4] = {
      local._discovered_soft_head,
      local._discovered_weak_head,
      local._discovered_final_head,
      local._discovered_phantom_head
    };

    for (int t = 0; t < 4; ++t) {
      oop current = lists[t];
      while (current != nullptr) {
        oop next = next_discovered(current);
        // Atomically enqueue to java.lang.ref.Reference.pending list
        oop old_pending = Universe::swap_reference_pending_list(current);
        set_next_discovered(current, old_pending);
        current = next;
      }
    }
    local.reset();
  }
#endif
}

size_t RedjetReferenceProcessor::unlink_vm_weak_tables() {
  size_t cleaned = 0;
#ifndef REDJET_STANDALONE_BUILD
  class RedjetIsAlivePredicate : public BoolObjectClosure {
  private:
    RedjetHeap*       _heap;
    RedjetMarkBitMap* _bitmap;
  public:
    RedjetIsAlivePredicate(RedjetHeap* heap, RedjetMarkBitMap* bitmap) : _heap(heap), _bitmap(bitmap) {}
    bool do_object_b(oop obj) override {
      return obj != nullptr && _heap->is_in(obj) && _bitmap->is_marked((HeapWord*)obj);
    }
  };

  class RedjetKeepAliveClosure : public OopClosure {
  private:
    RedjetHeap*       _heap;
    RedjetMarkBitMap* _bitmap;
  public:
    RedjetKeepAliveClosure(RedjetHeap* heap, RedjetMarkBitMap* bitmap) : _heap(heap), _bitmap(bitmap) {}
    void do_oop(oop* p) override {
      oop obj = RawAccess<>::oop_load(p);
      if (obj != nullptr && _heap->is_in(obj)) {
        _bitmap->par_mark((HeapWord*)obj);
      }
    }
    void do_oop(narrowOop* p) override {
      narrowOop noop_val = RawAccess<>::oop_load(p);
      if (!CompressedOops::is_null(noop_val)) {
        oop obj = CompressedOops::decode_not_null(noop_val);
        if (_heap->is_in(obj)) {
          _bitmap->par_mark((HeapWord*)obj);
        }
      }
    }
  };

  RedjetIsAlivePredicate is_alive(_heap, _bitmap);
  RedjetKeepAliveClosure keep_alive(_heap, _bitmap);

  for (OopStorage* storage : OopStorageSet::Range<OopStorageSet::WeakId>()) {
    if (storage != nullptr && storage->name() != nullptr && std::strcmp(storage->name(), "JNI Weak") != 0) {
      size_t before = storage->allocation_count();
      storage->weak_oops_do(&is_alive, &keep_alive);
      size_t after = storage->allocation_count();
      cleaned += (before > after) ? (before - after) : 0;
    }
  }
#endif
  _total_vm_weak_unlinked.fetch_add(cleaned, std::memory_order_relaxed);
  return cleaned;
}

void RedjetReferenceProcessor::process_discovered_references(bool clear_all_soft_refs, WorkerThreads* workers) {
  auto start_time = std::chrono::steady_clock::now();
  disable_discovery();

  // Phase 1: Soft references
  process_soft_references(clear_all_soft_refs);

  // Phase 2: Weak references
  process_weak_references();

  // Phase 3: Final references (with full transitive resurrection)
  process_final_references();

  // Phase 4: Phantom references
  process_phantom_references();

  // Phase 5: Enqueue all dead references to Reference.pending list
  enqueue_references();

  // Phase 6: Unlink JNI Weak Global handles
  unlink_jni_weak_roots();

  // Phase 7: Unlink JVM internal weak tables
  unlink_vm_weak_tables();

  // Phase 8: Synchronize with @RJGC off-heap smart cleaner
  sync_with_rjgc_offheap();

  auto end_time = std::chrono::steady_clock::now();
  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

  size_t total = _stats._soft_count + _stats._weak_count + _stats._final_count + _stats._phantom_count;
  _total_references_processed.fetch_add(total, std::memory_order_relaxed);
  _total_processing_cycles.fetch_add(1, std::memory_order_relaxed);
  _total_processing_time_us.fetch_add(elapsed_us, std::memory_order_relaxed);

  log_info(gc, ref)("Redjet Reference Processing: soft=%zu, weak=%zu, final=%zu, phantom=%zu (total=%zu, resurrected=%llu, elapsed=%lld us)",
                    _stats._soft_count, _stats._weak_count, _stats._final_count, _stats._phantom_count, total,
                    (unsigned long long)_total_resurrected_finals.load(std::memory_order_relaxed), (long long)elapsed_us);
}

size_t RedjetReferenceProcessor::unlink_jni_weak_roots() {
  size_t unlinked = 0;
#ifndef REDJET_STANDALONE_BUILD
  class RedjetIsAliveClosure : public BoolObjectClosure {
  private:
    RedjetMarkBitMap* _bitmap;
    RedjetHeap*       _heap;
  public:
    RedjetIsAliveClosure(RedjetHeap* heap, RedjetMarkBitMap* bitmap) : _bitmap(bitmap), _heap(heap) {}
    bool do_object_b(oop obj) override {
      return obj != nullptr && _heap->is_in(obj) && _bitmap->is_marked((HeapWord*)obj);
    }
  };

  class RedjetKeepAliveClosure : public OopClosure {
  private:
    RedjetHeap*       _heap;
    RedjetMarkBitMap* _bitmap;
  public:
    RedjetKeepAliveClosure(RedjetHeap* heap, RedjetMarkBitMap* bitmap) : _heap(heap), _bitmap(bitmap) {}
    void do_oop(oop* p) override {
      oop obj = RawAccess<>::oop_load(p);
      if (obj != nullptr && _heap->is_in(obj)) {
        _bitmap->par_mark((HeapWord*)obj);
      }
    }
    void do_oop(narrowOop* p) override {
      narrowOop noop_val = RawAccess<>::oop_load(p);
      if (!CompressedOops::is_null(noop_val)) {
        oop obj = CompressedOops::decode_not_null(noop_val);
        if (_heap->is_in(obj)) {
          _bitmap->par_mark((HeapWord*)obj);
        }
      }
    }
  };

  RedjetIsAliveClosure is_alive(_heap, _bitmap);
  RedjetKeepAliveClosure keep_alive(_heap, _bitmap);

  for (OopStorage* storage : OopStorageSet::Range<OopStorageSet::WeakId>()) {
    if (storage != nullptr && storage->name() != nullptr && std::strcmp(storage->name(), "JNI Weak") == 0) {
      size_t before_alloc = storage->allocation_count();
      storage->weak_oops_do(&is_alive, &keep_alive);
      size_t after_alloc = storage->allocation_count();
      unlinked += (before_alloc > after_alloc) ? (before_alloc - after_alloc) : 0;
      break;
    }
  }
#endif
  _total_jni_weak_unlinked.fetch_add(unlinked, std::memory_order_relaxed);
  return unlinked;
}

bool RedjetReferenceProcessor::register_offheap_segment(uintptr_t addr, size_t size, oop owner) {
  if (addr == 0 || size == 0) return false;

  size_t idx = _offheap_count.fetch_add(1, std::memory_order_relaxed);
  if (idx >= MAX_OFFHEAP_ENTRIES) {
    _offheap_count.store(MAX_OFFHEAP_ENTRIES, std::memory_order_relaxed);
    return false;
  }

  auto& entry = _offheap_table[idx];
  entry.native_address = addr;
  entry.size_bytes = size;
  entry.flags = 1;
  entry.registration_epoch = _total_processing_cycles.load(std::memory_order_relaxed);
  entry.weak_owner_ref = owner;
  return true;
}

size_t RedjetReferenceProcessor::sync_with_rjgc_offheap() {
  size_t reclaimed_offheap = 0;

  // Sweep EmptyBox morgue cache and advance epoch
  if (_heap != nullptr && _heap->falline() != nullptr) {
    size_t prev_emptybox = _heap->falline()->emptybox_count();
    _heap->falline()->advance_morgue_epoch(1, 0);
    size_t post_emptybox = _heap->falline()->emptybox_count();
    reclaimed_offheap += (post_emptybox > prev_emptybox) ? (post_emptybox - prev_emptybox) : 0;
  }

  // Sweep registered @RJGC off-heap segments whose owner objects are dead
  size_t total_registered = _offheap_count.load(std::memory_order_acquire);
  if (total_registered > MAX_OFFHEAP_ENTRIES) {
    total_registered = MAX_OFFHEAP_ENTRIES;
  }
  for (size_t i = 0; i < total_registered; ++i) {
    auto& entry = _offheap_table[i];
    if (entry.native_address != 0 && entry.weak_owner_ref != nullptr) {
      if (!is_referent_alive(entry.weak_owner_ref)) {
        // Deallocate native off-heap memory
#ifndef REDJET_STANDALONE_BUILD
        os::free(reinterpret_cast<void*>(entry.native_address));
#endif
        entry.native_address = 0;
        entry.weak_owner_ref = nullptr;
        reclaimed_offheap++;
      }
    }
  }

  // Trigger @RJGC Off-Heap Reclaimer sweeps
  size_t rjgc_evictions = RedjetOffHeapReclaimer::sweep_evictions();
  reclaimed_offheap += rjgc_evictions;

  _total_rjgc_offheap_synced.fetch_add(reclaimed_offheap, std::memory_order_relaxed);
  return reclaimed_offheap;
}

void RedjetReferenceProcessor::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet Reference Processor ---");
  st->print_cr("  Active Workers:             %u", _active_workers);
  st->print_cr("  Soft Ref Retention (ms/MB): %llu", (unsigned long long)_soft_clock.ms_per_mb());
  st->print_cr("  Free RAM Ratio:             %.2f%%", _soft_clock.free_ram_ratio() * 100.0);
  st->print_cr("  Allocation Pressure Factor: %.2f", _soft_clock.allocation_pressure_factor());
  st->print_cr("  Target Retention Window:    %llu ms", (unsigned long long)_soft_clock.target_retention_window_ms());
  st->print_cr("  Total Processing Cycles:    %llu", (unsigned long long)_total_processing_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Total Processing Time:      %llu us", (unsigned long long)_total_processing_time_us.load(std::memory_order_relaxed));
  st->print_cr("  Total References Processed: %llu", (unsigned long long)_total_references_processed.load(std::memory_order_relaxed));
  st->print_cr("  JNI Weak Roots Unlinked:    %llu", (unsigned long long)_total_jni_weak_unlinked.load(std::memory_order_relaxed));
  st->print_cr("  VM Weak Roots Unlinked:     %llu", (unsigned long long)_total_vm_weak_unlinked.load(std::memory_order_relaxed));
  st->print_cr("  Finalizers Resurrected:     %llu", (unsigned long long)_total_resurrected_finals.load(std::memory_order_relaxed));
  st->print_cr("  Transitive Resurrection Marks: %llu", (unsigned long long)_transitive_resurrection_marks.load(std::memory_order_relaxed));
  st->print_cr("  @RJGC Off-Heap Synced:      %llu (Tracked: %zu)",
               (unsigned long long)_total_rjgc_offheap_synced.load(std::memory_order_relaxed),
               _offheap_count.load(std::memory_order_relaxed));
  st->print_cr("  Last Cycle Stats:           soft=%zu, weak=%zu, final=%zu, phantom=%zu",
               _stats._soft_count, _stats._weak_count, _stats._final_count, _stats._phantom_count);
}
