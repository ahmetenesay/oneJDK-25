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

#ifndef SHARE_MIRC_MIRC_BREE_HPP
#define SHARE_MIRC_MIRC_BREE_HPP

#include <cstddef>
#include <cstdint>
#include "mirc/mirc_Lvl1.hpp"
#include "vac/vac_TableSnapshot.hpp"

// ============================================================================
// MIRC_BREE (Batch Register & Entangle Execution Weaver)
//
// Compiler-First Multi-Stream Kernel Fusion & Register Entanglement Engine:
//   - Entangles independent scalar/vector streams across full architectural
//     register banks (x86-64: 32 ZMM + 32 APX EGPR R16..R31; AArch64: 32 SVE2
//     Z0..Z31 + 31 GPR; RISC-V: 32 RVV V0..V31 + LMUL groups) to saturate
//     dual-load + quad-ALU/FMA + store ports in a single fused loop body.
//
// Universal GC Synergy:
//   - Standard GCs (G1 / ZGC / Shenandoah / Parallel / Serial):
//     Generates Strip-Mined Outer-Loop Safepoint Polls (`SafepointMechanism`)
//     with exact Vector Spill Descriptors (`spill_mask_lo/hi`) so STW GC pauses
//     preserve upper ZMM/YMM/SVE lanes without corruption.
//   - RedjetGC (`-XX:+UseRedjetGC` + `-XX:+RedjetEntangleExecution`):
//     Generates Zero-STW PILF (`Pipelined Loop Iteration Feed`) entangled loops
//     with 0.5-cycle L1-D `Parkpoint` checks, zero safepoint spills, and
//     `RedjetSIMDContentionGovernor` mutator port reservation.
// ============================================================================

enum class MIRC_BREESafepointMode : uint8_t {
  STRIP_MINED_STW_POLL      = 0, // G1 / ZGC / Shenandoah outer-strip safepoint + vector spill map
  REDJET_ZERO_STW_PARKPOINT = 1  // RedjetGC 0-spill PILF back-edge + 0.5-cycle L1-D Parkpoint
};

struct MIRC_EntangledStream {
  uint16_t stream_id;
  uint16_t base_gpr_reg;       // Physical GPR index (e.g., 0..31 including x86 APX R16..R31)
  uint16_t acc_vec_reg;        // Physical Vector accumulator register (0..31)
  uint16_t aux_vec_reg;        // Secondary entangled load/mul vector register (0..31)
  uint32_t elem_stride_bytes;
  bool     non_temporal_store; // Emit VMOVNTPS/PD / STNP when stream exceeds LLC/SRAM quota
};

struct MIRC_BREEWeavePlan {
  static constexpr size_t MAX_ENTANGLED_STREAMS = 8;

  uint32_t               method_id{ 0 };
  MIRC_BREESafepointMode safepoint_mode{ MIRC_BREESafepointMode::STRIP_MINED_STW_POLL };
  uint32_t               vector_width_bits{ 256 }; // 128, 256, or 512
  uint32_t               gpr_bank_count{ 16 };     // 32 when x86 APX or AArch64/RISC-V
  uint32_t               vec_bank_count{ 16 };     // 32 when AVX-512/AVX10, SVE2/NEON, or RVV
  uint32_t               entangled_stream_count{ 0 };
  uint32_t               inner_pilf_unroll{ 4 };
  uint32_t               outer_strip_mine_iters{ 2048 };
  uint64_t               vec_spill_mask{ 0 };      // Bitmask of live vector regs across STW poll
  uint64_t               gpr_spill_mask{ 0 };      // Bitmask of live GPRs across STW poll
  bool                   emit_vzeroupper_epilogue{ false };
  bool                   use_apx_ndd_3op{ false };
  MIRC_EntangledStream   streams[MAX_ENTANGLED_STREAMS];
};

class MIRC_BREE {
public:
  // Synthesizes a complete BREE register-entanglement & GC-barrier weave plan
  // from a VAC compact snapshot and MIRC Level-1 hardware synthesis table.
  RJ_API static bool build_weave_plan(const VAC_Table_Snapshot_Compact* vac_snap,
                                      const MIRC_Lvl1_Table* lvl1_tab,
                                      MIRC_BREEWeavePlan* out_plan) noexcept;

  // Runtime execution scope wrapper: notifies RedjetSIMDContentionGovernor when
  // entering/exiting a BREE entangled vector region under UseRedjetGC (no-op on other GCs).
  RJ_API static void enter_entangled_region(const MIRC_BREEWeavePlan& plan) noexcept;
  RJ_API static void exit_entangled_region(const MIRC_BREEWeavePlan& plan) noexcept;
};

class MIRC_BREEScopeGuard {
private:
  const MIRC_BREEWeavePlan& _plan;
public:
  explicit MIRC_BREEScopeGuard(const MIRC_BREEWeavePlan& plan) noexcept : _plan(plan) {
    MIRC_BREE::enter_entangled_region(_plan);
  }
  ~MIRC_BREEScopeGuard() noexcept {
    MIRC_BREE::exit_entangled_region(_plan);
  }
  MIRC_BREEScopeGuard(const MIRC_BREEScopeGuard&) = delete;
  MIRC_BREEScopeGuard& operator=(const MIRC_BREEScopeGuard&) = delete;
};

#endif // SHARE_MIRC_MIRC_BREE_HPP
