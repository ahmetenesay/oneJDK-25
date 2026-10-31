/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact WLFT or visit https://github.com/ahmetenesay/oneJDK-25
 * if you need additional information or have any questions.
 */

#ifdef REDLET_STANDALONE_BUILD
#include "gc/redlet/redletFlags.hpp"
#include "gc/redlet/redletInitLogger.hpp"
#include <cstdio>
#define EXACTFMT "%zu"
#define EXACTFMTARGS(x) (x)
#define tty nullptr
#define UseTLAB false
#define HeapWordSize 8
static const bool RedletVFSStrict = true;
static const bool RedletRMRP = true;
static const char* RedletRMRPMethod = "Plump";
static const double RedletFallineCompactRatio = 0.3;
static const char* RedletSIMD = "AVX2";
static const bool RedletHeteroArenas = true;
static const char* RedletGPUOffload = "Disabled";
class RedletHeap {
public:
  static RedletHeap* heap() { return nullptr; }
  size_t max_tlab_size() const { return 0; }
};
#else
#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletFlags.hpp"
#include "gc/redlet/redletInitLogger.hpp"
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#endif

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

  RedletFlags::print_on(tty);
}

void RedletInitLogger::print() {
  RedletInitLogger logger;
  logger.print_all();
}
