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

#ifndef SHARE_OPTO_MIRC_MIRC_LVL1_HPP
#define SHARE_OPTO_MIRC_MIRC_LVL1_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "opto/mirc/mirc_Simulator.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

class VAC_Table_Snapshot;
class Compile;

// Canonical 17-Slot SIMD Ladder Slot Definition
#ifndef HS_SIMD_LADDER_SLOT_T_DEFINED
#define HS_SIMD_LADDER_SLOT_T_DEFINED
struct alignas(64) hs_simd_ladder_slot_t {
  char     name[32];      // ISA name, e.g. "AVX512_Cached", "AVX2_Cached", "Scalar_Safe"
  uint32_t ladder_level;  // 0 = Peak/Matrix, 15 = Baseline Safe, 16 = Puzzler Wildcard
  uint8_t  is_active;     // 1 if currently active
  uint8_t  _pad[3];
};
#endif

// Synthesized MIRC Level 1 Table
struct alignas(64) MIRC_Lvl1_Table {
  // 17-Slot Prebaked SIMD Ladder: 16 Primary Candidate Rungs + Slot 16 Puzzler Overwrite Wildcard
  hs_simd_ladder_slot_t simd_ladder[17];
  uint16_t              candidate_count;
  uint16_t              active_idx;

  // 4-Step Simulation Convergence Results
  int                    best_unroll;
  double                 best_q_eff;
  mirc_simulation_step_t steps[3]; // [0] = 8x, [1] = 4x, [2] = 2x

  // Kernel Method Metadata
  uint32_t method_id;
  char     method_name[128];
  bool     is_strictfp;
  float    target_epsilon;
  uint64_t compile_tsc;
  uint64_t simulation_cycles;

  // Elastic Unrolled Loop Template Flags
  bool has_amx;
  bool has_avx512;
  bool has_avx2;
  bool has_neon;
  bool has_sve2;
  bool has_rvv;
  bool use_streaming_stores;
  bool no_alias;
  int  loop_stride_bytes;

  MIRC_Lvl1_Table();
};

#ifndef MIRC_HOTSWAP_DELTA_T_DEFINED
#define MIRC_HOTSWAP_DELTA_T_DEFINED
struct mirc_hotswap_delta_t {
  uint32_t method_id{ 0 };
  uint16_t old_simd_idx{ 0 };
  uint16_t new_simd_idx{ 0 };
  uint64_t baseline_cycles{ 0 };
  uint64_t winning_cycles{ 0 };
  uint64_t cycles_gained{ 0 };
  double   speedup{ 1.0 };
  char     winner_isa_name[32]{ 0 };
  void*    patch_target_pc{ nullptr };
  void*    new_code_entry{ nullptr };
  size_t   patch_size{ 0 };
  bool     slot16_overwrite{ false }; // Dynamic candidate discovered by Puzzler written directly to Slot 16
  bool     applied{ false };
  uint8_t  _pad[5]{ 0 };
};
#endif

// ============================================================================
// MIRC_Lvl1: Runtime Codegen & TableGen Synthesis Engine
// ============================================================================

class MIRC_Lvl1 {
public:
  // Ingests VAC_Table_Snapshot, runs the 4-step convergence simulation across
  // TableGen hardware models (*.inc), and synthesizes the 17-slot SIMD ladder + MIRC_Lvl1_Table
  MSIMD_API static MIRC_Lvl1_Table* synthesize(const VAC_Table_Snapshot* vac_tab);

  // Ingests C2 Compile context, extracts method/loop attributes, runs the 4-step convergence
  // simulation, and synthesizes the 17-slot SIMD ladder + MIRC_Lvl1_Table for C2
  MSIMD_API static MIRC_Lvl1_Table* synthesize_c2(Compile* C);
};

#endif // SHARE_OPTO_MIRC_MIRC_LVL1_HPP
