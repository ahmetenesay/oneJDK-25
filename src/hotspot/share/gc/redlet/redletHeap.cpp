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

#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletInitLogger.hpp"
#include "gc/redlet/redletMemoryPool.hpp"
#include "gc/redlet/redletThreadLocalData.hpp"
#include "gc/redlet/redlet_globals.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/universe.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "services/memoryService.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

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
  _msimd_ctx(nullptr),
  _current_sweep_no(1),
  _allocated_bytes(0) {}

RedletHeap::~RedletHeap() {
  if (_msimd_ctx != nullptr) {
    sf_destroy(_msimd_ctx);
    _msimd_ctx = nullptr;
  }
}

jint RedletHeap::initialize() {
  // Bind MPMC Vyukov RingLogs early so pre-boot telemetry is flushed immediately
  sf_ring_bind_active_logs(&_hotlog, &_coldlog);

  // Initialize unified Hardware Engine Nexus (Hybrid P+E, NUMA, SMT, SRAM/BLIS, TableGen uArch, PMU)
  sf_init_hardware_nexus(nullptr);

  size_t align = HeapAlignment;
  size_t init_byte_size = align_up(InitialHeapSize, align);
  size_t max_byte_size  = align_up(MaxHeapSize, align);

  if (RedletSCMode || RedletForbidDynamicGrowth || AlwaysPreTouch) {
    // Under Safety-Critical (SSIOLSC), AlwaysPreTouch, or when dynamic growth is forbidden,
    // lock initial committed size to MaxHeapSize upfront to eliminate paging jitter.
    init_byte_size = max_byte_size;
    log_info(gc)("RedletGC Zero-Jitter boundary: Committed heap locked to MaxHeapSize upfront (" EXACTFMT ")", EXACTFMTARGS(max_byte_size));
  }

  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
  _virtual_space.initialize(heap_rs, init_byte_size);

  MemRegion committed_region((HeapWord*)_virtual_space.low(), (HeapWord*)_virtual_space.high());

  initialize_reserved_region(heap_rs);

  _space = new ContiguousSpace();
  _space->initialize(committed_region, true, true);

  _max_tlab_size = MIN2(RedletMaxTLABSize / HeapWordSize, _space->capacity());

  // Initialize VFS memory trees & strict branch validation policies
  _vfs_tree.init();
  g_sf_vfs_tree.init();
  SF_custominiVm_set_strict_vfs_branch_validation(RedletVFSStrict ? 1 : 0);
  SF_custominiVm_vfs_set_panic_dialog_enabled(RedletVFSPanicDialog ? 1 : 0);

  // Initialize oneTBB heterogeneous task arenas if enabled
  if (RedletHeteroArenas) {
    _tbb_arenas.init();
  }

  // Initialize runway allocator with initial capacity and bind to Strided Tensor Views & Primitiva
  _runway.init(max_byte_size / 4, RedletForbidDynamicGrowth || RedletSCMode);
  sf_tensor_views_bind_runway(&_runway);

  // Initialize unified RedletGC Big GC engine
  _gc.init(&_runway, &_motherline, &_falline, &_landfill);

  // Initialize default Pure-Mathematical MSIMD execution context (active out-of-the-box for Java math/SIMD)
  msimd_config_t mcfg{};
  mcfg.profile                 = RedletSCMode ? MSIMD_PROFILE_SAFETY_CRITICAL : MSIMD_PROFILE_STANDARD;
  mcfg.static_arena_size_bytes = RedletSCMode ? MIN2(max_byte_size / 8, (size_t)(64ULL * 1024 * 1024)) : 0;
  mcfg.max_loop_iterations     = RedletSCMode ? 1000000u : 0u;
  mcfg.forbid_runtime_heap     = RedletSCMode || RedletForbidDynamicGrowth;
  mcfg.user_static_arena_buf   = nullptr;
  msimd_backend_t backend      = RedletHeteroArenas ? MSIMD_BACKEND_TBB : MSIMD_BACKEND_AUTO;
  if (sf_init_with_config(&_msimd_ctx, MSIMD_ISA_AUTO, backend, &mcfg) != MSIMD_SUCCESS) {
    log_warning(gc)("RedletGC MSIMD context initialization fell back to scalar/fallback tier");
  }

  // Install barrier set
  BarrierSet::set_barrier_set(new RedletBarrierSet());

  RedletInitLogger::print();

  return JNI_OK;
}

