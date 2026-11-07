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

#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/runtime/redjetInitLogger.hpp"
#include "gc/redjet/runtime/redjetThreadLocalData.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjetReferenceProcessor.hpp"
#include "gc/redjet/runtime/redjetStringDedup.hpp"
#include "gc/redjet/runtime/redjetStringTableCleaner.hpp"
#include "gc/redjet/runtime/redjetClassUnloader.hpp"
#include "gc/redjet/runtime/redjetGCMemoryManager.hpp"
#include "gc/redjet/runtime/redjetMemoryPool.hpp"
#include "gc/redjet/runtime/redjetGCPolicyCounters.hpp"
#include "gc/redjet/runtime/redjetScheduler.hpp"
#include "gc/redjet/engine/redjetSeasonalCoordinator.hpp"
#include "gc/redjet/runtime/redjetOSInterruptShield.hpp"
#include "gc/redjet/runtime/redjetSnatchRebate.hpp"
#include "gc/redjet/runtime/redjetUpclocker.hpp"
#include "gc/redjet/runtime/redjetBPUDirector.hpp"
#include "gc/redjet/runtime/redjetUnifiedLogger.hpp"
#include "gc/redjet/runtime/redjetHeapVerifier.hpp"
#include "gc/redjet/simd/redjetSIMDContentionGovernor.hpp"
#include "gc/redjet/api/redjetFastJNI.hpp"
#include "gc/redjet/api/redjetFastFFM.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "oops/oop.inline.hpp"
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

RedjetHeap* RedjetHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  assert(heap != nullptr, "Uninitialized heap");
  assert(heap->kind() == CollectedHeap::Redjet, "Invalid heap kind");
  return static_cast<RedjetHeap*>(heap);
}

RedjetHeap::RedjetHeap() :
  _monitoring_support(nullptr),
  _pool(nullptr),
  _memory_manager("Redjet Heap"),
  _gc_memory_manager(nullptr),
  _ref_processor(nullptr),
  _class_unloader(nullptr),
  _policy_counters(nullptr),
  _space(nullptr),
  _max_tlab_size(0),
  _root_scanner(nullptr),
  _msimd_ctx(nullptr),
  _current_sweep_no(1),
  _allocated_bytes(0) {}

RedjetHeap::~RedjetHeap() {
  _slab_allocator.destroy();
  if (_policy_counters != nullptr) {
    delete _policy_counters;
    _policy_counters = nullptr;
  }
  if (_class_unloader != nullptr) {
    delete _class_unloader;
    _class_unloader = nullptr;
  }
  if (_ref_processor != nullptr) {
    delete _ref_processor;
    _ref_processor = nullptr;
  }
  if (_gc_memory_manager != nullptr) {
    delete _gc_memory_manager;
    _gc_memory_manager = nullptr;
  }
  if (_root_scanner != nullptr) {
    delete _root_scanner;
    _root_scanner = nullptr;
  }
  if (_msimd_ctx != nullptr) {
    hs_destroy(_msimd_ctx);
    _msimd_ctx = nullptr;
  }
  RedjetFastFFM::shutdown();
  RedjetFastJNI::shutdown();
  RedjetSeasonalCoordinator::shutdown();
  RedjetOSInterruptShield::shutdown();
  RedjetSnatchRebateEngine::shutdown();
  RedjetUpclocker::instance().destroy();
  RedjetCompressedSpaces::instance().shutdown();
  g_redjet_scheduler.shutdown();
}

