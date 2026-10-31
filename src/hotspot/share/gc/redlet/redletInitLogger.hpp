/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETINITLOGGER_HPP
#define SHARE_GC_REDLET_REDLETINITLOGGER_HPP

#include "gc/shared/gcInitLogger.hpp"

class RedletInitLogger : public GCInitLogger {
protected:
  void print_gc_specific() override;

public:
  static void print();
};

#endif // SHARE_GC_REDLET_REDLETINITLOGGER_HPP