void RedletHeap::initialize_serviceability() {
  _pool = new RedletMemoryPool(this);
  _monitoring_support = new RedletMonitoringSupport(this);
  _memory_manager.add_pool(_pool);
}

GrowableArray<GCMemoryManager*> RedletHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(1);
  memory_managers.append(&_memory_manager);
  return memory_managers;
}

GrowableArray<MemoryPool*> RedletHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(1);
  memory_pools.append(_pool);
  return memory_pools;
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
    log_trace(gc, alloc)("Allocated %zu bytes via Falline Snatch at " PTR_FORMAT, size * HeapWordSize, p2i(res));
    return res;
  }

  // 2. Second attempt: Bump allocate from committed contiguous space
  HeapWord* res = _space->par_allocate(size);
  if (res != nullptr) {
    _allocated_bytes.fetch_add(size * HeapWordSize, std::memory_order_relaxed);
    return res;
  }

  // 3. Third attempt: Expand heap if possible (strictly forbidden when RedletForbidDynamicGrowth or RedletSCMode is active)
  if (!RedletForbidDynamicGrowth && !RedletSCMode) {
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
  } else {
    log_debug(gc, alloc)("Dynamic heap expansion forbidden under RedletSCMode / RedletForbidDynamicGrowth (%zu words requested)", size);
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
  // Support PrimitiveRW bypass and deterministic Default bureaucratic collection phases
  if (RedletSCMode) {
    if (RedletSCGCType != nullptr && strcasecmp(RedletSCGCType, "PrimitiveRW") == 0) {
      log_info(gc)("RedletGC SC: PrimitiveRW bypass active - GC collection bypassed for cause '%s'", GCCause::to_string(cause));
      sf_unified_log_emit(SF_Ebin0_GC_SWEEP, SF_Ebin1_INFO, 0, "sc_bypass",
                          "RedletGC SC: PrimitiveRW bypass active - GC collection bypassed");
      return;
    }

    // Deterministic Default bureaucratic collection phases
    log_info(gc)("RedletGC SC: Deterministic Bureaucratic collection triggered: cause '%s'", GCCause::to_string(cause));
    sf_unified_log_emit(SF_Ebin0_GC_PHASE_CHANGE, SF_Ebin1_INFO, 0, "sc_bureaucratic",
                        "RedletGC SC: Deterministic Bureaucratic Phase-STW collection active");
    _current_sweep_no.fetch_add(1, std::memory_order_relaxed);

    // Phase 1: STW Bureaucratic sweep & VFS hierarchy audit
    log_debug(gc)("RedletGC SC Phase 1: Bureaucratic STW and VFS integrity validation");
    sf_gc_safepoint_rendezvous_wait();
    sf_gc_hookpoint_trigger(SF_HOOKPOINT_GC_PRE_SWEEP, 0, nullptr, 0);
    _gc.step();
    sf_gc_hookpoint_trigger(SF_HOOKPOINT_GC_POST_SWEEP, 0, nullptr, 0);

    // Phase 2: Falline emptybox cache deterministic sweep & coalesce
    log_debug(gc)("RedletGC SC Phase 2: Falline emptybox deterministic cache audit");

    // Phase 3: RMRP deterministic reclamation if active
    if (RedletRMRP) {
      execute_rmrp();
    }
    log_info(gc)("RedletGC SC: Deterministic Bureaucratic collection phase completed (sweep %llu)",
                 (unsigned long long)_current_sweep_no.load(std::memory_order_relaxed));
    return;
  }

  log_info(gc)("RedletGC collection triggered: cause '%s'", GCCause::to_string(cause));
  _current_sweep_no.fetch_add(1, std::memory_order_relaxed);
  sf_gc_safepoint_rendezvous_wait();
  sf_gc_controlpoint_check(_allocated_bytes.load(std::memory_order_relaxed));
  g_sf_mltc_solver.check_and_solve(used(), capacity(), 0.0f);
  sf_gc_hookpoint_trigger(SF_HOOKPOINT_GC_PRE_SWEEP, 0, nullptr, 0);
  _gc.step();
  sf_gc_hookpoint_trigger(SF_HOOKPOINT_GC_POST_SWEEP, 0, nullptr, 0);
  if (RedletRMRP) {
    execute_rmrp();
  }
  drain_hotlog_to_coldlog(&_hotlog, &_coldlog);
}

