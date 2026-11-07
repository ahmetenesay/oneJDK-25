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
#include "gc/redjet/runtime/redjetMonitoringSupport.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/allocator/redjet_rmrp.hpp"
#include "gc/shared/generationCounters.hpp"
#include "memory/allocation.hpp"
#include "memory/metaspaceCounters.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/perfData.hpp"
#include "services/memoryService.hpp"

class RedjetSpaceCounters : public CHeapObj<mtGC> {
private:
  PerfVariable* _capacity;
  PerfVariable* _used;
  char*         _name_space;

public:
  RedjetSpaceCounters(const char* name,
                      int ordinal,
                      size_t max_size,
                      size_t initial_capacity,
                      GenerationCounters* gc) {
    if (UsePerfData) {
      EXCEPTION_MARK;
      ResourceMark rm;

      const char* cns = PerfDataManager::name_space(gc->name_space(), "space", ordinal);
      _name_space = NEW_C_HEAP_ARRAY(char, strlen(cns) + 1, mtGC);
      strcpy(_name_space, cns);

      const char* cname = PerfDataManager::counter_name(_name_space, "name");
      PerfDataManager::create_string_constant(SUN_GC, cname, name, CHECK);

      cname = PerfDataManager::counter_name(_name_space, "maxCapacity");
      PerfDataManager::create_constant(SUN_GC, cname, PerfData::U_Bytes, (jlong)max_size, CHECK);

      cname = PerfDataManager::counter_name(_name_space, "capacity");
      _capacity = PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, (jlong)initial_capacity, CHECK);

      cname = PerfDataManager::counter_name(_name_space, "used");
      _used = PerfDataManager::create_variable(SUN_GC, cname, PerfData::U_Bytes, (jlong)0, CHECK);

      cname = PerfDataManager::counter_name(_name_space, "initCapacity");
      PerfDataManager::create_constant(SUN_GC, cname, PerfData::U_Bytes, (jlong)initial_capacity, CHECK);
    }
  }

  ~RedjetSpaceCounters() {
    if (_name_space != nullptr) {
      FREE_C_HEAP_ARRAY(char, _name_space);
    }
  }

  void update_all(size_t capacity, size_t used) {
    _capacity->set_value(capacity);
    _used->set_value(used);
  }
};

class RedjetGenerationCounters : public GenerationCounters {
private:
  RedjetHeap* _heap;
public:
  RedjetGenerationCounters(RedjetHeap* heap) :
    GenerationCounters("RedjetHeap", 1, 1, 0, heap->max_capacity(), heap->capacity()),
    _heap(heap) {}

  void update_all() {
    GenerationCounters::update_all(_heap->capacity());
  }
};

RedjetMonitoringSupport::RedjetMonitoringSupport(RedjetHeap* heap) {
  _heap_counters  = new RedjetGenerationCounters(heap);
  _space_counters = new RedjetSpaceCounters("Heap", 0, heap->max_capacity(), 0, _heap_counters);

  _emptybox_created      = nullptr;
  _emptybox_claimed      = nullptr;
  _emptybox_timeouts     = nullptr;
  _emptybox_active_bytes = nullptr;

  _mltc_bandwidth        = nullptr;
  _mltc_congestion_score = nullptr;
  _mltc_paced_allocs     = nullptr;
  _mltc_stall_ns         = nullptr;
  _mltc_leak_tier        = nullptr;

  _rmrp_plump_reclaimed  = nullptr;
  _rmrp_seq_reclaimed    = nullptr;
  _rmrp_vfs_reclaimed    = nullptr;
  _rmrp_zero_dedup       = nullptr;
  _rmrp_vfs_triggers     = nullptr;

  if (UsePerfData) {
    EXCEPTION_MARK;
    ResourceMark rm;

    // EmptyBox live telemetry counters
    _emptybox_created      = PerfDataManager::create_variable(SUN_GC, "redjet.emptybox.reservationsCreated", PerfData::U_Events, (jlong)0, CHECK);
    _emptybox_claimed      = PerfDataManager::create_variable(SUN_GC, "redjet.emptybox.reservationsClaimed", PerfData::U_Events, (jlong)0, CHECK);
    _emptybox_timeouts     = PerfDataManager::create_variable(SUN_GC, "redjet.emptybox.timeoutsDetected", PerfData::U_Events, (jlong)0, CHECK);
    _emptybox_active_bytes = PerfDataManager::create_variable(SUN_GC, "redjet.emptybox.activeReservedBytes", PerfData::U_Bytes, (jlong)0, CHECK);

    // MLTC live telemetry counters
    _mltc_bandwidth        = PerfDataManager::create_variable(SUN_GC, "redjet.mltc.bandwidthMbps", PerfData::U_None, (jlong)0, CHECK);
    _mltc_congestion_score = PerfDataManager::create_variable(SUN_GC, "redjet.mltc.congestionScore", PerfData::U_None, (jlong)0, CHECK);
    _mltc_paced_allocs     = PerfDataManager::create_variable(SUN_GC, "redjet.mltc.pacedAllocations", PerfData::U_Events, (jlong)0, CHECK);
    _mltc_stall_ns         = PerfDataManager::create_variable(SUN_GC, "redjet.mltc.stallDelayNs", PerfData::U_None, (jlong)0, CHECK);
    _mltc_leak_tier        = PerfDataManager::create_variable(SUN_GC, "redjet.mltc.leakTier", PerfData::U_None, (jlong)0, CHECK);

    // RMRP live telemetry counters
    _rmrp_plump_reclaimed  = PerfDataManager::create_variable(SUN_GC, "redjet.rmrp.plumpReclaimedBytes", PerfData::U_Bytes, (jlong)0, CHECK);
    _rmrp_seq_reclaimed    = PerfDataManager::create_variable(SUN_GC, "redjet.rmrp.sequentialReclaimedBytes", PerfData::U_Bytes, (jlong)0, CHECK);
    _rmrp_vfs_reclaimed    = PerfDataManager::create_variable(SUN_GC, "redjet.rmrp.vfsReclaimedBytes", PerfData::U_Bytes, (jlong)0, CHECK);
    _rmrp_zero_dedup       = PerfDataManager::create_variable(SUN_GC, "redjet.rmrp.zeroDeduplicatedBytes", PerfData::U_Bytes, (jlong)0, CHECK);
    _rmrp_vfs_triggers     = PerfDataManager::create_variable(SUN_GC, "redjet.rmrp.vfsCompactionTriggers", PerfData::U_Events, (jlong)0, CHECK);
  }
}

