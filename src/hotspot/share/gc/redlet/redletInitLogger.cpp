/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/redletFlags.hpp"
#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletInitLogger.hpp"
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/globalDefinitions.hpp"

void RedletInitLogger::print_gc_specific() {
  log_info(gc, init)("RedletGC Engine: Active (Ultra-Low Latency Generative & Vectorized GC)");
  log_info(gc, init)("Redlet VFS Memory Tree: StrictValidation=%s, RMRP=%s (%s)",
                     RedletVFSStrict ? "true" : "false",
                     RedletRMRP ? "true" : "false",
                     RedletRMRPMethod);
  log_info(gc, init)("Redlet Falline: CompactRatio=%.2f", RedletFallineCompactRatio);
  log_info(gc, init)("Redlet SIMD Tracing: %s", RedletSIMD);
  log_info(gc, init)("Redlet oneTBB Heterogeneous Arenas: %s", RedletHeteroArenas ? "Enabled (P-Core/E-Core aware)" : "Disabled");
  log_info(gc, init)("Redlet GPU Offload Engine: %s", RedletGPUOffload);

  if (UseTLAB) {
    size_t max_tlab = RedletHeap::heap()->max_tlab_size() * HeapWordSize;
    log_info(gc, init)("TLAB Size Max: " EXACTFMT, EXACTFMTARGS(max_tlab));
  }

  RedletFlags::print_on(defaultStream::output_stream());
}

void RedletInitLogger::print() {
  RedletInitLogger logger;
  logger.init();
}
