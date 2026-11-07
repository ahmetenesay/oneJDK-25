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

#ifndef SHARE_VAC_VAC_SSAFLATTENER_HPP
#define SHARE_VAC_VAC_SSAFLATTENER_HPP

#include <cstddef>
#include <cstdint>
#include "vac/vac_TableSnapshot.hpp"

// ============================================================================
// VAC_SSAFlattener
//
// Pre-MIRC Static Single Assignment (SSA) Flattening, Affine Alias Proof,
// TailTraces Liveness Dead-End Clasp Injector, and Compiler-Assisted
// `System.gc()` Lookahead Pre-Warmer (`-XX:+RedjetGCInvokesExplicitCleanHints`).
//
// Universal GC Compatible:
//   - Works cleanly across G1, ZGC, Shenandoah, Parallel, Serial (pure SSA
//     dead-code elimination, affine `NoAlias`, and scalar replacement).
//   - Under `-XX:+UseRedjetGC`, additionally synthesizes:
//       1. `TailTraces` dead-reference clasp points at exact last-use SSA sites
//          with `RedjetBPUDirector` BTB fallthrough polarity hints.
//       2. Static `System.gc()` lookahead `preflight_gc_hint` markers placed
//          `LOOKAHEAD_WINDOW` instructions ahead of explicit clean callsites.
// ============================================================================

enum VAC_SyntheticMarkerKind : uint8_t {
  VAC_MARKER_NONE                = 0,
  VAC_MARKER_TAILTRACE_DEAD_END  = 1, // Emit TailTraces clasp signal for dead non-escaping oop
  VAC_MARKER_EXPLICIT_GC_PREWARM = 2, // Pre-warm RedjetExplicitGC::preflight_gc_hint ahead of System.gc()
  VAC_MARKER_PRECISION_STRICT    = 3  // Enforce strict IEEE-754 non-reassociative FPU evaluation
};

struct VAC_AffineInterval {
  uint16_t base_ssa_id;
  int64_t  min_offset_bytes;
  int64_t  max_offset_bytes;
  int32_t  stride_bytes;
  bool     escapes_method;
};

struct VAC_SyntheticSite {
  uint32_t                inst_index;
  uint16_t                ssa_value_id;
  VAC_SyntheticMarkerKind kind;
  uint8_t                 bpu_likely_fallthrough; // 1 = keep hot non-escape path on fall-through
  uint32_t                target_bci;
};

struct VAC_SSAFlattenReport {
  static constexpr size_t MAX_SYNTHETIC_SITES = 256;

  uint32_t          method_id{ 0 };
  uint32_t          original_inst_count{ 0 };
  uint32_t          flattened_inst_count{ 0 };
  uint32_t          dead_phis_eliminated{ 0 };
  uint32_t          fma_candidates_fused{ 0 };
  bool              no_alias_proven{ false };
  bool              all_objects_non_escaping{ false };
  uint32_t          tailtrace_clasps_injected{ 0 };
  uint32_t          explicit_gc_lookaheads_injected{ 0 };
  uint32_t          synthetic_site_count{ 0 };
  VAC_SyntheticSite synthetic_sites[MAX_SYNTHETIC_SITES];
};

class VAC_SSAFlattener {
public:
  static constexpr uint32_t EXPLICIT_GC_LOOKAHEAD_INSTS = 16;
  static constexpr uint16_t VAC_OP_EXPLICIT_GC_CALL     = 0x0FE0u;
  static constexpr uint16_t VAC_OP_PHI_TRIVIAL          = 0x0FE1u;
  static constexpr uint16_t VAC_OP_ESCAPE_STORE         = 0x0FE2u;

  // Flattens SSA instructions in-place inside `snap`, proves affine NoAlias,
  // fuses MUL+ADD -> FMA (when !strict_ieee754), computes exact last-use
  // TailTraces dead-ends, and injects System.gc() preflight lookahead markers.
  static bool flatten_and_analyze(VAC_Table_Snapshot_Compact* snap,
                                  const VAC_AffineInterval* intervals,
                                  size_t interval_count,
                                  bool strict_ieee754,
                                  VAC_SSAFlattenReport* out_report);

  // Executes runtime preflight dispatch for any pre-warm markers in a compiled unit
  static void dispatch_preflight_markers(const VAC_SSAFlattenReport& report);
};

#endif // SHARE_VAC_VAC_SSAFLATTENER_HPP