jint RedjetHeap::initialize() {
  // Bind MPMC Vyukov RingLogs early so pre-boot telemetry is flushed immediately
  hs_ring_bind_active_logs(&_hotlog, &_coldlog);

  // Initialize unified Hardware Engine Nexus (Hybrid P+E, NUMA, SMT, SRAM/BLIS, TableGen uArch, PMU)
  hs_init_hardware_nexus(nullptr);

  size_t align = HeapAlignment;
  size_t init_byte_size = align_up(InitialHeapSize, align);
  size_t max_byte_size  = align_up(MaxHeapSize, align);

  if (RedjetSCMode || RedjetForbidDynamicGrowth || AlwaysPreTouch) {
    // Under Safety-Critical (SSIOLSC), AlwaysPreTouch, or when dynamic growth is forbidden,
    // lock initial committed size to MaxHeapSize upfront to eliminate paging jitter.
    init_byte_size = max_byte_size;
    log_info(gc)("RedjetGC Zero-Jitter boundary: Committed heap locked to MaxHeapSize upfront (" EXACTFMT ")", EXACTFMTARGS(max_byte_size));
  }

  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
  _virtual_space.initialize(heap_rs, init_byte_size);

  MemRegion committed_region((HeapWord*)_virtual_space.low(), (HeapWord*)_virtual_space.high());

  initialize_reserved_region(heap_rs);

  _space = new ContiguousSpace();
  _space->initialize(committed_region, true, true);

  _max_tlab_size = MIN2(RedjetMaxTLABSize / HeapWordSize, _space->capacity());

  // Initialize Redjet Mark BitMap covering committed heap space and Root Scanner
  _mark_bitmap.initialize((HeapWord*)_virtual_space.low(), (HeapWord*)_virtual_space.high());
  _root_scanner = new RedjetRootScanner(this, &_mark_bitmap);

  // Initialize HotSpot Parity Subsystems: Reference Processor, Class Unloader, String Dedup
  _ref_processor = new RedjetReferenceProcessor(this, &_mark_bitmap);
  _class_unloader = new RedjetClassUnloader(this);
  RedjetStringDedup::initialize();

  // Initialize scheduler mode (Domestic vs TBB mutual exclusion - PLAN.md Section 8.8.1)
  RedjetSchedulerMode sched_mode = RedjetSchedulerMode::DOMESTIC;
  if (RedjetGCScheduler != nullptr && strcasecmp(RedjetGCScheduler, "TBB") == 0) {
    sched_mode = RedjetSchedulerMode::TBB;
  }
  g_redjet_scheduler.init(os::processor_count(), sched_mode);

  // Initialize VFS memory tree & strict branch validation policies
  _vfs_tree.init();
  HS_hotspot_set_strict_vfs_branch_validation((RedjetVFSStrict || RedjetGCStrictVFS) ? 1 : 0);
  HS_hotspot_vfs_set_panic_dialog_enabled(RedjetVFSPanicDialog ? 1 : 0);

  // Initialize Isolated Redjet Compressed Spaces (narrowKlass in header_pane + isolated code_boot)
  size_t code_boot_req = RedjetFlags::config().integers.code_arena_bytes > 0
                             ? RedjetFlags::config().integers.code_arena_bytes
                             : RedjetCompressedSpaces::DEFAULT_CODE_BOOT_BYTES;
  RedjetCompressedSpaces::instance().initialize(
      reinterpret_cast<uintptr_t>(_virtual_space.low()),
      _virtual_space.reserved_size(),
      code_boot_req,
      RedjetCompressedSpaces::DEFAULT_HEADER_PANE_BYTES);
  _vfs_tree.record_domain_alloc(RedjetMemoryDomain::HOTSPOT_MEMORY, false, code_boot_req);

  // Initialize Dynamic SIMD-to-Scalar Contention Governor, BPU Director, Fast JNI/FFM, and Ring-3 Upclocker
  RedjetSIMDContentionGovernor::instance().initialize();
  RedjetBPUDirector::instance().reset();
  RedjetFastJNI::initialize();
  RedjetFastFFM::initialize();
  RedjetUpclocker::instance().init();
  if (RedjetGCUpclockerQoS || RedjetFlags::config().strings.upclocker_enabled) {
    RedjetUpclocker::instance().pin_max_frequency_warmup(0, RedjetUpclocker::DEFAULT_WARMUP_BURST_MS);
  }

  // Initialize oneTBB heterogeneous task arenas only if TBB scheduler is active
  // (PLAN.md Section 8.8.1: Strict mutual exclusion - TBB is 100% silenced under Domestic scheduler)
  if (sched_mode == RedjetSchedulerMode::TBB) {
    _tbb_arenas.init();
  }

  // Initialize runway allocator with initial capacity and bind to Strided Tensor Views & Primitiva
  _runway.init(max_byte_size / 4, RedjetForbidDynamicGrowth || RedjetSCMode);
  hs_tensor_views_bind_runway(&_runway);

  // Initialize Slab & Elastic TLAB Allocator
  _slab_allocator.init(max_byte_size / 4,
                       AlwaysPreTouch,
                       RedjetForbidDynamicGrowth || RedjetSCMode,
                       RedjetFlags::config().integers.freelist_recycle_policy,
                       RedjetFlags::config().integers.slab_chunk_size_bytes);
  _slab_allocator.set_falline(&_falline);

  // Initialize unified RedjetGC Big GC engine
  _gc.init(&_runway, &_motherline, &_falline, &_landfill);

  // Initialize default Pure-Mathematical RJ SIMD execution context (active out-of-the-box for Java math/SIMD)
  rj_config_t mcfg{};
  mcfg.profile                 = RedjetSCMode ? RJ_PROFILE_SAFETY_CRITICAL : RJ_PROFILE_STANDARD;
  mcfg.static_arena_size_bytes = RedjetSCMode ? MIN2(max_byte_size / 8, (size_t)(64ULL * 1024 * 1024)) : 0;
  mcfg.max_loop_iterations     = RedjetSCMode ? 1000000u : 0u;
  mcfg.forbid_runtime_heap     = RedjetSCMode || RedjetForbidDynamicGrowth;
  mcfg.user_static_arena_buf   = nullptr;
  rj_backend_t backend         = RedjetHeteroArenas ? RJ_BACKEND_TBB : RJ_BACKEND_AUTO;
  if (hs_init_with_config(&_msimd_ctx, RJ_ISA_AUTO, backend, &mcfg) != RJ_SUCCESS) {
    log_warning(gc)("RedjetGC RJ SIMD context initialization fell back to scalar/fallback tier");
  }

  // Initialize Seasonal Transitions Coordinator, OS Interrupt Shield, and Snatch & Rebate
  RedjetSeasonalCoordinator::initialize();
  RedjetOSInterruptShield::initialize();
  RedjetOSInterruptShield::register_falline_morgue_range((uintptr_t)_virtual_space.low(), _virtual_space.size());
  RedjetSnatchRebateEngine::initialize();

  // Install barrier set
  BarrierSet::set_barrier_set(new RedjetBarrierSet());

  RedjetInitLogger::print();

  return JNI_OK;
}

