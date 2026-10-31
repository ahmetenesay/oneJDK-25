/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_SSIOLSC_REDLETSSIOLSCHEURISTICS_HPP
#define SHARE_GC_REDLET_SSIOLSC_REDLETSSIOLSCHEURISTICS_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"

class RedletSSIOLSCHeuristics {
private:
  SF_custominiVm_scgc_type_t _scgc_type{ SF_SCGC_TYPE_DEFAULT };
  std::atomic<size_t>        _allocated_bytes{ 0 };
  std::atomic<uint64_t>      _cycle_count{ 0 };
  size_t                     _fixed_tlab_size{ 4 * 1024 * 1024 };

public:
  RedletSSIOLSCHeuristics(SF_custominiVm_scgc_type_t scgc = SF_SCGC_TYPE_DEFAULT);
  virtual ~RedletSSIOLSCHeuristics() = default;

  virtual const char* name() const { return "SSIOLSC"; }
  virtual bool should_start_gc() const;
  virtual bool is_dynamic_growth_allowed() const { return false; } // Zero-jitter hard guarantee
  virtual size_t max_tlab_size() const { return _fixed_tlab_size; }
  virtual bool is_primitive_rw() const { return _scgc_type == SF_SCGC_TYPE_PRIMITIVE_RW; }
  virtual void record_allocation(size_t bytes);
  virtual void record_gc_end();
};

#endif // SHARE_GC_REDLET_SSIOLSC_REDLETSSIOLSCHEURISTICS_HPP
