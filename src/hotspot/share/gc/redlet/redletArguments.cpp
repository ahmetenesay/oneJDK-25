/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/redletArguments.hpp"
#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redlet_globals.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

size_t RedletArguments::conservative_max_heap_alignment() {
  return UseLargePages ? os::large_page_size() : os::vm_page_size();
}

void RedletArguments::initialize() {
  GCArguments::initialize();

  assert(UseRedletGC, "Sanity");

  if (RedletMaxTLABSize < MinTLABSize) {
    log_warning(gc)("RedletMaxTLABSize < MinTLABSize, adjusting it to %zu", MinTLABSize);
    RedletMaxTLABSize = MinTLABSize;
  }

#ifdef COMPILER2
  if (FLAG_IS_DEFAULT(UseCountedLoopSafepoints)) {
    FLAG_SET_DEFAULT(UseCountedLoopSafepoints, true);
    if (FLAG_IS_DEFAULT(LoopStripMiningIter)) {
      FLAG_SET_DEFAULT(LoopStripMiningIter, 1000);
    }
  }
#endif
}

void RedletArguments::initialize_alignments() {
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  size_t align = MAX2(os::vm_allocation_granularity(), page_size);
  SpaceAlignment = align;
  HeapAlignment  = align;
}

CollectedHeap* RedletArguments::create_heap() {
  return new RedletHeap();
}
