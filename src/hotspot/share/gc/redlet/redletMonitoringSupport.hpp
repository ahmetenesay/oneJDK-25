/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETMONITORINGSUPPORT_HPP
#define SHARE_GC_REDLET_REDLETMONITORINGSUPPORT_HPP

#include "memory/allocation.hpp"

class GenerationCounters;
class HSpaceCounter;
class RedletHeap;

class RedletMonitoringSupport : public CHeapObj<mtGC> {
private:
  GenerationCounters* _heap_counters;
  HSpaceCounter*      _space_counters;

public:
  RedletMonitoringSupport(RedletHeap* heap);
  void update_counters();
};

#endif // SHARE_GC_REDLET_REDLETMONITORINGSUPPORT_HPP
