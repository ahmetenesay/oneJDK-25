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

#include "mirc/mirc_BREE.hpp"
#include "gc/shared/gc_globals.hpp"
#include "utilities/macros.hpp"

#if defined(AMD64) || defined(_M_X64) || defined(__x86_64__)
#include "is/isMacrowebHolder.hpp"
#elif defined(AARCH64) || defined(_M_ARM64) || defined(__aarch64__)
#include "is/isMacrowebHolder.hpp"
#elif defined(RISCV) || defined(__riscv)
#include "is/isMacrowebHolder.hpp"
#endif

#if INCLUDE_REDJETGC
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "gc/redjet/simd/redjetSIMDContentionGovernor.hpp"
#endif

#include <algorithm>
#include <cstring>

bool MIRC_BREE::build_weave_plan(const VAC_Table_Snapshot_Compact* vac_snap,
                                 const MIRC_Lvl1_Table* lvl1_tab,
                                 MIRC_BREEWeavePlan* out_plan) noexcept {
  if (out_plan == nullptr) return false;
  MIRC_BREEWeavePlan plan{};

  if (vac_snap != nullptr) {
    plan.method_id = vac_snap->method_id;
  } else if (lvl1_tab != nullptr) {
    plan.method_id = lvl1_tab->method_id;
  }

  // 1. Query Universal ISA Tree via isMacrowebHolder
  bool has_512  = false;
  bool has_256  = false;
  bool has_apx  = false;
#if defined(AMD64) || defined(_M_X64) || defined(__x86_64__)
  isMacrowebHolder::initialize();
  has_apx = isMacrowebHolder::IS_APX_F();
  has_512 = isMacrowebHolder::IS_AVX10_2() || isMacrowebHolder::IS_AVX512F();
  has_256 = isMacrowebHolder::IS_AVX10_1() || isMacrowebHolder::IS_AVX2();
  plan.gpr_bank_count           = has_apx ? 32u : 16u;
  plan.vec_bank_count           = has_512 ? 32u : 16u;
  plan.use_apx_ndd_3op          = has_apx;
  plan.emit_vzeroupper_epilogue = (has_512 || has_256);
#elif defined(AARCH64) || defined(_M_ARM64) || defined(__aarch64__)
  isMacrowebHolder::initialize();
  has_512 = isMacrowebHolder::IS_SVE2() && (isMacrowebHolder::get_sve_vl_bytes() >= 64);
  has_256 = isMacrowebHolder::IS_SVE()  && (isMacrowebHolder::get_sve_vl_bytes() >= 32);
  plan.gpr_bank_count           = 31u;
  plan.vec_bank_count           = 32u;
  plan.use_apx_ndd_3op          = true; // ARM64 natively has 3-operand non-destructive GPR ops
  plan.emit_vzeroupper_epilogue = false;
#elif defined(RISCV) || defined(__riscv)
  isMacrowebHolder::initialize();
  has_512 = isMacrowebHolder::IS_RVV_1_0() && (isMacrowebHolder::get_rvv_vlenb() >= 64);
  has_256 = isMacrowebHolder::IS_RVV_1_0() && (isMacrowebHolder::get_rvv_vlenb() >= 32);
  plan.gpr_bank_count           = 31u;
  plan.vec_bank_count           = 32u;
  plan.use_apx_ndd_3op          = true;
  plan.emit_vzeroupper_epilogue = false;
#endif

  if (lvl1_tab != nullptr && lvl1_tab->is_strictfp) {
    // Honor strict IEEE-754 shield: disable wide FMA reassociation if target_epsilon == 0
    if (lvl1_tab->target_epsilon == 0.0f) {
      has_512 = false;
    }
  }

  plan.vector_width_bits = has_512 ? 512u : (has_256 ? 256u : 128u);

  // 2. Select GC Safepoint / Barrier Protocol (Universal GC vs RedjetGC Zero-STW)
  bool redjet_zero_stw = false;
#if INCLUDE_REDJETGC
  if (UseRedjetGC && RedjetEntangleExecution && !RedjetSCMode && !RedjetGCSCMode) {
    redjet_zero_stw = true;
  }
#endif

  if (redjet_zero_stw) {
    plan.safepoint_mode         = MIRC_BREESafepointMode::REDJET_ZERO_STW_PARKPOINT;
    plan.outer_strip_mine_iters = 0; // No STW strip-mine needed; 0.5-cycle L1-D Parkpoint check
  } else {
    plan.safepoint_mode         = MIRC_BREESafepointMode::STRIP_MINED_STW_POLL;
    plan.outer_strip_mine_iters = (plan.vector_width_bits == 512) ? 1024u : 2048u;
  }

  // 3. Determine Stream Entanglement Count & Register Partitioning
  uint32_t desired_streams = 2;
  if (vac_snap != nullptr) {
    uint32_t load_store_pressure = vac_snap->req.virt_gpr_pressure;
    if (load_store_pressure >= 8 && plan.vec_bank_count >= 32) {
      desired_streams = 8;
    } else if (load_store_pressure >= 4) {
      desired_streams = 4;
    }
  } else if (plan.vec_bank_count >= 32) {
    desired_streams = 4;
  }

  desired_streams = std::min<uint32_t>(desired_streams,
      static_cast<uint32_t>(MIRC_BREEWeavePlan::MAX_ENTANGLED_STREAMS));
  plan.entangled_stream_count = desired_streams;
  plan.inner_pilf_unroll      = (lvl1_tab != nullptr && lvl1_tab->best_unroll > 0)
                                    ? static_cast<uint32_t>(lvl1_tab->best_unroll)
                                    : ((plan.vec_bank_count >= 32) ? 8u : 4u);

  const bool use_nt_stores = (lvl1_tab != nullptr && lvl1_tab->use_streaming_stores) ||
                             (vac_snap != nullptr && vac_snap->req.sram_footprint_bytes > (256u * 1024u));

  // Partition physical GPRs (preferring APX R16..R31 when available to keep RAX..R15 free!)
  const uint16_t gpr_base_start = (plan.gpr_bank_count >= 32) ? 16u : 8u;
  for (uint32_t s = 0; s < desired_streams; ++s) {
    MIRC_EntangledStream& st = plan.streams[s];
    st.stream_id          = static_cast<uint16_t>(s);
    st.base_gpr_reg       = static_cast<uint16_t>((gpr_base_start + s) % plan.gpr_bank_count);
    st.acc_vec_reg        = static_cast<uint16_t>((s * 2) % plan.vec_bank_count);
    st.aux_vec_reg        = static_cast<uint16_t>((s * 2 + 1) % plan.vec_bank_count);
    st.elem_stride_bytes  = plan.vector_width_bits / 8u;
    st.non_temporal_store = use_nt_stores;

    if (plan.safepoint_mode == MIRC_BREESafepointMode::STRIP_MINED_STW_POLL) {
      plan.gpr_spill_mask |= (1ULL << st.base_gpr_reg);
      plan.vec_spill_mask |= (1ULL << st.acc_vec_reg) | (1ULL << st.aux_vec_reg);
    }
  }

  *out_plan = plan;
  return true;
}

void MIRC_BREE::enter_entangled_region(const MIRC_BREEWeavePlan& plan) noexcept {
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetSIMDContentionGovernor::instance().notify_mutator_simd_enter(plan.vector_width_bits);
  }
#else
  (void)plan;
#endif
}

void MIRC_BREE::exit_entangled_region(const MIRC_BREEWeavePlan& plan) noexcept {
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    (void)plan;
    RedjetSIMDContentionGovernor::instance().notify_mutator_simd_exit();
  }
#else
  (void)plan;
#endif
}