void RedjetHeap::initialize_serviceability() {
  _pool = new RedjetUnifiedHeapPool(this);
  _monitoring_support = new RedjetMonitoringSupport(this);
  _memory_manager.add_pool(_pool);

  // HotSpot Parity: GC Memory Manager and specialized MemoryPools (Unified, Runway, Hotfield, Landfill)
  _gc_memory_manager = new RedjetGCMemoryManager(this);
  _gc_memory_manager->initialize_pools();

  // HotSpot Parity: PerfData Policy Counters
  _policy_counters = new RedjetGCPolicyCounters(this);
}

GrowableArray<GCMemoryManager*> RedjetHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(2);
  memory_managers.append(&_memory_manager);
  if (_gc_memory_manager != nullptr) {
    memory_managers.append(_gc_memory_manager);
  }
  return memory_managers;
}

GrowableArray<MemoryPool*> RedjetHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(5);
  memory_pools.append(_pool);
  if (_gc_memory_manager != nullptr) {
    if (_gc_memory_manager->pool_unified() != nullptr) {
      memory_pools.append(_gc_memory_manager->pool_unified());
    }
    if (_gc_memory_manager->pool_runway() != nullptr) {
      memory_pools.append(_gc_memory_manager->pool_runway());
    }
    if (_gc_memory_manager->pool_hotfield() != nullptr) {
      memory_pools.append(_gc_memory_manager->pool_hotfield());
    }
    if (_gc_memory_manager->pool_landfill() != nullptr) {
      memory_pools.append(_gc_memory_manager->pool_landfill());
    }
  }
  return memory_pools;
}

size_t RedjetHeap::used() const {
  size_t space_used = _space != nullptr ? _space->used() : 0;
  return space_used + _slab_allocator.get_used_bytes();
}

