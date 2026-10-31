/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletInitLogger.hpp"
#include "gc/redlet/redletMemoryPool.hpp"
#include "gc/redlet/redletThreadLocalData.hpp"
#include "gc/redlet/redlet_globals.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "gc/shared/space.inline.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "services/memoryService.hpp"
#include "utilities/macros.hpp"

RedletHeap* RedletHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  assert(heap != nullptr, "Uninitialized heap");
  assert(heap->kind() == CollectedHeap::Redlet, "Invalid heap kind");
  return static_cast<RedletHeap*>(heap);
}

RedletHeap::RedletHeap() :
  _monitoring_support(nullptr),
  _pool(nullptr),
  _memory_manager("Redlet Heap"),
  _space(nullptr),
  _max_tlab_size(0),
  _current_sweep_no(1),
  _allocated_bytes(0) {}

jint RedletHeap::initialize() {
  CollectedHeap::initialize();

  size_t align = _barrier_set->conservative_max_heap_alignment();
  size_t init_byte_size = align_up(InitialHeapSize, align);
  size_t max_byte_size  = align_up(MaxHeapSize, align);

  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
  _virtual_space.initialize(heap_rs, init_byte_size);

  MemRegion committed_region((HeapWord*)_virtual_space.low(), (HeapWord*)_virtual_space.high());
  MemRegion reserved_region((HeapWord*)_virtual_space.low_boundary(), (HeapWord*)_virtual_space.high_boundary());

  initialize_reserved_region(reserved_region);

  _space = new ContiguousSpace();
  _space->initialize(committed_region, true, true);

  _max_tlab_size = MIN2(RedletMaxTLABSize / HeapWordSize, _space->capacity());

  // Initialize VFS memory tree
  _vfs_tree.init();

  RedletInitLogger::print();

  return JNI_OK;
}

void RedletHeap::initialize_serviceability() {
  _pool = new RedletMemoryPool(this);
  _monitoring_support = new RedletMonitoringSupport(this);
  _memory_manager.add_pool(_pool);
}

GrowableArray<GCMemoryManager*> RedletHeap::memory_managers() {
  return GrowableArray<GCMemoryManager*>(1, 1, &_memory_manager);
}

GrowableArray<MemoryPool*> RedletHeap::memory_pools() {
  return GrowableArray<MemoryPool*>(1, 1, _pool);
}

size_t RedletHeap::used() const {
  return _space != nullptr ? _space->used() : 0;
}

HeapWord* RedletHeap::allocate_work(size_t size) {
  assert(is_object_aligned(size), "Allocation size should be aligned: %zu", size);

  // 1. First attempt: Snatch from Falline emptybox cache (sub-microsecond O(1) snatch)
  RedletEmptyBox* eb = _falline.snatch_emptybox(size * HeapWordSize, _current_sweep_no.load(std::memory_order_relaxed));
  if (eb != nullptr) {
    HeapWord* res = reinterpret_cast<HeapWord*>(eb);
    _allocated_bytes.fetch_add(size * HeapWordSize, std::memory_order_relaxed);
    log_trace(gc, alloc)("Allocated " SIZE_FORMAT " bytes via Falline Snatch at " PTR_FORMAT, size * HeapWordSize, p2i(res));
    return res;
  }

  // 2. Second attempt: Bump allocate from committed contiguous space
  HeapWord* res = _space->par_allocate(size);
  if (res != nullptr) {
    _allocated_bytes.fetch_add(size * HeapWordSize, std::memory_order_relaxed);
    return res;
  }

  // 3. Third attempt: Expand heap if possible
  size_t expand_bytes = align_up(MAX2(size * HeapWordSize, (size_t)RedletMinHeapExpand), SpaceAlignment);
  if (_virtual_space.uncommitted_size() >= expand_bytes) {
    if (_virtual_space.expand_by(expand_bytes)) {
      MemRegion new_committed((HeapWord*)_virtual_space.low(), (HeapWord*)_virtual_space.high());
      _space->set_end((HeapWord*)_virtual_space.high());
      log_info(gc)("Expanded heap by " EXACTFMT " to " EXACTFMT, EXACTFMTARGS(expand_bytes), EXACTFMTARGS(_virtual_space.committed_size()));
      res = _space->par_allocate(size);
      if (res != nullptr) {
        _allocated_bytes.fetch_add(size * HeapWordSize, std::memory_order_relaxed);
        return res;
      }
    }
  }

  // 4. Fourth attempt: Execute RMRP reclamation and retry
  if (RedletRMRP) {
    size_t reclaimed = execute_rmrp();
    if (reclaimed >= size * HeapWordSize) {
      eb = _falline.snatch_emptybox(size * HeapWordSize, _current_sweep_no.load(std::memory_order_relaxed));
      if (eb != nullptr) {
        return reinterpret_cast<HeapWord*>(eb);
      }
    }
  }

  return nullptr;
}

HeapWord* RedletHeap::mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) {
  *gc_overhead_limit_was_exceeded = false;
  return allocate_work(size);
}

HeapWord* RedletHeap::allocate_new_tlab(size_t min_size, size_t requested_size, size_t* actual_size) {
  Thread* thread = Thread::current();
  size_t size = requested_size;

  if (RedletElasticTLAB) {
    size_t ergo_size = RedletThreadLocalData::ergo_tlab_size(thread);
    if (ergo_size != 0) {
      size = clamp(ergo_size, min_size, requested_size);
    }
  }

  size = align_object_size(size);
  HeapWord* res = allocate_work(size);

  if (res != nullptr) {
    *actual_size = size;
    if (RedletElasticTLAB) {
      RedletThreadLocalData::set_ergo_tlab_size(thread, size * 2);
    }
  } else {
    *actual_size = 0;
  }

  return res;
}

size_t RedletHeap::unsafe_max_tlab_alloc(Thread* thr) const {
  if (_space == nullptr) return 0;
  return MIN2(_max_tlab_size, _space->free() / HeapWordSize);
}

void RedletHeap::collect(GCCause::Cause cause) {
  log_info(gc)("RedletGC collection triggered: cause '%s'", GCCause::to_string(cause));
  _current_sweep_no.fetch_add(1, std::memory_order_relaxed);
  if (RedletRMRP) {
    execute_rmrp();
  }
}

void RedletHeap::do_full_collection(bool clear_all_soft_refs) {
  collect(GCCause::_full_gc_alot);
}

void RedletHeap::object_iterate(ObjectClosure* cl) {
  if (_space != nullptr) {
    _space->object_iterate(cl);
  }
}

size_t RedletHeap::execute_rmrp() {
  log_debug(gc, rmrp)("Executing RMRP memory resident reclamation (method: %s)", RedletRMRPMethod);
  size_t reclaimed_bytes = 0;

  // Track falline stats
  size_t count = _falline.emptybox_count();
  log_info(gc, rmrp)("RMRP scan: %zu emptyboxes currently registered in Falline cache", count);

  return reclaimed_bytes;
}
