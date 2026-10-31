/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
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
          range(1, max_intx)

// end of GC_REDLET_FLAGS

#endif // SHARE_GC_REDLET_REDLET_GLOBALS_HPP