HeapWord* RedjetHeap::allocate_work(size_t size) {
  assert(is_object_aligned(size), "Allocation size should be aligned: %zu", size);

  // 1. First attempt: Snatch from Falline emptybox cache (sub-microsecond O(1) snatch)
  RedjetEmptyBox* eb = _falline.snatch_emptybox(size * HeapWordSize, _current_sweep_no.load(std::memory_order_relaxed));
  if (eb != nullptr) {
    HeapWord* res = reinterpret_cast<HeapWord*>(eb);
    _allocated_bytes.fetch_add(size * HeapWordSize, std::memory_order_relaxed);
    _vfs_tree.record_domain_alloc(RedjetMemoryDomain::JAVA_MEMORY, true, size * HeapWordSize);
    log_trace(gc, alloc)("Allocated %zu bytes via Falline Snatch at " PTR_FORMAT, size * HeapWordSize, p2i(res));
    return res;
  }

  // 2. Second attempt: Bump allocate from committed contiguous space
  HeapWord* res = _space->par_allocate(size);
  if (res != nullptr) {
    _allocated_bytes.fetch_add(size * HeapWordSize, std::memory_order_relaxed);
    _vfs_tree.record_domain_alloc(RedjetMemoryDomain::JAVA_MEMORY, true, size * HeapWordSize);
    return res;
  }

  // 3. Third attempt: Expand heap if possible (strictly forbidden when RedjetForbidDynamicGrowth or RedjetSCMode is active)
  if (!RedjetForbidDynamicGrowth && !RedjetSCMode) {
    size_t expand_bytes = align_up(MAX2(size * HeapWordSize, (size_t)RedjetMinHeapExpand), SpaceAlignment);
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
    log_debug(gc, alloc)("Dynamic heap expansion forbidden under RedjetSCMode / RedjetForbidDynamicGrowth (%zu words requested)", size);
  }

  // 4. Fourth attempt: Execute RMRP reclamation and retry
  if (RedjetRMRP) {
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

HeapWord* RedjetHeap::mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) {
  *gc_overhead_limit_was_exceeded = false;
  size_t bytes = size * HeapWordSize;

  // Small objects (<= 256 KB) route to discrete Slab size classes
  if (bytes <= RedjetSlabAllocator::MAX_SLAB_SIZE) {
    void* p = _slab_allocator.allocate(bytes);
    if (p != nullptr) {
      _allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
      _vfs_tree.record_domain_alloc(RedjetMemoryDomain::JAVA_MEMORY, true, bytes);
      log_trace(gc, alloc)("Allocated %zu bytes via SlabAllocator at " PTR_FORMAT, bytes, p2i(p));
      return reinterpret_cast<HeapWord*>(p);
    }
  }

  // Large / Humongous objects (> 256 KB) bypass slab freelists and allocate directly from runway/virtual space
  return allocate_work(size);
}

HeapWord* RedjetHeap::allocate_new_tlab(size_t min_size, size_t requested_size, size_t* actual_size) {
  Thread* thread = Thread::current();
  size_t size = requested_size;

  if (RedjetElasticTLAB) {
    size_t ergo_size = RedjetThreadLocalData::ergo_tlab_size(thread);
    if (ergo_size != 0) {
      size = clamp(ergo_size, min_size, _max_tlab_size);
    }
  }

  size = align_object_size(size);

  // Route TLAB refills directly to RedjetSlabAllocator with SIMD zeroing and Falline remainder capture
  HeapWord* res = _slab_allocator.refill_tlab(min_size, size, actual_size);
  if (res == nullptr) {
    res = allocate_work(size);
    if (res != nullptr) {
      *actual_size = size;
    } else {
      *actual_size = 0;
    }
  }

  if (res != nullptr && RedjetElasticTLAB) {
    RedjetThreadLocalData::set_ergo_tlab_size(thread, (*actual_size) * 2);
  }

  return res;
}

size_t RedjetHeap::unsafe_max_tlab_alloc(Thread* thr) const {
  if (_space == nullptr) return 0;
  return MIN2(_max_tlab_size, _space->free() / HeapWordSize);
}

void RedjetHeap::collect(GCCause::Cause cause) {
  // Support PrimitiveRW bypass and deterministic Default bureaucratic collection phases
  if (RedjetSCMode) {
    if (RedjetSCGCType != nullptr && strcasecmp(RedjetSCGCType, "PrimitiveRW") == 0) {
      log_info(gc)("RedjetGC SC: PrimitiveRW bypass active - GC collection bypassed for cause '%s'", GCCause::to_string(cause));
      hs_unified_log_emit(HS_Ebin0_GC_SWEEP, HS_Ebin1_INFO, 0, "sc_bypass",
                          "RedjetGC SC: PrimitiveRW bypass active - GC collection bypassed");
      return;
    }

    // Deterministic Default bureaucratic collection phases
    log_info(gc)("RedjetGC SC: Deterministic Bureaucratic collection triggered: cause '%s'", GCCause::to_string(cause));
    hs_unified_log_emit(HS_Ebin0_GC_PHASE_CHANGE, HS_Ebin1_INFO, 0, "sc_bureaucratic",
                        "RedjetGC SC: Deterministic Bureaucratic Phase-STW collection active");
    _current_sweep_no.fetch_add(1, std::memory_order_relaxed);

    // Phase 1: STW Bureaucratic sweep & VFS hierarchy audit
    log_debug(gc)("RedjetGC SC Phase 1: Bureaucratic STW and VFS integrity validation");
    hs_gc_safepoint_rendezvous_wait();
    hs_gc_hookpoint_trigger(HS_HOOKPOINT_GC_PRE_SWEEP, 0, nullptr, 0);
    _gc.step();
    hs_gc_hookpoint_trigger(HS_HOOKPOINT_GC_POST_SWEEP, 0, nullptr, 0);

    // Phase 2: Falline emptybox cache deterministic sweep & coalesce
    log_debug(gc)("RedjetGC SC Phase 2: Falline emptybox deterministic cache audit");

    // Phase 3: RMRP deterministic reclamation if active
    if (RedjetRMRP) {
      execute_rmrp();
    }
    log_info(gc)("RedjetGC SC: Deterministic Bureaucratic collection phase completed (sweep %llu)",
                 (unsigned long long)_current_sweep_no.load(std::memory_order_relaxed));
    return;
  }

  uint64_t cycle_start_ns = sacculus_now_ns();
  uint64_t sweep_id = _current_sweep_no.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t used_before = used();
  size_t cap_before  = capacity();
  RedjetUnifiedLogger::instance().log_cycle_start(sweep_id, cause, used_before, cap_before);

  if (_gc_memory_manager != nullptr) {
    _gc_memory_manager->gc_cycle_begin(cause);
  }

  hs_gc_safepoint_rendezvous_wait();
  hs_gc_controlpoint_check(_allocated_bytes.load(std::memory_order_relaxed));
  g_hs_mltc_solver.check_and_solve(used_before, cap_before, 0.0f);
  hs_gc_hookpoint_trigger(HS_HOOKPOINT_GC_PRE_SWEEP, 0, nullptr, 0);

  // 1. HotSpot Root Scan & Live Object Graph Tracing
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Concurrent Root Scan & Mark");
    if (_ref_processor != nullptr) {
      _ref_processor->enable_discovery();
    }
    size_t traced = scan_roots_and_trace();
    phase_log.set_items(traced);
  }

  // 2. Reference Processing & Weak Root Cleaning
  if (_ref_processor != nullptr) {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Reference Processing");
    _ref_processor->process_discovered_references(false);
    _ref_processor->disable_discovery();
  }

  // 3. String Table and Symbol Table cleaning
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "String & Symbol Table Clean");
    RedjetStringTableCleaner::clean_tables();
  }

  // 4. Dynamic Class Unloading & Metaspace purge
  if (_class_unloader != nullptr) {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Class Unloading & HeaderPane Purge");
    _class_unloader->do_unloading();
  }

  // 5. Sweep unmarked memory into Falline / Landfill
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Falline EmptyBox Sweep");
    size_t swept_bytes = sweep_unmarked();
    phase_log.set_items(swept_bytes);
  }

  // 6. Step Redjet Big GC Engine
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Redjet Engine Step & Puzzler Coach");
    _gc.step();
    hs_gc_hookpoint_trigger(HS_HOOKPOINT_GC_POST_SWEEP, 0, nullptr, 0);
  }

  if (RedjetRMRP || RedjetGCRMRP) {
    RedjetGCPhaseLogScope phase_log(sweep_id, "RMRP Resident Coalesce");
    size_t rmrp_bytes = execute_rmrp();
    phase_log.set_items(rmrp_bytes);
  }

  // 7. Reset mark bitmap for next collection cycle
  _mark_bitmap.clear();
  drain_hotlog_to_coldlog(&_hotlog, &_coldlog);

  // 8. Update telemetry counters and end GC cycle
  if (_policy_counters != nullptr) {
    _policy_counters->update_counters();
  }
  if (_gc_memory_manager != nullptr) {
    _gc_memory_manager->gc_cycle_end();
  }

  RedjetUnifiedLogger::instance().log_vfs_domain_summary(sweep_id, this);
  RedjetUnifiedLogger::instance().log_cycle_end(
      sweep_id, cause, used_before, used(), capacity(), sacculus_now_ns() - cycle_start_ns);
}

