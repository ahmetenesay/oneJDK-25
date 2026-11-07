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

#include "vac/vac_SSAFlattener.hpp"
#include "gc/shared/gc_globals.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_REDJETGC
#include "gc/redjet/api/redjetExplicitGC.hpp"
#include "gc/redjet/runtime/redjetBPUDirector.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#endif

#include <algorithm>
#include <cstring>

namespace {
static constexpr size_t MAX_TRACKED_SSA_VALUES = 2048;

static bool intervals_disjoint(const VAC_AffineInterval& a, const VAC_AffineInterval& b) noexcept {
  if (a.base_ssa_id == b.base_ssa_id) {
    return (a.max_offset_bytes <= b.min_offset_bytes) || (b.max_offset_bytes <= a.min_offset_bytes);
  }
  if (!a.escapes_method && !b.escapes_method) {
    // Distinct non-escaping allocation bases are guaranteed disjoint in memory
    return true;
  }
  return (a.max_offset_bytes <= b.min_offset_bytes) || (b.max_offset_bytes <= a.min_offset_bytes);
}
} // namespace

bool VAC_SSAFlattener::flatten_and_analyze(VAC_Table_Snapshot_Compact* snap,
                                           const VAC_AffineInterval* intervals,
                                           size_t interval_count,
                                           bool strict_ieee754,
                                           VAC_SSAFlattenReport* out_report) {
  if (snap == nullptr) return false;

  VAC_SSAFlattenReport local_rep{};
  local_rep.method_id           = snap->method_id;
  local_rep.original_inst_count = snap->instruction_count;

  // 1. Affine Interval NoAlias & Escape Proof
  bool alias_detected = false;
  bool any_escaped    = false;
  if (intervals != nullptr && interval_count > 0) {
    for (size_t i = 0; i < interval_count; ++i) {
      if (intervals[i].escapes_method) any_escaped = true;
      for (size_t j = i + 1; j < interval_count; ++j) {
        if (!intervals_disjoint(intervals[i], intervals[j])) {
          alias_detected = true;
          break;
        }
      }
      if (alias_detected) break;
    }
    local_rep.no_alias_proven          = !alias_detected;
    local_rep.all_objects_non_escaping = !any_escaped;
    snap->has_no_alias                 = local_rep.no_alias_proven ? 1 : 0;
  } else {
    local_rep.no_alias_proven          = (snap->has_no_alias != 0);
    local_rep.all_objects_non_escaping = false;
  }

  if (snap->instruction_count == 0 || snap->instructions == nullptr) {
    local_rep.flattened_inst_count = 0;
    if (out_report != nullptr) *out_report = local_rep;
    return true;
  }

  // 2. Trivial Phi Elimination & Optional IEEE-754 Safe MUL+ADD -> FMA Fusion
  uint16_t remap[MAX_TRACKED_SSA_VALUES];
  for (size_t i = 0; i < MAX_TRACKED_SSA_VALUES; ++i) {
    remap[i] = static_cast<uint16_t>(i);
  }

  uint32_t write_idx = 0;
  for (uint32_t i = 0; i < snap->instruction_count; ++i) {
    VAC_Instruction_Entry inst = snap->instructions[i];
    if (inst.src1 < MAX_TRACKED_SSA_VALUES) inst.src1 = remap[inst.src1];
    if (inst.src2 < MAX_TRACKED_SSA_VALUES) inst.src2 = remap[inst.src2];
    if (inst.src3 < MAX_TRACKED_SSA_VALUES) inst.src3 = remap[inst.src3];

    // Collapse trivial single-source or self-referential Phi nodes: phi(x, x) -> x
    if (inst.opcode == VAC_OP_PHI_TRIVIAL &&
        (inst.src2 == 0 || inst.src2 == inst.src1 || inst.src2 == inst.dst)) {
      if (inst.dst < MAX_TRACKED_SSA_VALUES && inst.src1 < MAX_TRACKED_SSA_VALUES) {
        remap[inst.dst] = remap[inst.src1];
      }
      local_rep.dead_phis_eliminated++;
      continue;
    }

    // Fuse adjacent floating-point MUL + ADD into FMA3 ONLY when strict IEEE-754 shield is NOT set
    if (!strict_ieee754 && write_idx > 0 &&
        (inst.datatype == VAC_TYPE_FLOAT32 || inst.datatype == VAC_TYPE_FLOAT64)) {
      VAC_Instruction_Entry& prev = snap->instructions[write_idx - 1];
      if (prev.opcode == VAC_OP_MUL && inst.opcode == VAC_OP_ADD &&
          (inst.src1 == prev.dst || inst.src2 == prev.dst)) {
        uint16_t addend = (inst.src1 == prev.dst) ? inst.src2 : inst.src1;
        prev.opcode           = VAC_OP_FMA;
        prev.src3             = addend;
        prev.dst              = inst.dst;
        prev.fma_fusion_ready = 1;
        local_rep.fma_candidates_fused++;
        continue;
      }
    } else if (strict_ieee754) {
      inst.fma_fusion_ready = 0;
    }

    snap->instructions[write_idx++] = inst;
  }

  snap->instruction_count        = write_idx;
  local_rep.flattened_inst_count = write_idx;

  // 3. Exact Liveness Analysis (Last-Use Index + Escape Tracking) & System.gc() Lookahead
  int32_t last_use[MAX_TRACKED_SSA_VALUES];
  bool    is_object_val[MAX_TRACKED_SSA_VALUES];
  bool    escaped_val[MAX_TRACKED_SSA_VALUES];
  std::fill_n(last_use, MAX_TRACKED_SSA_VALUES, -1);
  std::fill_n(is_object_val, MAX_TRACKED_SSA_VALUES, false);
  std::fill_n(escaped_val, MAX_TRACKED_SSA_VALUES, false);

  // Mark interval-proven escapes
  if (intervals != nullptr) {
    for (size_t k = 0; k < interval_count; ++k) {
      if (intervals[k].base_ssa_id < MAX_TRACKED_SSA_VALUES && intervals[k].escapes_method) {
        escaped_val[intervals[k].base_ssa_id] = true;
      }
    }
  }

  for (uint32_t i = 0; i < snap->instruction_count; ++i) {
    const VAC_Instruction_Entry& inst = snap->instructions[i];
    if (inst.datatype == VAC_TYPE_OBJECT && inst.dst < MAX_TRACKED_SSA_VALUES && inst.dst != 0) {
      is_object_val[inst.dst] = true;
    }
    if (inst.src1 > 0 && inst.src1 < MAX_TRACKED_SSA_VALUES) last_use[inst.src1] = static_cast<int32_t>(i);
    if (inst.src2 > 0 && inst.src2 < MAX_TRACKED_SSA_VALUES) last_use[inst.src2] = static_cast<int32_t>(i);
    if (inst.src3 > 0 && inst.src3 < MAX_TRACKED_SSA_VALUES) last_use[inst.src3] = static_cast<int32_t>(i);

    if (inst.opcode == VAC_OP_ESCAPE_STORE && inst.src1 < MAX_TRACKED_SSA_VALUES) {
      escaped_val[inst.src1] = true;
    }

    // Compiler-Assisted System.gc() Static Lookahead (-XX:+RedjetGCInvokesExplicitCleanHints)
    if (inst.opcode == VAC_OP_EXPLICIT_GC_CALL) {
      bool emit_lookahead = false;
#if INCLUDE_REDJETGC
      emit_lookahead = (UseRedjetGC && RedjetGCInvokesExplicitCleanHints);
#endif
      if (emit_lookahead && local_rep.synthetic_site_count < VAC_SSAFlattenReport::MAX_SYNTHETIC_SITES) {
        uint32_t prewarm_idx = (i > EXPLICIT_GC_LOOKAHEAD_INSTS) ? (i - EXPLICIT_GC_LOOKAHEAD_INSTS) : 0u;
        VAC_SyntheticSite& site     = local_rep.synthetic_sites[local_rep.synthetic_site_count++];
        site.inst_index             = prewarm_idx;
        site.ssa_value_id           = 0;
        site.kind                   = VAC_MARKER_EXPLICIT_GC_PREWARM;
        site.bpu_likely_fallthrough = 1;
        site.target_bci             = i;
        local_rep.explicit_gc_lookaheads_injected++;
      }
    }
  }

  // 4. TailTraces Dead-End Clasp Synthesis (-XX:+RedjetGCUseDeadReferenceSignals)
#if INCLUDE_REDJETGC
  if (UseRedjetGC && RedjetGCUseDeadReferenceSignals) {
    for (uint16_t ssa_id = 1; ssa_id < MAX_TRACKED_SSA_VALUES; ++ssa_id) {
      if (is_object_val[ssa_id] && !escaped_val[ssa_id] && last_use[ssa_id] >= 0) {
        if (local_rep.synthetic_site_count >= VAC_SSAFlattenReport::MAX_SYNTHETIC_SITES) break;

        uint64_t site_hash = (static_cast<uint64_t>(snap->method_id) << 32) ^
                             static_cast<uint64_t>(last_use[ssa_id]);
        RedjetBPULivenessDecision bpu =
            RedjetBPUDirector::instance().evaluate_tailtrace_clasp(site_hash, 0, 64);

        VAC_SyntheticSite& site     = local_rep.synthetic_sites[local_rep.synthetic_site_count++];
        site.inst_index             = static_cast<uint32_t>(last_use[ssa_id]);
        site.ssa_value_id           = ssa_id;
        site.kind                   = VAC_MARKER_TAILTRACE_DEAD_END;
        site.bpu_likely_fallthrough = bpu.emit_likely_fallthrough ? 1 : 0;
        site.target_bci             = static_cast<uint32_t>(last_use[ssa_id]);
        local_rep.tailtrace_clasps_injected++;
      }
    }
  }
#endif

  if (out_report != nullptr) {
    *out_report = local_rep;
  }
  return true;
}

void VAC_SSAFlattener::dispatch_preflight_markers(const VAC_SSAFlattenReport& report) {
#if INCLUDE_REDJETGC
  if (!UseRedjetGC || !RedjetGCInvokesExplicitCleanHints) return;
  for (uint32_t i = 0; i < report.synthetic_site_count; ++i) {
    const VAC_SyntheticSite& site = report.synthetic_sites[i];
    if (site.kind == VAC_MARKER_EXPLICIT_GC_PREWARM) {
      RedjetExplicitGC::preflight_gc_hint(report.method_id, site.target_bci);
    }
  }
#else
  (void)report;
#endif
}
