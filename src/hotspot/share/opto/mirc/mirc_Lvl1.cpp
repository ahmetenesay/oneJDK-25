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

#include "opto/mirc/mirc_Lvl1.hpp"
#include "c1/c1_SortOpt.hpp"
#include "opto/compile.hpp"
#include "ci/ciMethod.hpp"
#include <cstdio>
#include <cmath>
#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

static inline uint64_t mirc_rdtsc() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 1000ULL;
#endif
}

// ============================================================================
// 1. MIRC_Lvl1_Table Constructor
// ============================================================================

MIRC_Lvl1_Table::MIRC_Lvl1_Table()
  : candidate_count(17),
    active_idx(15),
    best_unroll(8),
    best_q_eff(1.0),
    method_id(0),
    is_strictfp(true),
    target_epsilon(1.192092896e-07f), // FLT_EPSILON for strictfp fallback
    compile_tsc(0),
    simulation_cycles(0),
    has_amx(false),
    has_avx512(false),
    has_avx2(false),
    has_neon(false),
    has_sve2(false),
    has_rvv(false),
    use_streaming_stores(false),
    no_alias(false),
    loop_stride_bytes(64) {
  memset(simd_ladder, 0, sizeof(simd_ladder));
  memset(steps, 0, sizeof(steps));
  memset(method_name, 0, sizeof(method_name));
}

// ============================================================================
// 2. MIRC_Lvl1 Synthesis Engine
// ============================================================================