void RedjetHeap::do_full_collection(bool clear_all_soft_refs) {
  uint64_t cycle_start_ns = sacculus_now_ns();
  uint64_t sweep_id = _current_sweep_no.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t used_before = used();
  RedjetUnifiedLogger::instance().log_cycle_start(sweep_id, GCCause::_full_gc_alot, used_before, capacity());

  if (_gc_memory_manager != nullptr) {
    _gc_memory_manager->gc_cycle_begin(GCCause::_full_gc_alot);
  }
  hs_gc_safepoint_rendezvous_wait();
  hs_gc_hookpoint_trigger(HS_HOOKPOINT_GC_PRE_COMPACT, 0, nullptr, 0);

  // 1. HotSpot Root Scan & Live Object Graph Tracing
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Full Root Scan & Mark");
    if (_ref_processor != nullptr) {
      _ref_processor->enable_discovery();
    }
    size_t traced = scan_roots_and_trace();
    phase_log.set_items(traced);
  }

  // 2. Reference Processing with full clear
  if (_ref_processor != nullptr) {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Full Reference Processing");
    _ref_processor->process_discovered_references(clear_all_soft_refs);
    _ref_processor->disable_discovery();
  }

  // 3. String Table and Symbol Table cleaning
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Full String & Symbol Clean");
    RedjetStringTableCleaner::clean_tables();
  }

  // 4. Dynamic Class Unloading & Metaspace purge
  if (_class_unloader != nullptr) {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Full Class Unloading");
    _class_unloader->do_unloading();
  }

  // 5. Full Compaction / Shake Bed
  {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Full Shake-Bed Compaction");
    _gc.shake_bed();
    hs_gc_hookpoint_trigger(HS_HOOKPOINT_GC_POST_COMPACT, 0, nullptr, 0);
  }

  if (RedjetRMRP || RedjetGCRMRP) {
    RedjetGCPhaseLogScope phase_log(sweep_id, "Full RMRP Resident Coalesce");
    size_t rmrp_bytes = execute_rmrp();
    phase_log.set_items(rmrp_bytes);
  }

  // 6. Reset mark bitmap
  _mark_bitmap.clear();
  drain_hotlog_to_coldlog(&_hotlog, &_coldlog);

  // 7. Update telemetry counters and end GC cycle
  if (_policy_counters != nullptr) {
    _policy_counters->update_counters();
  }
  if (_gc_memory_manager != nullptr) {
    _gc_memory_manager->gc_cycle_end();
  }

  RedjetUnifiedLogger::instance().log_vfs_domain_summary(sweep_id, this);
  RedjetUnifiedLogger::instance().log_cycle_end(
      sweep_id, GCCause::_full_gc_alot, used_before, used(), capacity(), sacculus_now_ns() - cycle_start_ns);
}

