/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETHEAP_HPP
#define SHARE_GC_REDLET_REDLETHEAP_HPP

#include "gc/redlet/redletBarrierSet.hpp"
#include "gc/redlet/redletFalline.hpp"
#include "gc/redlet/redletMonitoringSupport.hpp"
#include "gc/redlet/vfs/redletVFSTree.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "gc/shared/space.hpp"
#include "memory/virtualspace.hpp"
#include "services/memoryManager.hpp"
#include <atomic>

class RedletMemoryPool;

class RedletHeap : public CollectedHeap {
  friend class VMStructs;

private:
  RedletMonitoringSupport* _monitoring_support;
  MemoryPool*              _pool;
  GCMemoryManager          _memory_manager;
  ContiguousSpace*         _space;
  VirtualSpace             _virtual_space;
  size_t                   _max_tlab_size;

  RedletVFSTree            _vfs_tree;
  RedletFalline            _falline;
  std::atomic<uint64_t>    _current_sweep_no;
  std::atomic<size_t>      _allocated_bytes;

public:
  static RedletHeap* heap();

  RedletHeap();

  Name kind() const override {
    return CollectedHeap::Redlet;
  }

  const char* name() const override {
    return "Redlet";
  }

  jint initialize() override;
  void initialize_serviceability() override;

  GrowableArray<GCMemoryManager*> memory_managers() override;
  GrowableArray<MemoryPool*> memory_pools() override;

  size_t max_capacity() const override { return _virtual_space.reserved_size();  }
  size_t capacity()     const override { return _virtual_space.committed_size(); }
  size_t used()         const override;

  bool is_in(const void* p) const override {
    return _space != nullptr && _space->is_in(p);
  }

  bool requires_barriers(stackChunkOop obj) const override { return false; }

  // Allocation
  HeapWord* allocate_work(size_t size);
  HeapWord* mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) override;
  HeapWord* allocate_new_tlab(size_t min_size,
                              size_t requested_size,
                              size_t* actual_size) override;

  // TLAB allocation
  size_t tlab_capacity(Thread* thr)         const override { return capacity();     }
  size_t tlab_used(Thread* thr)             const override { return used();         }
  size_t max_tlab_size()                    const override { return _max_tlab_size; }
  size_t unsafe_max_tlab_alloc(Thread* thr) const override;

  void collect(GCCause::Cause cause) override;
  void do_full_collection(bool clear_all_soft_refs) override;

  // Heap walking support
  void object_iterate(ObjectClosure* cl) override;

  // Object pinning support: every object is implicitly pinned
  void pin_object(JavaThread* thread, oop obj) override {}
  void unpin_object(JavaThread* thread, oop obj) override {}

  // No support for block parsing
  HeapWord* block_start(const void* addr) const { return nullptr; }
  bool block_is_obj(const HeapWord* addr) const { return false; }

  void gc_threads_do(ThreadClosure* tc) const override {}

  void register_nmethod(nmethod* nm) override {}
  void unregister_nmethod(nmethod* nm) override {}
  void verify_nmethod(nmethod* nm) override {}

  void prepare_for_verify() override {}
  void verify(VerifyOption option) override {}

  // Redlet custom subsystems
  RedletVFSTree* vfs()     { return &_vfs_tree; }
  RedletFalline* falline() { return &_falline; }

  // RMRP (Reduce Memory Resident Set Reclamation)
  size_t execute_rmrp();
};

#endif // SHARE_GC_REDLET_REDLETHEAP_HPP
