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

#ifndef SHARE_GC_REDLET_REDLET_GLOBALS_HPP
#define SHARE_GC_REDLET_REDLET_GLOBALS_HPP

#include "runtime/globals_shared.hpp"

//
// Defines all global flags used by RedletGC.
//

#define GC_REDLET_FLAGS(develop,                                             \
                        develop_pd,                                          \
                        product,                                             \
                        product_pd,                                          \
                        range,                                               \
                        constraint)                                          \
                                                                             \
  product(ccstr, RedletOptions, "", EXPERIMENTAL,                            \
          "Direct Redlet flag sequence or intent keywords "                  \
          "(e.g. 'turbo,trim,strict_vfs,vector').")                          \
                                                                             \
  product(bool, RedletVFSStrict, true, EXPERIMENTAL,                         \
          "Enforce strict VFS branch validation and fail-fast protection "   \
          "against missing VFS branches.")                                   \
                                                                             \
  product(bool, RedletVFSPanicDialog, false, EXPERIMENTAL,                   \
          "Enable modal alert dialog popup on fatal VFS panics.")            \
                                                                             \
  product(bool, RedletRMRP, true, EXPERIMENTAL,                              \
          "Enable Reduce Memory Resident Set Reclamation (RMRP).")           \
                                                                             \
  product(ccstr, RedletRMRPMethod, "Plump", EXPERIMENTAL,                    \
          "RMRP memory reclamation method: Plump or Sequential.")            \
                                                                             \
  product(double, RedletFallineCompactRatio, 0.30, EXPERIMENTAL,             \
          "Falline compaction threshold ratio (0.05 to 0.95).")              \
          range(0.05, 0.95)                                                  \
                                                                             \
  product(ccstr, RedletSIMD, "auto", EXPERIMENTAL,                           \
          "Vectorization engine: auto, avx10_2, avx512, sve2, rvv, scalar.") \
                                                                             \
  product(bool, RedletHeteroArenas, true, EXPERIMENTAL,                      \
          "Enable P-Core and E-Core heterogeneous task arenas via oneTBB.")  \
                                                                             \
  product(ccstr, RedletGPUOffload, "none", EXPERIMENTAL,                     \
          "GPU accelerator backend: none, levelzero, hip.")                  \
                                                                             \
  product(size_t, RedletMaxTLABSize, 4 * M, EXPERIMENTAL,                    \
          "Max TLAB size to use with RedletGC.")                             \
          range(1, max_intx)                                                 \
                                                                             \
  product(bool, RedletElasticTLAB, true, EXPERIMENTAL,                       \
          "Use elastic policy to manage TLAB sizes.")                        \
                                                                             \
  product(size_t, RedletMinHeapExpand, 128 * M, EXPERIMENTAL,                \
          "Min expansion step for heap.")                                    \
          range(1, max_intx)                                                 \
                                                                             \
  product(ccstr, RedletProfile, "NORMAX", EXPERIMENTAL,                      \
          "Redlet execution profile: NORMAX (standard high-throughput SIMD) "\
          "or SSIOLSC (safety-critical bounded deterministic real-time).")   \
                                                                             \
  product(bool, RedletSCMode, false, EXPERIMENTAL,                           \
          "Enable Safety-Critical (SSIOLSC) hard real-time deterministic "   \
          "mode with bounded microsecond latency and zero-growth envelope.") \
                                                                             \
  product(bool, RedletForbidDynamicGrowth, false, EXPERIMENTAL,              \
          "Forbid dynamic heap expansion at runtime for zero-jitter envelope.") \
                                                                             \
  product(ccstr, RedletSCGCType, "Default", EXPERIMENTAL,                    \
          "Safety-Critical GC type: Default (bureaucratic STW) or "          \
          "PrimitiveRW (unmanaged direct memory access).")

// end of GC_REDLET_FLAGS

#endif // SHARE_GC_REDLET_REDLET_GLOBALS_HPP