size_t RedjetHeap::scan_roots_and_trace() {
  if (_root_scanner != nullptr) {
    return _root_scanner->scan_roots_and_trace();
  }
  return 0;
}

size_t RedjetHeap::sweep_unmarked() {
  size_t reclaimed_bytes = 0;
  if (_space != nullptr) {
    HeapWord* cur = _space->bottom();
    HeapWord* top = _space->top();
    while (cur < top) {
      oop obj = cast_to_oop(cur);
      size_t obj_size_words = obj->size();
      size_t obj_size_bytes = obj_size_words * HeapWordSize;
      if (!_mark_bitmap.is_marked(cur)) {
        if (obj_size_bytes >= sizeof(RedjetEmptyBox)) {
          _falline.register_emptybox(obj_size_bytes,
                                     reinterpret_cast<RedjetEmptyBox*>(cur),
                                     _current_sweep_no.load(std::memory_order_relaxed));
          _vfs_tree.record_domain_free(RedjetMemoryDomain::JAVA_MEMORY, true, obj_size_bytes);
          reclaimed_bytes += obj_size_bytes;
        }
      }
      cur += obj_size_words;
    }
  }
  return reclaimed_bytes;
}

void RedjetHeap::oops_do(OopClosure* cl) {
  if (_root_scanner != nullptr) {
    _root_scanner->roots_do(cl);
  }
}

