/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_NORMAX_REDLETNORMAXMODE_HPP
#define SHARE_GC_REDLET_NORMAX_REDLETNORMAXMODE_HPP

class RedletNORMAXHeuristics;

class RedletNORMAXMode {
public:
  RedletNORMAXMode() = default;
  virtual ~RedletNORMAXMode() = default;

  virtual void initialize_flags() const;
  virtual RedletNORMAXHeuristics* initialize_heuristics() const;
  virtual const char* name() const { return "NORMAX"; }
  virtual bool is_safety_critical() const { return false; }
};

#endif // SHARE_GC_REDLET_NORMAX_REDLETNORMAXMODE_HPP
