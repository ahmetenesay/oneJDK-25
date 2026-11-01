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

#ifndef SHARE_GC_REDJET_REDJET_GLOBALS_HPP
#define SHARE_GC_REDJET_REDJET_GLOBALS_HPP

#include "runtime/globals_shared.hpp"

//
// Defines all global flags used by RedjetGC.
//

#define GC_REDJET_FLAGS(develop,                                             \
                        develop_pd,                                          \
                        product,                                             \
                        product_pd,                                          \
                        range,                                               \
                        constraint)                                          \
                                                                             \
  product(ccstr, RedjetOptions, "", EXPERIMENTAL,                            \
          "Direct Redjet flag sequence or intent keywords "                  \
          "(e.g. 'turbo,trim,strict_vfs,vector').")                          \
                                                                             \
  product(bool, RedjetVFSStrict, true, EXPERIMENTAL,                         \
          "Enforce strict VFS branch validation and fail-fast protection "   \
          "against missing VFS branches.")                                   \
                                                                             \
  product(bool, RedjetVFSPanicDialog, false, EXPERIMENTAL,                   \
          "Enable modal alert dialog popup on fatal VFS panics.")            \
                                                                             \
  product(bool, RedjetRMRP, true, EXPERIMENTAL,                              \
          "Enable Reduce Memory Resident Set Reclamation (RMRP).")           \
                                                                             \
  product(ccstr, RedjetRMRPMethod, "Plump", EXPERIMENTAL,                    \
          "RMRP memory reclamation method: Plump or Sequential.")            \
                                                                             \
  product(double, RedjetFallineCompactRatio, 0.30, EXPERIMENTAL,             \
          "Falline compaction threshold ratio (0.05 to 0.95).")              \
          range(0.05, 0.95)                                                  \
                                                                             \
  product(ccstr, RedjetSIMD, "auto", EXPERIMENTAL,                           \
          "Vectorization engine: auto, avx10_2, avx512, sve2, rvv, scalar.") \
                                                                             \
  product(bool, RedjetHeteroArenas, true, EXPERIMENTAL,                      \
          "Enable P-Core and E-Core heterogeneous task arenas via oneTBB.")  \
                                                                             \
  product(ccstr, RedjetGPUOffload, "none", EXPERIMENTAL,                     \
          "GPU accelerator backend: none, levelzero, hip.")                  \
                                                                             \
  product(size_t, RedjetMaxTLABSize, 4 * M, EXPERIMENTAL,                    \
          "Max TLAB size to use with RedjetGC.")                             \
          range(1, max_intx)                                                 \
                                                                             \
  product(bool, RedjetElasticTLAB, true, EXPERIMENTAL,                       \
          "Use elastic policy to manage TLAB sizes.")                        \
                                                                             \
  product(size_t, RedjetMinHeapExpand, 128 * M, EXPERIMENTAL,                \
          "Min expansion step for heap.")                                    \
          range(1, max_intx)                                                 \
                                                                             \
  product(ccstr, RedjetProfile, "NORMAX", EXPERIMENTAL,                      \
          "Redjet execution profile: NORMAX (standard high-throughput SIMD) "\
          "or SSIOLSC (safety-critical bounded deterministic real-time).")   \
                                                                             \
  product(bool, RedjetSCMode, false, EXPERIMENTAL,                           \
          "Enable Safety-Critical (SSIOLSC) hard real-time deterministic "   \
          "mode with bounded microsecond latency and zero-growth envelope.") \
                                                                             \
  product(bool, RedjetForbidDynamicGrowth, false, EXPERIMENTAL,              \
          "Forbid dynamic heap expansion at runtime for zero-jitter envelope.") \
                                                                             \
  product(ccstr, RedjetSCGCType, "Default", EXPERIMENTAL,                    \
          "Safety-Critical GC type: Default (bureaucratic STW) or "          \
          "PrimitiveRW (unmanaged direct memory access).")                   \
                                                                             \
  product(ccstr, RedjetGlobalVolatility, "DRAM", EXPERIMENTAL,               \
          "Global memory volatility regime: DRAM (standard write-back/stream) "\
          "or SRAM (L1/L2/3D-VCache resident scratchpad discipline).")       \
                                                                             \
  product(ccstr, RedjetPuzzlerMorph, "Shadow", EXPERIMENTAL,                 \
          "Puzzler Pitstop ISA morphing strategy: Shadow (lock-free RCU "    \
          "shadow swap) or Excision (exclusive Pitstop bay excision).")      \
                                                                             \
  product(double, RedjetLandfillCriticalRatio, 0.85, EXPERIMENTAL,           \
          "Landfill & Controlpoint high-pressure threshold ratio (0.10 to 0.99).") \
          range(0.10, 0.99)                                                  \
                                                                             \
  product(double, RedjetHeatDecayFactor, 0.97, EXPERIMENTAL,                 \
          "Per-sweep exponential decay factor for object heat scores.")      \
          range(0.01, 1.00)                                                  \
                                                                             \
  product(double, RedjetHeatGainWeight, 1.00, EXPERIMENTAL,                  \
          "Multiplier applied to Hookpoint object heat gain increments.")    \
          range(0.01, 100.0)                                                 \
                                                                             \
  product(uintx, RedjetInlineAsmThreshold, 5, EXPERIMENTAL,                  \
          "Minimum priority threshold (0..10) enabling inline ASM kernels.") \
          range(0, 10)                                                       \
                                                                             \
  product(bool, RedjetEntangleExecution, true, EXPERIMENTAL,                 \
          "Enable HSVM Batch Register and Entangle Execution pipelines.")    \
                                                                             \
  product(bool, RedjetEnable2Points, true, EXPERIMENTAL,                    \
          "Enable interdependent 2-Point Engine (Parkpoint & Dockpoint).")   \
                                                                             \
  product(bool, RedjetEnable4Points, true, EXPERIMENTAL,                     \
          "Compatibility alias for RedjetEnable2Points.")                    \
                                                                             \
  product(uintx, RedjetParkpointSpinCount, 256, EXPERIMENTAL,                \
          "SMT spin-backoff iteration count before atomic wait park.")       \
          range(1, 65536)                                                    \
                                                                             \
  product(uintx, RedjetPausepointSpinCount, 256, EXPERIMENTAL,               \
          "Compatibility alias for RedjetParkpointSpinCount.")               \
          range(1, 65536)                                                    \
                                                                             \
  product(bool, RedjetDockpointPacing, true, EXPERIMENTAL,                   \
          "Enable Dockpoint mutator allocation pacing at high heap pressure.") \
                                                                             \
  product(bool, RedjetControlpointPacing, true, EXPERIMENTAL,                \
          "Compatibility alias for RedjetDockpointPacing.")

// end of GC_REDJET_FLAGS

#endif // SHARE_GC_REDJET_REDJET_GLOBALS_HPP
