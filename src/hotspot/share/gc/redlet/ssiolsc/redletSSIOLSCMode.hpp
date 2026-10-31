/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_SSIOLSC_REDLETSSIOLSCMODE_HPP
#define SHARE_GC_REDLET_SSIOLSC_REDLETSSIOLSCMODE_HPP

#include "gc/redlet/redletGearkit.hpp"

class RedletSSIOLSCHeuristics;

class RedletSSIOLSCMode {
private:
  SF_custominiVm_scgc_type_t _scgc_type{ SF_SCGC_TYPE_DEFAULT };

public:
  RedletSSIOLSCMode(SF_custominiVm_scgc_type_t scgc = SF_SCGC_TYPE_DEFAULT);
  virtual ~RedletSSIOLSCMode() = default;

  virtual void initialize_flags() const;
  virtual RedletSSIOLSCHeuristics* initialize_heuristics() const;
  virtual const char* name() const { return "SSIOLSC"; }
  virtual bool is_safety_critical() const { return true; }
  SF_custominiVm_scgc_type_t scgc_type() const { return _scgc_type; }
};

#endif // SHARE_GC_REDLET_SSIOLSC_REDLETSSIOLSCMODE_HPP
