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
#include "gc/redlet/redletMonitoringSupport.hpp"
#include "gc/shared/generationCounters.hpp"
#include "memory/allocation.hpp"
#include "memory/metaspaceCounters.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/perfData.hpp"
#include "services/memoryService.hpp"

class RedletSpaceCounters : public CHeapObj<mtGC> {
private:
  PerfVariable* _capacity;
  PerfVariable* _used;
  char*         _name_space;

public:
  RedletSpaceCounters(const char* name,
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

  ~RedletSpaceCounters() {
    if (_name_space != nullptr) {
      FREE_C_HEAP_ARRAY(char, _name_space);
    }
  }

  void update_all(size_t capacity, size_t used) {
    _capacity->set_value(capacity);
    _used->set_value(used);
  }
};

class RedletGenerationCounters : public GenerationCounters {
private:
  RedletHeap* _heap;
public:
  RedletGenerationCounters(RedletHeap* heap) :
    GenerationCounters("RedletHeap", 1, 1, 0, heap->max_capacity(), heap->capacity()),
    _heap(heap) {}

  void update_all() {
    GenerationCounters::update_all(_heap->capacity());
  }
};

RedletMonitoringSupport::RedletMonitoringSupport(RedletHeap* heap) {
  _heap_counters  = new RedletGenerationCounters(heap);
  _space_counters = (HSpaceCounter*)new RedletSpaceCounters("Heap", 0, heap->max_capacity(), 0, _heap_counters);
}

void RedletMonitoringSupport::update_counters() {
  MemoryService::track_memory_usage();

  if (UsePerfData) {
    RedletHeap* heap = RedletHeap::heap();
    size_t used = heap->used();
    size_t capacity = heap->capacity();
    ((RedletGenerationCounters*)_heap_counters)->update_all();
    ((RedletSpaceCounters*)_space_counters)->update_all(capacity, used);
    MetaspaceCounters::update_performance_counters();
  }
}
