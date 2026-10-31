/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETARGUMENTS_HPP
#define SHARE_GC_REDLET_REDLETARGUMENTS_HPP

#include "gc/shared/gcArguments.hpp"

class CollectedHeap;

class RedletArguments : public GCArguments {
private:
  void initialize_alignments() override;
  void initialize() override;
  size_t conservative_max_heap_alignment() override;
  CollectedHeap* create_heap() override;
};

#endif // SHARE_GC_REDLET_REDLETARGUMENTS_HPP
