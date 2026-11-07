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

#ifndef SHARE_MIRC_DYNTOPOSELSIMU_HPP
#define SHARE_MIRC_DYNTOPOSELSIMU_HPP

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <vector>

#include "mirc/ad.hpp"

// TableGen Microarchitecture Hardware Model Profile
struct mirc_tablegen_x86_model_t {
  const char* arch_name;
  const char* vendor;
  int         release_year;
  int         vector_bits;
  int         vector_registers;
  int         issue_width;
  double      fma_f32_tp;
  double      fma_f64_tp;
  double      fma_latency;
  double      load_tp;
  double      stream_tp;
  double      max_port_pressure;
  bool        has_fma;
  bool        has_avx2;
  bool        has_avx512;
  bool        has_avx10_2;
  bool        has_vnni;
  bool        has_amx;
  bool        is_qualified;
};

// TableGen Microarchitecture Hardware Model Profile for ARM (AArch64)
struct mirc_tablegen_arm_model_t {
  const char* arch_name;
  const char* vendor;
  int         release_year;
  int         vector_bits;
  int         vector_registers;
  int         issue_width;
  double      fma_f32_tp;
  double      fma_f64_tp;
  double      fma_latency;
  double      load_tp;
  double      stream_tp;
  double      max_port_pressure;
  bool        has_fma;
  bool        has_neon;
  bool        has_sve;
  bool        has_sve2;
  bool        is_qualified;
};

// TableGen Microarchitecture Hardware Model Profile for RISC-V (RVV)
struct mirc_tablegen_riscv_model_t {
  const char* arch_name;
  const char* vendor;
  int         release_year;
  int         vector_bits;
  int         vector_registers;
  int         issue_width;
  double      fma_f32_tp;
  double      fma_f64_tp;
  double      fma_latency;
  double      load_tp;
  double      stream_tp;
  double      max_port_pressure;
  bool        has_fma;
  bool        has_rvv;
  bool        is_qualified;
};

extern const mirc_tablegen_x86_model_t   g_mirc_x86_hardware_models[];
extern const mirc_tablegen_arm_model_t   g_mirc_arm_hardware_models[];
extern const mirc_tablegen_riscv_model_t g_mirc_riscv_hardware_models[];

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

class VAC_Table_Snapshot;

// ============================================================================
// 1. codegen.py Algorithmic Ground-Truth Models & Data Types
// ============================================================================

struct mirc_rule_complexity_t {
  int fma_ops;
  int transcendental_ops;
  int div_sqrt_ops;
  int blend_ops;
  int load_count;
  int store_count;
  double arithmetic_intensity;
};

struct mirc_hardware_sla_t {
  double threshold;          // AutoQualificationThreshold >= 1.15
  bool   require_fma;        // RequireHardwareFMA == true
  double max_port_pressure;  // MaxAllowablePortPressure <= 0.85
};

struct mirc_simulation_step_t {
  int    unroll_factor;
  double s1_baseline_ratio;    // Step 1: Baseline Hardware Execution Ratio
  double s2_port_pressure;     // Step 2: Unrolled Loop & Port Contention Simulation
  double s2_port_penalty;
  double s2_port_score;
  int    s3_regs_needed;       // Step 3: Vector Register Liveliness & Stack Spill Risk
  double s3_spill_ratio;
  double s3_reg_score;
  double s4_streaming_bonus;   // Step 4: Memory Subsystem & Non-Temporal Streaming Convergence
  double s4_converged_q_eff;
};

struct mirc_qualification_result_t {
  const char* arch_name;
  bool        is_qualified;
  double      q_eff;
  int         best_unroll;
  double      best_port_pressure;
  int         vector_bits;
  int         tier;
  char        disqualify_reason[128];
  mirc_simulation_step_t trajectory[3]; // [0]=8x, [1]=4x, [2]=2x
};

// ============================================================================
// 2. MIRC Hardware Profile Resolved from Permanent TableGen *.inc Models
// ============================================================================

struct mirc_resolved_hardware_t {
  const char* arch_name;
  const char* vendor;
  int         vector_bits;
  int         vector_registers;
  int         issue_width;
  double      fma_tp;
  double      fma_lat;
  double      load_tp;
  double      stream_tp;
  bool        has_fma;
  bool        has_avx2;
  bool        has_avx512;
  bool        has_avx10_2;
  bool        has_vnni;
  bool        has_amx;
  bool        has_neon;
  bool        has_sve;
  bool        has_sve2;
  bool        has_rvv;
  bool        is_qualified;
};

// ============================================================================
// 3. Branch Prediction vs. Brute-Force Arbitration Types
// ============================================================================

enum BranchStrategy {
  EMIT_CONDITIONAL_JUMP,
  EMIT_BRANCHLESS_CMOV,
  EMIT_VECTOR_MASK_BLEND
};

struct VAC_BPU_Requirements {
  uint32_t branch_entropy;
  uint32_t critical_path_depth;
  uint32_t predication_suitability;
  bool     has_vector_context;
};

struct Hardware_Model {
  uint32_t btb_entries;
  uint32_t available_alu_ports;
  uint32_t simd_width_bits;
  double   fma_latency;
};

// ============================================================================
// 4. DyntoposelSimu: Deep C++ Port of codegen.py Hardware Convergence Engine
// ============================================================================

class DyntoposelSimu {
public:
  // Resolves host processor against TableGen permanent *.inc models
  RJ_API static mirc_resolved_hardware_t resolve_host_hardware();

  // Analyzes instruction complexity and operator breakdown from VAC snapshot
  RJ_API static mirc_rule_complexity_t analyze_complexity(const VAC_Table_Snapshot* vac_tab);

  // Runs the complete 4-step iterative convergence simulation across [8x, 4x, 2x] unrolls
  // following codegen.py evaluate_and_qualify_x86_architectures algorithm
  RJ_API static mirc_qualification_result_t simulate_convergence(
      const mirc_resolved_hardware_t& hw,
      const mirc_rule_complexity_t& comp,
      bool use_streaming_stores,
      const mirc_hardware_sla_t& sla);

  // Arbitrates branch prediction unit vs. branchless ALU execution
  RJ_API static BranchStrategy arbitrate(const VAC_BPU_Requirements& vac_bpu, const Hardware_Model& hw);
};

typedef DyntoposelSimu MIRC_Simulator;

#endif // SHARE_MIRC_DYNTOPOSELSIMU_HPP
