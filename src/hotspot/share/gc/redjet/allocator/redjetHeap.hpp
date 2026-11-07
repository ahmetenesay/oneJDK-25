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

#ifndef SHARE_GC_REDJET_REDJETHEAP_HPP
#define SHARE_GC_REDJET_REDJETHEAP_HPP

#include <atomic>
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/simd/redjetSIMD.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/allocator/redjetPagefile.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/allocator/redjetSlabAllocator.hpp"
#include "gc/redjet/engine/redjetPitstop.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/runtime/redjetTensorViews.hpp"
#include "gc/redjet/simd/redjetMathRJ.hpp"
#include "gc/redjet/allocator/redjetTBBArena.hpp"
#include "gc/redjet/engine/redjetFalline.hpp"
#include "gc/redjet/runtime/redjetGC.hpp"
#include "gc/redjet/runtime/redjetBarrierSet.hpp"
#include "gc/redjet/runtime/redjetMonitoringSupport.hpp"
#include "gc/redjet/simd/redjetMarkBitMap.hpp"
#include "gc/redjet/engine/redjetRootScanner.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "gc/shared/space.hpp"
#include "memory/virtualspace.hpp"
#include "runtime/globals.hpp"
#include "services/memoryManager.hpp"

class RedjetMemoryPool;

class RedjetHeap : public CollectedHeap {
  friend class VMStructs;

private:
  RedjetMonitoringSupport* _monitoring_support;
  MemoryPool*              _pool;
  GCMemoryManager          _memory_manager;
  ContiguousSpace*         _space;
  VirtualSpace             _virtual_space;
  size_t                   _max_tlab_size;

  sacculus_hotlog_t        _hotlog;
  sacculus_coldlog_t       _coldlog;
  RedjetVFSTree            _vfs_tree;
  RedjetFalline            _falline;
  RedjetTBBArenaManager    _tbb_arenas;
  RedjetMotherline         _motherline;
  RedjetRunway             _runway;
  RedjetSlabAllocator      _slab_allocator;
  RedjetLandfill           _landfill;
  RedjetPitstop            _pitstop;
  RedjetPuzzleArena        _puzzle_arena;
  RedjetGC                 _gc;
  RedjetMarkBitMap         _mark_bitmap;
  RedjetRootScanner*       _root_scanner;
  msimd_context            _msimd_ctx;

  std::atomic<uint64_t>    _current_sweep_no;
  std::atomic<size_t>      _allocated_bytes;

public:
  static RedjetHeap* heap();

  RedjetHeap();
  ~RedjetHeap();

  Name kind() const override {
    return CollectedHeap::Redjet;
  }

  const char* name() const override {
    return "Redjet";
  }

  jint initialize() override;
  void initialize_serviceability() override;

  GrowableArray<GCMemoryManager*> memory_managers() override;
  GrowableArray<MemoryPool*> memory_pools() override;

  size_t max_capacity() const override { return _virtual_space.reserved_size() + _slab_allocator.capacity();  }
  size_t capacity()     const override { return _virtual_space.committed_size() + _slab_allocator.capacity(); }
  size_t used()         const override;

  bool is_in(const void* p) const override {
    return (_space != nullptr && _space->is_in(p)) || _slab_allocator.is_in(p);
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

  MemRegion reserved_region() const { return _reserved; }
  bool is_in_reserved(const void* addr) const {
    return _reserved.contains(addr) || _slab_allocator.is_in(addr);
  }

  void print_heap_on(outputStream* st) const override;
  void print_gc_on(outputStream* st) const override {}
  void print_tracing_info() const override;
  bool print_location(outputStream* st, void* addr) const override;

  // Redjet custom subsystems
  sacculus_hotlog_t*     hotlog()       { return &_hotlog; }
  sacculus_coldlog_t*    coldlog()      { return &_coldlog; }
  RedjetVFSTree*         vfs()          { return &_vfs_tree; }
  RedjetFalline*         falline()      { return &_falline; }
  RedjetTBBArenaManager* tbb_arenas()   { return &_tbb_arenas; }
  RedjetMotherline*      motherline()   { return &_motherline; }
  RedjetRunway*          runway()         { return &_runway; }
  RedjetSlabAllocator*   slab_allocator() { return &_slab_allocator; }
  RedjetLandfill*        landfill()       { return &_landfill; }
  RedjetPitstop*         pitstop()      { return &_pitstop; }
  RedjetPuzzleArena*     puzzle_arena() { return &_puzzle_arena; }
  RedjetGC*              gc()           { return &_gc; }
  RedjetMarkBitMap*      mark_bitmap()  { return &_mark_bitmap; }
  RedjetRootScanner*     root_scanner() { return _root_scanner; }
  msimd_context          msimd_ctx()    { return _msimd_ctx; }
  rj_context             rj_ctx()       { return _msimd_ctx; }

  // Root scanning and sweep operations
  size_t scan_roots_and_trace();
  size_t sweep_unmarked();
  void oops_do(OopClosure* cl);

  bool is_sc_mode()               const { return RedjetSCMode; }
  bool is_forbid_dynamic_growth() const { return RedjetForbidDynamicGrowth; }

  // RMRP (Reduce Memory Resident Set Reclamation)
  size_t execute_rmrp();
};

#endif // SHARE_GC_REDJET_REDJETHEAP_HPP
