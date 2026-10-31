/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/redletArguments.hpp"
#include "gc/redlet/redletFlags.hpp"
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

  RedletFlags::init_defaults();
  if (RedletOptions != nullptr && RedletOptions[0] != '\0') {
    RedletFlags::parse_flag_sequence(RedletOptions);
  }

  if (RedletMaxTLABSize < MinTLABSize) {
    log_warning(gc)("RedletMaxTLABSize < MinTLABSize, adjusting it to %zu", MinTLABSize);
    RedletMaxTLABSize = MinTLABSize;
  }

  // Evaluate execution profile: NORMAX (Dynamic SIMD) vs SSIOLSC (Safety-Critical Deterministic)
  bool is_ssiolsc = RedletSCMode ||
                    (RedletProfile != nullptr && (strcasecmp(RedletProfile, "SSIOLSC") == 0 ||
                                                  strcasecmp(RedletProfile, "SC") == 0 ||
                                                  strcasecmp(RedletProfile, "SafetyCritical") == 0));

  if (is_ssiolsc) {
    if (FLAG_IS_DEFAULT(RedletSCMode)) {
      FLAG_SET_ERGO(RedletSCMode, true);
    }
    if (FLAG_IS_DEFAULT(AlwaysPreTouch)) {
      FLAG_SET_ERGO(AlwaysPreTouch, true);
    }
    if (FLAG_IS_DEFAULT(RedletForbidDynamicGrowth)) {
      FLAG_SET_ERGO(RedletForbidDynamicGrowth, true);
    }
    if (FLAG_IS_DEFAULT(RedletVFSStrict)) {
      FLAG_SET_ERGO(RedletVFSStrict, true);
    }
    log_info(gc)("RedletGC Safety-Critical (SSIOLSC) Profile engaged:");
    log_info(gc)("  - Hard real-time determinism active (replacing soft heuristics)");
    log_info(gc)("  - Page pre-touch (AlwaysPreTouch) enforced to prevent page faults");
    log_info(gc)("  - Dynamic heap expansion forbidden (Zero-jitter execution envelope)");
    log_info(gc)("  - Strict VFS branch validation and memory isolation enforced");
  } else {
    log_info(gc)("RedletGC NORMAX Profile engaged: Dynamic SIMD throughput with oneTBB heterogeneous work-stealing.");
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