void RedjetMonitoringSupport::update_counters() {
  MemoryService::track_memory_usage();

  if (UsePerfData) {
    RedjetHeap* heap = RedjetHeap::heap();
    size_t used = heap->used();
    size_t capacity = heap->capacity();
    ((RedjetGenerationCounters*)_heap_counters)->update_all();
    _space_counters->update_all(capacity, used);

    // Update EmptyBox live telemetry
    RedjetEmptyBoxTelemetry eb = RedjetEmptyBoxManager::get_telemetry();
    if (_emptybox_created != nullptr) _emptybox_created->set_value((jlong)eb.total_reservations_created);
    if (_emptybox_claimed != nullptr) _emptybox_claimed->set_value((jlong)eb.total_reservations_claimed);
    if (_emptybox_timeouts != nullptr) _emptybox_timeouts->set_value((jlong)eb.total_timeouts_detected);
    if (_emptybox_active_bytes != nullptr) _emptybox_active_bytes->set_value((jlong)eb.active_reserved_bytes);

    // Update MLTC live telemetry
    if (_mltc_bandwidth != nullptr) _mltc_bandwidth->set_value((jlong)g_hs_mltc_solver.velocity_mbps);
    if (_mltc_congestion_score != nullptr) _mltc_congestion_score->set_value((jlong)(g_hs_mltc_solver.latest_snapshot.congestion_score * 100.0f));
    RedjetMLTCPacer& pacer = heap->mltc_pacer();
    RedjetMLTCLeakDetector& leak = heap->mltc_leak_detector();
    if (_mltc_paced_allocs != nullptr) _mltc_paced_allocs->set_value((jlong)pacer.total_paced_allocations());
    if (_mltc_stall_ns != nullptr) _mltc_stall_ns->set_value((jlong)pacer.total_stall_delay_ns());
    if (_mltc_leak_tier != nullptr) _mltc_leak_tier->set_value((jlong)leak.current_risk_tier());

    // Update RMRP live telemetry
    if (_rmrp_plump_reclaimed != nullptr) _rmrp_plump_reclaimed->set_value((jlong)RedjetRMRP::total_plump_reclaimed());
    if (_rmrp_seq_reclaimed != nullptr) _rmrp_seq_reclaimed->set_value((jlong)RedjetRMRP::total_sequential_reclaimed());
    if (_rmrp_vfs_reclaimed != nullptr) _rmrp_vfs_reclaimed->set_value((jlong)RedjetRMRP::total_vfs_reclaimed());
    if (_rmrp_zero_dedup != nullptr) _rmrp_zero_dedup->set_value((jlong)RedjetRMRP::total_zero_bytes_deduplicated());
    if (_rmrp_vfs_triggers != nullptr) _rmrp_vfs_triggers->set_value((jlong)RedjetRMRP::total_vfs_compactions());

    MetaspaceCounters::update_performance_counters();
  }
}