void RedletHeap::do_full_collection(bool clear_all_soft_refs) {
  log_info(gc)("RedletGC full collection triggered (clear_all_soft_refs=%s)", clear_all_soft_refs ? "true" : "false");
  sf_gc_safepoint_rendezvous_wait();
  sf_gc_hookpoint_trigger(SF_HOOKPOINT_GC_PRE_COMPACT, 0, nullptr, 0);
  _gc.shake_bed();
  sf_gc_hookpoint_trigger(SF_HOOKPOINT_GC_POST_COMPACT, 0, nullptr, 0);
  _current_sweep_no.fetch_add(1, std::memory_order_relaxed);
  if (RedletRMRP) {
    execute_rmrp();
  }
  drain_hotlog_to_coldlog(&_hotlog, &_coldlog);
}

void RedletHeap::object_iterate(ObjectClosure* cl) {
  if (_space != nullptr) {
    _space->object_iterate(cl);
  }
}

void RedletHeap::print_heap_on(outputStream* st) const {
  st->print_cr("Redlet Heap (%s)", RedletSCMode ? "SSIOLSC Safety-Critical" : "NORMAX Dynamic SIMD");
  StreamIndentor si(st, 1);
  _virtual_space.print_on(st);
  if (_space != nullptr) {
    st->print_cr("Allocation space:");
    StreamIndentor si2(st, 1);
    _space->print_on(st, "");
  }
  _falline.print_on(st);
}

bool RedletHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<RedletHeap>::print_location(st, addr);
}

void RedletHeap::print_tracing_info() const {
  size_t reserved  = max_capacity();
  size_t committed = capacity();
  size_t cur_used  = used();
  if (reserved != 0) {
    log_info(gc)("Redlet Heap: %zu%s reserved, %zu%s (%.2f%%) committed, %zu%s (%.2f%%) used",
                 byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
                 byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
                 committed * 100.0 / reserved,
                 byte_size_in_proper_unit(cur_used),  proper_unit_for_byte_size(cur_used),
                 cur_used * 100.0 / reserved);
  }
}

size_t RedletHeap::execute_rmrp() {
  log_debug(gc, heap)("Executing RMRP memory resident reclamation (method: %s)", RedletRMRPMethod);
  sf_rmrp_method_e method =
      (RedletRMRPMethod != nullptr && strcasecmp(RedletRMRPMethod, "Sequential") == 0)
          ? sf_rmrp_method_e::SEQUENTIAL
          : sf_rmrp_method_e::PLUMP;

  size_t count = _falline.emptybox_count();
  size_t page_sz = os::vm_page_size();
  size_t target_bytes = (count > 0) ? (count * page_sz) : page_sz;
  size_t reclaimed_bytes = RedletRMRP::execute_reclamation(method, target_bytes);
  if (reclaimed_bytes > 0) {
    _runway.allocator()->rebate_pages(static_cast<uint32_t>(method), reclaimed_bytes);
  }
  log_info(gc, heap)("RMRP scan: %zu emptyboxes in Falline cache, reclaimed %zu bytes", count, reclaimed_bytes);

  return reclaimed_bytes;
}