void RedjetHeap::object_iterate(ObjectClosure* cl) {
  if (_space != nullptr) {
    _space->object_iterate(cl);
  }
}

void RedjetHeap::verify(VerifyOption option) {
  (void)RedjetHeapVerifier::verify_heap(this, option);
}

void RedjetHeap::print_heap_on(outputStream* st) const {
  st->print_cr("Redjet Heap (%s)", (RedjetSCMode || RedjetGCSCMode) ? "SSIOLSC Safety-Critical" : "NORMAX Dynamic SIMD");
  StreamIndentor si(st, 1);
  _virtual_space.print_on(st);
  if (_space != nullptr) {
    st->print_cr("Allocation space:");
    StreamIndentor si2(st, 1);
    _space->print_on(st, "");
  }
  _falline.print_on(st);
  _vfs_tree.print_domain_summary_on(st);
  RedjetCompressedSpaces::instance().print_on(st);
}

void RedjetHeap::print_gc_on(outputStream* st) const {
  RedjetUnifiedLogger::instance().print_statistics_summary(st, this);
}

bool RedjetHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<RedjetHeap>::print_location(st, addr);
}

void RedjetHeap::print_tracing_info() const {
  size_t reserved  = max_capacity();
  size_t committed = capacity();
  size_t cur_used  = used();
  if (reserved != 0) {
    log_info(gc)("Redjet Heap: %zu%s reserved, %zu%s (%.2f%%) committed, %zu%s (%.2f%%) used",
                 byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
                 byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
                 committed * 100.0 / reserved,
                 byte_size_in_proper_unit(cur_used),  proper_unit_for_byte_size(cur_used),
                 cur_used * 100.0 / reserved);
  }
  LogTarget(Info, gc, stats) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    RedjetUnifiedLogger::instance().print_statistics_summary(&ls, this);
  }
}

size_t RedjetHeap::execute_rmrp() {
  log_debug(gc, heap)("Executing RMRP memory resident reclamation (method: %s)", RedjetRMRPMethod);
  hs_rmrp_method_e method =
      (RedjetRMRPMethod != nullptr && strcasecmp(RedjetRMRPMethod, "Sequential") == 0)
          ? hs_rmrp_method_e::SEQUENTIAL
          : hs_rmrp_method_e::PLUMP;

  size_t count = _falline.emptybox_count();
  size_t page_sz = os::vm_page_size();
  size_t target_bytes = (count > 0) ? (count * page_sz) : page_sz;
  size_t reclaimed_bytes = RedjetRMRP::execute_reclamation(method, target_bytes);
  if (reclaimed_bytes > 0) {
    hs_snatch_rebate_mode_e rmode = (method == hs_rmrp_method_e::SEQUENTIAL)
                                        ? hs_snatch_rebate_mode_e::RAPID
                                        : hs_snatch_rebate_mode_e::DECOMMIT;
    _runway.allocator.rebate_pages(rmode, reclaimed_bytes);
    _slab_allocator.rebate_pages(rmode, reclaimed_bytes);
  }
  log_info(gc, heap)("RMRP scan: %zu emptyboxes in Falline cache, reclaimed %zu bytes", count, reclaimed_bytes);

  return reclaimed_bytes;
}
