/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_NORMAX_REDLETNORMAXHEURISTICS_HPP
#define SHARE_GC_REDLET_NORMAX_REDLETNORMAXHEURISTICS_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>

class RedletNORMAXHeuristics {
private:
  std::atomic<size_t>   _allocated_bytes{ 0 };
  std::atomic<uint64_t> _cycle_count{ 0 };
  size_t                _max_tlab_size{ 4 * 1024 * 1024 };

public:
  RedletNORMAXHeuristics();
  virtual ~RedletNORMAXHeuristics() = default;

  virtual const char* name() const { return "NORMAX"; }
  virtual bool should_start_gc() const;
  virtual bool is_dynamic_growth_allowed() const { return true; }
  virtual size_t max_tlab_size() const { return _max_tlab_size; }
  virtual void record_allocation(size_t bytes);
  virtual void record_gc_end();
};

#endif // SHARE_GC_REDLET_NORMAX_REDLETNORMAXHEURISTICS_HPP
