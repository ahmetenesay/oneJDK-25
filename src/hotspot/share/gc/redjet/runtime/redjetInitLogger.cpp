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

#ifdef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjetInitLogger.hpp"
#include <cstdio>
#define EXACTFMT "%zu"
#define EXACTFMTARGS(x) (x)
#define tty nullptr
#define UseTLAB false
#define HeapWordSize 8
static const bool RedjetVFSStrict = true;
static const bool RedjetRMRP = true;
static const char* RedjetRMRPMethod = "Plump";
static const double RedjetFallineCompactRatio = 0.3;
static const char* RedjetSIMD = "AVX2";
static const bool RedjetHeteroArenas = true;
static const char* RedjetGPUOffload = "Disabled";
class RedjetHeap {
public:
  static RedjetHeap* heap() { return nullptr; }
  size_t max_tlab_size() const { return 0; }
};
#else
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjetInitLogger.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#endif

void RedjetInitLogger::print_gc_specific() {
  log_info(gc, init)("RedjetGC Engine: Active (Ultra-Low Latency Generative & Vectorized GC)");
  log_info(gc, init)("Redjet VFS Memory Tree: StrictValidation=%s, RMRP=%s (%s)",
                     RedjetVFSStrict ? "true" : "false",
                     RedjetRMRP ? "true" : "false",
                     RedjetRMRPMethod);
  log_info(gc, init)("Redjet Falline: CompactRatio=%.2f", RedjetFallineCompactRatio);
  log_info(gc, init)("Redjet SIMD Tracing: %s", RedjetSIMD);
  log_info(gc, init)("Redjet oneTBB Heterogeneous Arenas: %s", RedjetHeteroArenas ? "Enabled (P-Core/E-Core aware)" : "Disabled");
  log_info(gc, init)("Redjet GPU Offload Engine: %s", RedjetGPUOffload);

  if (UseTLAB) {
    size_t max_tlab = RedjetHeap::heap()->max_tlab_size() * HeapWordSize;
    log_info(gc, init)("TLAB Size Max: " EXACTFMT, EXACTFMTARGS(max_tlab));
  }

  RedjetFlags::print_on(tty);
}

void RedjetInitLogger::print() {
  RedjetInitLogger logger;
  logger.print_all();
}