MIRC_Lvl1_Table* MIRC_Lvl1::synthesize(const VAC_Table_Snapshot* vac_tab) {
  if (vac_tab == nullptr) return nullptr;

  const uint64_t sim_start_tsc = mirc_rdtsc();
  auto* table = new MIRC_Lvl1_Table();

  // Ingest Method & Kernel Attributes from VAC Snapshot
  table->method_id   = vac_tab->function_id();
  table->is_strictfp = vac_tab->is_strictfp();
  table->no_alias    = vac_tab->no_alias_proven();
  strncpy(table->method_name, vac_tab->method_name(), sizeof(table->method_name) - 1);

  // Default precision: if not specified, strictly enforce 100% IEEE 754 standard
  table->target_epsilon = table->is_strictfp ? 1.192092896e-07f : 1e-4f;

  // Resolve host hardware profile using permanent TableGen *.inc models
  const mirc_resolved_hardware_t hw = MIRC_Simulator::resolve_host_hardware();
  table->has_amx    = hw.has_amx;
  table->has_avx512 = hw.has_avx512;
  table->has_avx2   = hw.has_avx2;
  table->has_neon   = hw.has_neon;
  table->has_sve2   = hw.has_sve2;
  table->has_rvv    = hw.has_rvv;

  // Analyze complexity from VAC snapshot
  const mirc_rule_complexity_t comp = MIRC_Simulator::analyze_complexity(vac_tab);

  // Streaming store decision: >= 256 KB memory footprint threshold from codegen.py
  table->use_streaming_stores = (vac_tab->total_memory_bytes() >= 262144);

  // Run deep 4-step convergence simulation across unroll trials [8x, 4x, 2x]
  mirc_hardware_sla_t sla{ 1.15, true, 0.85 };
  mirc_qualification_result_t qual = MIRC_Simulator::simulate_convergence(
      hw, comp, table->use_streaming_stores, sla);

  table->best_q_eff  = qual.q_eff;
  table->best_unroll = qual.best_unroll;
  table->loop_stride_bytes = (hw.vector_bits / 8) * qual.best_unroll;

  for (int t = 0; t < 3; ++t) {
    table->steps[t] = qual.trajectory[t];
  }

  // --------------------------------------------------------------------------
  // Synthesize Canonical 17-Slot SIMD Ladder (simd_table[17])
  // 16 Primary Candidate Rungs + Slot 16 Puzzler Overwrite Wildcard
  // --------------------------------------------------------------------------
  struct static_rung_t {
    const char* name;
    uint32_t    level;
  };

  static const static_rung_t kPrimaryRungs[16] = {
    { "AMX_Tile",               0  }, // Slot 0:  Tier 0 (Matrix/Tile)
    { "AVX10_2_512",            1  }, // Slot 1:  Tier 1 (Wide Vector 512-bit)
    { "AVX512_NT",              2  }, // Slot 2:  Tier 1 (Non-temporal streaming)
    { "AVX512_Cached",          3  }, // Slot 3:  Tier 1 (L1/L2 resident)
    { "AVX512_VL",              4  }, // Slot 4:  Tier 1 (256/128-bit sub-vectors)
    { "AVX2_NT",                5  }, // Slot 5:  Tier 2 (Basic/Cached Vector 256-bit)
    { "AVX2_Cached",            6  }, // Slot 6:  Tier 2 (AVX2 + FMA3)
    { "AVX_VNNI",               7  }, // Slot 7:  Tier 2 (VNNI dot-product)
    { "SVE2_Cached",            8  }, // Slot 8:  Tier 1 ARM (SVE2 512b)
    { "SVE_Cached",             9  }, // Slot 9:  Tier 2 ARM (SVE 256b)
    { "NEON_Cached",            10 }, // Slot 10: Tier 3 ARM (NEON 128b)
    { "RVV_Cached",             11 }, // Slot 11: Tier 1/2 RISC-V Vector
    { "SSE42_Cached",           12 }, // Slot 12: Tier 3 (128-bit SSE4.2)
    { "Lane16_ALU_Emu",         13 }, // Slot 13: Tier 4 (16-Lane ALU Emulation)
    { "Coprocessor_CORDIC_TMU", 14 }, // Slot 14: Tier 5 (Special Acceleration)
    { "Scalar_Safe",            15 }  // Slot 15: Tier 6 (Strict IEEE 754 baseline failsafe)
  };

  for (int i = 0; i < 16; ++i) {
    strncpy(table->simd_ladder[i].name, kPrimaryRungs[i].name, sizeof(table->simd_ladder[i].name) - 1);
    table->simd_ladder[i].ladder_level = kPrimaryRungs[i].level;
    table->simd_ladder[i].is_active    = 0;
  }

  // Slot 16: Dynamic Puzzler Overwrite Wildcard (reserved for background discovery)
  strncpy(table->simd_ladder[16].name, "Puzzler_Wildcard", sizeof(table->simd_ladder[16].name) - 1);
  table->simd_ladder[16].ladder_level = 16;
  table->simd_ladder[16].is_active    = 0;

  table->candidate_count = 17;

  // Select host initial active SIMD rung based on detected host CPU capabilities and qualification
  uint16_t active_slot = 15; // default to Scalar_Safe
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  if (hw.has_amx) {
    active_slot = 0; // AMX_Tile
  } else if (hw.has_avx512 && qual.is_qualified) {
    active_slot = table->use_streaming_stores ? 2 : 3; // AVX512_NT or AVX512_Cached
  } else if (hw.has_avx2 && qual.is_qualified) {
    active_slot = table->use_streaming_stores ? 5 : 6; // AVX2_NT or AVX2_Cached
  } else {
    active_slot = 12; // SSE42_Cached
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (hw.has_sve2) {
    active_slot = 8; // SVE2_Cached
  } else {
    active_slot = 10; // NEON_Cached
  }
#elif defined(__riscv) || defined(__riscv_vector)
  active_slot = 11; // RVV_Cached
#endif

  table->active_idx = active_slot;
  table->simd_ladder[active_slot].is_active = 1;

  const uint64_t sim_end_tsc = mirc_rdtsc();
  table->simulation_cycles = (sim_end_tsc >= sim_start_tsc) ? (sim_end_tsc - sim_start_tsc) : 100ULL;
  table->compile_tsc       = sim_end_tsc;

  return table;
}

MIRC_Lvl1_Table* MIRC_Lvl1::synthesize_c2(Compile* C) {
  if (C == nullptr || !C->has_method()) return nullptr;

  const uint64_t sim_start_tsc = mirc_rdtsc();
  auto* table = new MIRC_Lvl1_Table();

  ciMethod* method = C->method();
  table->method_id   = (method != nullptr) ? (uint32_t)(uintptr_t)method : C->compile_id();
  table->is_strictfp = (method != nullptr) ? method->is_strict() : true;
  table->no_alias    = true;
  if (method != nullptr) {
    strncpy(table->method_name, method->name()->as_utf8(), sizeof(table->method_name) - 1);
  } else {
    strncpy(table->method_name, "c2_compiled_kernel", sizeof(table->method_name) - 1);
  }

  table->target_epsilon = table->is_strictfp ? 1.192092896e-07f : 1e-4f;

  const mirc_resolved_hardware_t hw = MIRC_Simulator::resolve_host_hardware();
  table->has_amx    = hw.has_amx;
  table->has_avx512 = hw.has_avx512;
  table->has_avx2   = hw.has_avx2;
  table->has_neon   = hw.has_neon;
  table->has_sve2   = hw.has_sve2;
  table->has_rvv    = hw.has_rvv;

  mirc_rule_complexity_t comp{};
  // Fast Sea-of-Nodes topology extraction
  uint live_cnt = C->live_nodes();
  int loop_cnt  = std::max(1, C->inner_loops());
  comp.fma_ops  = std::max(1, (int)(live_cnt / 8));
  comp.load_count = std::max(1, (int)(live_cnt / 4));
  comp.store_count = std::max(1, (int)(live_cnt / 8));
  comp.transcendental_ops = 0;
  comp.div_sqrt_ops = 0;
  comp.blend_ops = std::max(0, (int)(live_cnt / 16));
  comp.arithmetic_intensity = (double)(comp.fma_ops * 2) / (double)std::max(1, comp.load_count + comp.store_count);

  table->use_streaming_stores = (C->max_node_limit() > 10000 || (live_cnt * 64) >= 262144);

  mirc_hardware_sla_t sla{ 1.15, true, 0.85 };
  mirc_qualification_result_t qual = MIRC_Simulator::simulate_convergence(
      hw, comp, table->use_streaming_stores, sla);

  table->best_q_eff  = qual.q_eff;
  table->best_unroll = qual.best_unroll;
  table->loop_stride_bytes = (hw.vector_bits / 8) * qual.best_unroll;

  for (int t = 0; t < 3; ++t) {
    table->steps[t] = qual.trajectory[t];
  }

  static const struct { const char* name; uint32_t level; } kPrimaryRungs[16] = {
    { "AMX_Tile",               0  },
    { "AVX10_2_512",            1  },
    { "AVX512_NT",              2  },
    { "AVX512_Cached",          3  },
    { "AVX512_VL",              4  },
    { "AVX2_NT",                5  },
    { "AVX2_Cached",            6  },
    { "AVX_VNNI",               7  },
    { "SVE2_Cached",            8  },
    { "SVE_Cached",             9  },
    { "NEON_Cached",            10 },
    { "RVV_Cached",             11 },
    { "SSE42_Cached",           12 },
    { "Lane16_ALU_Emu",         13 },
    { "Coprocessor_CORDIC_TMU", 14 },
    { "Scalar_Safe",            15 }
  };

  for (int i = 0; i < 16; ++i) {
    strncpy(table->simd_ladder[i].name, kPrimaryRungs[i].name, sizeof(table->simd_ladder[i].name) - 1);
    table->simd_ladder[i].ladder_level = kPrimaryRungs[i].level;
    table->simd_ladder[i].is_active    = 0;
  }

  strncpy(table->simd_ladder[16].name, "Puzzler_Wildcard", sizeof(table->simd_ladder[16].name) - 1);
  table->simd_ladder[16].ladder_level = 16;
  table->simd_ladder[16].is_active    = 0;
  table->candidate_count = 17;

  uint16_t active_slot = 15;
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  if (hw.has_amx) {
    active_slot = 0;
  } else if (hw.has_avx512 && qual.is_qualified) {
    active_slot = table->use_streaming_stores ? 2 : 3;
  } else if (hw.has_avx2 && qual.is_qualified) {
    active_slot = table->use_streaming_stores ? 5 : 6;
  } else {
    active_slot = 12;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (hw.has_sve2) {
    active_slot = 8;
  } else {
    active_slot = 10;
  }
#elif defined(__riscv) || defined(__riscv_vector)
  active_slot = 11;
#endif

  table->active_idx = active_slot;
  table->simd_ladder[active_slot].is_active = 1;

  const uint64_t sim_end_tsc = mirc_rdtsc();
  table->simulation_cycles = (sim_end_tsc >= sim_start_tsc) ? (sim_end_tsc - sim_start_tsc) : 100ULL;
  table->compile_tsc       = sim_end_tsc;

  return table;
}
