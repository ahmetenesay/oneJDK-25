/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/ssiolsc/redletSSIOLSCHeuristics.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#endif

RedletSSIOLSCHeuristics::RedletSSIOLSCHeuristics(SF_custominiVm_scgc_type_t scgc)
  : _scgc_type(scgc) {
  _fixed_tlab_size = 4 * 1024 * 1024;
}

bool RedletSSIOLSCHeuristics::should_start_gc() const {
  // In SSIOLSC deterministic safety-critical mode:
  // PrimitiveRW bypasses standard GC; Default mode uses bounded STW
  if (is_primitive_rw()) {
    return false;
  }
  return false;
}

void RedletSSIOLSCHeuristics::record_allocation(size_t bytes) {
  _allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RedletSSIOLSCHeuristics::record_gc_end() {
  _cycle_count.fetch_add(1, std::memory_order_relaxed);
  _allocated_bytes.store(0, std::memory_order_relaxed);
}
