/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/normax/redletNORMAXHeuristics.hpp"
#include "gc/redlet/redletMLTC.hpp"
#include "gc/redlet/redletRunway.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#endif

RedletNORMAXHeuristics::RedletNORMAXHeuristics() {
  _max_tlab_size = 4 * 1024 * 1024;
}

bool RedletNORMAXHeuristics::should_start_gc() const {
  // Check if MLTC detects upward memory traffic trend
  if (g_sf_mltc_solver.continuous_upward_trend.load(std::memory_order_relaxed)) {
    return true;
  }
  return false;
}

void RedletNORMAXHeuristics::record_allocation(size_t bytes) {
  _allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RedletNORMAXHeuristics::record_gc_end() {
  _cycle_count.fetch_add(1, std::memory_order_relaxed);
  _allocated_bytes.store(0, std::memory_order_relaxed);
}
