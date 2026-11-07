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

#include "mirc/DyntoposelSimu.hpp"
#include "vac/c1_SortOpt.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

// Host CPU capability inspection
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

// ============================================================================
// TableGen Synthesized Hardware Model Database (derived from TableGen rules & codegen.py SLA)
// ============================================================================

const mirc_tablegen_x86_model_t g_mirc_x86_hardware_models[] = {
  // ArchName, Vendor, Year, VBits, VRegs, IssueW, FMA32_TP, FMA64_TP, FMALat, LoadTP, StreamTP, PortPress, FMA, AVX2, AVX512, AVX10_2, VNNI, AMX, Qualified
  { "ZEN5",       "AMD",   2024, 512, 32, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true,  true,  true,  false, true,  false, true },
  { "ZEN4",       "AMD",   2022, 512, 32, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true,  true,  true,  false, true,  false, true },
  { "ZEN3",       "AMD",   2020, 256, 16, 6, 0.50, 0.50, 4.0, 0.33, 0.50, 0.42, true,  true,  false, false, false, false, true },
  { "ZEN2",       "AMD",   2019, 256, 16, 4, 0.50, 0.50, 5.0, 0.33, 0.50, 0.45, true,  true,  false, false, false, false, true },
  { "ZEN+",       "AMD",   2018, 128, 16, 4, 1.00, 1.00, 5.0, 0.50, 0.50, 0.50, true,  true,  false, false, false, false, true },
  { "ARL-P",      "Intel", 2024, 256, 16, 8, 0.50, 0.50, 4.0, 0.25, 0.50, 0.35, true,  true,  false, false, true,  true,  true },
  { "ARL-E",      "Intel", 2024, 256, 16, 4, 1.00, 1.00, 4.0, 0.50, 0.50, 0.55, true,  true,  false, false, true,  false, true },
  { "MTL",        "Intel", 2023, 256, 16, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true,  true,  false, false, true,  false, true },
  { "ADL",        "Intel", 2021, 256, 16, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true,  true,  false, false, true,  false, true },
  { "RKL",        "Intel", 2021, 512, 32, 5, 0.50, 0.50, 4.0, 0.33, 0.50, 0.45, true,  true,  true,  false, true,  false, true },
  { "TGL",        "Intel", 2020, 512, 32, 5, 0.50, 0.50, 4.0, 0.33, 0.50, 0.45, true,  true,  true,  false, true,  false, true },
  { "ICL",        "Intel", 2019, 512, 32, 5, 0.50, 0.50, 4.0, 0.50, 0.50, 0.50, true,  true,  true,  false, true,  false, true },
  { "SKX",        "Intel", 2017, 512, 32, 4, 0.50, 0.50, 4.0, 0.50, 0.50, 0.50, true,  true,  true,  false, false, false, true },
  { "CLX",        "Intel", 2019, 512, 32, 4, 0.50, 0.50, 4.0, 0.50, 0.50, 0.50, true,  true,  true,  false, true,  false, true },
  { "EMR",        "Intel", 2023, 512, 32, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true,  true,  true,  false, true,  true,  true },
  { "SKL",        "Intel", 2015, 256, 16, 4, 0.50, 0.50, 4.0, 0.50, 0.50, 0.50, true,  true,  false, false, false, false, true },
  { "TRM",        "AMD",   2024, 512, 32, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true,  true,  true,  false, true,  false, true },
  { "KX7000",     "Zhaoxin", 2024, 256, 16, 4, 1.00, 1.00, 5.0, 0.50, 0.50, 0.55, true, true, false, false, false, false, true },
  { nullptr,      nullptr, 0,    0,   0,  0, 0,       0,       0,   0,       0,       0,    false, false, false, false, false, false, false }
};

const mirc_tablegen_arm_model_t g_mirc_arm_hardware_models[] = {
  // ArchName, Vendor, Year, VBits, VRegs, IssueW, FMA32_TP, FMA64_TP, FMALat, LoadTP, StreamTP, PortPress, FMA, NEON, SVE, SVE2, Qualified
  { "Neoverse-V2", "ARM",      2023, 512, 32, 8, 0.50, 0.50, 4.0, 0.25, 0.50, 0.35, true, true, true, true,  true },
  { "Neoverse-V1", "ARM",      2021, 512, 32, 8, 0.50, 0.50, 4.0, 0.33, 0.50, 0.40, true, true, true, false, true },
  { "Neoverse-N2", "ARM",      2021, 256, 32, 4, 0.50, 0.50, 4.0, 0.33, 0.50, 0.42, true, true, true, true,  true },
  { "Neoverse-N1", "ARM",      2019, 128, 32, 4, 1.00, 1.00, 4.0, 0.50, 0.50, 0.50, true, true, false, false, true },
  { "Cortex-X4",   "ARM",      2023, 256, 32, 8, 0.50, 0.50, 4.0, 0.25, 0.50, 0.38, true, true, true, true,  true },
  { "Cortex-X3",   "ARM",      2022, 256, 32, 6, 0.50, 0.50, 4.0, 0.25, 0.50, 0.40, true, true, true, true,  true },
  { "Apple-M3",    "Apple",    2023, 128, 32, 8, 0.25, 0.25, 3.0, 0.25, 0.33, 0.30, true, true, false, false, true },
  { "Apple-M2",    "Apple",    2022, 128, 32, 8, 0.25, 0.25, 3.0, 0.25, 0.33, 0.32, true, true, false, false, true },
  { "Apple-M1",    "Apple",    2020, 128, 32, 8, 0.33, 0.33, 3.0, 0.25, 0.33, 0.35, true, true, false, false, true },
  { "Oryon",       "Qualcomm", 2024, 128, 32, 8, 0.25, 0.25, 3.5, 0.25, 0.40, 0.33, true, true, false, false, true },
  { "Falkor",      "Qualcomm", 2017, 128, 32, 4, 1.00, 1.00, 4.0, 0.50, 0.50, 0.55, true, true, false, false, true },
  { nullptr,       nullptr,    0,    0,   0,  0, 0,    0,    0,   0,    0,    0,    false, false, false, false, false }
};

const mirc_tablegen_riscv_model_t g_mirc_riscv_hardware_models[] = {
  // ArchName, Vendor, Year, VBits, VRegs, IssueW, FMA32_TP, FMA64_TP, FMALat, LoadTP, StreamTP, PortPress, FMA, RVV, Qualified
  { "XiangShan-KunMingHu", "CAS",       2024, 512, 32, 6, 0.50, 0.50, 4.0, 0.33, 0.50, 0.38, true, true, true },
  { "XiangShan-NanHu",     "CAS",       2022, 256, 32, 4, 0.50, 0.50, 4.0, 0.50, 0.50, 0.45, true, true, true },
  { "T-Head-C908",         "T-Head",    2023, 256, 32, 2, 1.00, 1.00, 4.0, 0.50, 0.50, 0.50, true, true, true },
  { "T-Head-C920",         "T-Head",    2022, 128, 32, 3, 1.00, 1.00, 4.0, 0.50, 0.50, 0.55, true, true, true },
  { "SpacemiT-X60",        "SpacemiT",  2024, 256, 32, 2, 1.00, 1.00, 4.0, 0.50, 0.50, 0.50, true, true, true },
  { nullptr,               nullptr,     0,    0,   0,  0, 0,    0,    0,   0,    0,    0,    false, false, false }
};

// ============================================================================
// 1. Host Hardware Resolution against Permanent TableGen Models (*.inc)
// ============================================================================

mirc_resolved_hardware_t DyntoposelSimu::resolve_host_hardware() {
  mirc_resolved_hardware_t hw{};
  hw.arch_name = "Generic";
  hw.vendor    = "Unknown";
  hw.vector_bits = 128;
  hw.vector_registers = 16;
  hw.issue_width = 4;
  hw.fma_tp   = 1.0;
  hw.fma_lat  = 4.0;
  hw.load_tp  = 0.50;
  hw.stream_tp = 0.50;
  hw.has_fma  = false;
  hw.has_avx2 = false;
  hw.has_avx512 = false;
  hw.has_avx10_2 = false;
  hw.has_vnni = false;
  hw.has_amx  = false;
  hw.has_neon = false;
  hw.has_sve  = false;
  hw.has_sve2 = false;
  hw.has_rvv  = false;
  hw.is_qualified = true;

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  int cpuInfo[4] = { 0, 0, 0, 0 };
#if defined(_MSC_VER)
  __cpuid(cpuInfo, 0);
  int nIds = cpuInfo[0];
  if (nIds >= 1) {
    __cpuid(cpuInfo, 1);
    bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
    bool avx = osxsave && ((cpuInfo[2] & (1 << 28)) != 0);
    hw.has_fma = avx && ((cpuInfo[2] & (1 << 12)) != 0);
  }
  if (nIds >= 7) {
    __cpuidex(cpuInfo, 7, 0);
    hw.has_avx2   = (cpuInfo[1] & (1 << 5)) != 0;
    hw.has_avx512 = (cpuInfo[1] & (1 << 16)) != 0; // AVX512F
    hw.has_vnni   = (cpuInfo[2] & (1 << 11)) != 0; // AVX512_VNNI
    hw.has_amx    = (cpuInfo[3] & (1 << 24)) != 0; // AMX-TILE
  }
#elif defined(__cpuid)
  __cpuid(0, cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);
  int nIds = cpuInfo[0];
  if (nIds >= 1) {
    __cpuid(1, cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);
    bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
    bool avx = osxsave && ((cpuInfo[2] & (1 << 28)) != 0);
    hw.has_fma = avx && ((cpuInfo[2] & (1 << 12)) != 0);
  }
  if (nIds >= 7) {
    __cpuid_count(7, 0, cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);
    hw.has_avx2   = (cpuInfo[1] & (1 << 5)) != 0;
    hw.has_avx512 = (cpuInfo[1] & (1 << 16)) != 0;
    hw.has_vnni   = (cpuInfo[2] & (1 << 11)) != 0;
    hw.has_amx    = (cpuInfo[3] & (1 << 24)) != 0;
  }
#else
  hw.has_avx2 = true;
  hw.has_fma  = true;
#endif

  // Match against TableGen X86 models (X86GenMachNode.inc)
  const char* target_model_name = "SKL";
  if (hw.has_amx) {
    target_model_name = "ARL-P";
  } else if (hw.has_avx512) {
    target_model_name = "ZEN4";
  } else if (hw.has_avx2) {
    target_model_name = "ZEN3";
  }

  for (size_t i = 0; g_mirc_x86_hardware_models[i].arch_name != nullptr; ++i) {
    const auto& m = g_mirc_x86_hardware_models[i];
    if (strcmp(m.arch_name, target_model_name) == 0) {
      hw.arch_name        = m.arch_name;
      hw.vendor           = m.vendor;
      hw.vector_bits      = m.vector_bits;
      hw.vector_registers = m.vector_registers;
      hw.issue_width      = m.issue_width;
      hw.fma_tp           = m.fma_f32_tp;
      hw.fma_lat          = m.fma_latency;
      hw.load_tp          = m.load_tp;
      hw.stream_tp        = m.stream_tp;
      hw.has_avx10_2      = m.has_avx10_2;
      hw.is_qualified     = m.is_qualified;
      break;
    }
  }

#elif defined(__aarch64__) || defined(_M_ARM64)
  const char* target_arm_model = "Neoverse-V2";
  for (size_t i = 0; g_mirc_arm_hardware_models[i].arch_name != nullptr; ++i) {
    const auto& m = g_mirc_arm_hardware_models[i];
    if (strcmp(m.arch_name, target_arm_model) == 0) {
      hw.arch_name        = m.arch_name;
      hw.vendor           = m.vendor;
      hw.vector_bits      = m.vector_bits;
      hw.vector_registers = m.vector_registers;
      hw.issue_width      = m.issue_width;
      hw.fma_tp           = m.fma_f32_tp;
      hw.fma_lat          = m.fma_latency;
      hw.load_tp          = m.load_tp;
      hw.stream_tp        = m.stream_tp;
      hw.has_neon         = m.has_neon;
      hw.has_fma          = m.has_fma;
      hw.has_sve          = m.has_sve;
      hw.has_sve2         = m.has_sve2;
      hw.is_qualified     = m.is_qualified;
      break;
    }
  }

#elif defined(__riscv) || defined(__riscv_vector)
  const char* target_riscv_model = "XiangShan-KunMingHu";
  for (size_t i = 0; g_mirc_riscv_hardware_models[i].arch_name != nullptr; ++i) {
    const auto& m = g_mirc_riscv_hardware_models[i];
    if (strcmp(m.arch_name, target_riscv_model) == 0) {
      hw.arch_name        = m.arch_name;
      hw.vendor           = m.vendor;
      hw.vector_bits      = m.vector_bits;
      hw.vector_registers = m.vector_registers;
      hw.issue_width      = m.issue_width;
      hw.fma_tp           = m.fma_f32_tp;
      hw.fma_lat          = m.fma_latency;
      hw.load_tp          = m.load_tp;
      hw.stream_tp        = m.stream_tp;
      hw.has_rvv          = m.has_rvv;
      hw.has_fma          = m.has_fma;
      hw.is_qualified     = m.is_qualified;
      break;
    }
  }
#endif

  return hw;
}

// ============================================================================
// 2. Rule Complexity Analysis (Instruction Mix from VAC_Table_Snapshot)
// ============================================================================

mirc_rule_complexity_t DyntoposelSimu::analyze_complexity(const VAC_Table_Snapshot* vac_tab) {
  mirc_rule_complexity_t c{};
  if (vac_tab == nullptr) {
    c.fma_ops = 1;
    c.arithmetic_intensity = 1.0;
    return c;
  }

  c.fma_ops             = std::max(1, vac_tab->fma_ops());
  c.transcendental_ops  = vac_tab->transcendental_ops();
  c.div_sqrt_ops        = vac_tab->div_sqrt_ops();
  c.blend_ops           = vac_tab->blend_ops();
  c.load_count          = vac_tab->load_count();
  c.store_count         = vac_tab->store_count();
  c.arithmetic_intensity = vac_tab->arithmetic_intensity();

  return c;
}

// ============================================================================
// 3. 4-Step Iterative Convergence Simulation (codegen.py lines 589-634)
// ============================================================================

mirc_qualification_result_t DyntoposelSimu::simulate_convergence(
    const mirc_resolved_hardware_t& hw,
    const mirc_rule_complexity_t& comp,
    bool use_streaming_stores,
    const mirc_hardware_sla_t& sla) {

  mirc_qualification_result_t res{};
  res.arch_name = hw.arch_name;
  res.vector_bits = hw.vector_bits;
  res.best_unroll = 8;
  res.q_eff = 0.0;
  res.best_port_pressure = 1.0;
  res.is_qualified = false;
  res.tier = (hw.vector_bits >= 512) ? 1 : ((hw.has_vnni || hw.has_avx2) ? 2 : 3);
  res.disqualify_reason[0] = '\0';

  const double width_factor = static_cast<double>(hw.vector_bits) / 128.0;
  const int unroll_trials[3] = { 8, 4, 2 };

  double best_q_eff = 0.0;
  int best_unroll = 8;
  double best_port_pressure = 1.0;

  for (int t = 0; t < 3; ++t) {
    const int trial_unroll = unroll_trials[t];
    mirc_simulation_step_t& s = res.trajectory[t];
    s.unroll_factor = trial_unroll;

    // Step 1: Baseline Hardware Latency & Reciprocal Throughput
    // s1_baseline = (WidthFactor * 10.0) / (max(0.15, FMA_TP) * max(1.0, FMA_Lat))
    s.s1_baseline_ratio = (width_factor * 10.0) /
                          (std::max(0.15, hw.fma_tp) * std::max(1.0, hw.fma_lat));

    // Step 2: Unrolled Loop & Port Contention Simulation
    // PortPressure = min(1.0, (FMA_TP / 2.0) + (Load_TP / 4.0))
    // Factoring instruction mix: FMA weight + Load weight
    const double fma_weight = static_cast<double>(comp.fma_ops);
    const double ld_weight  = static_cast<double>(comp.load_count > 0 ? comp.load_count : 1);
    const double mix_ratio  = ld_weight / (fma_weight + ld_weight);

    s.s2_port_pressure = std::min(1.0, (hw.fma_tp / 2.0) + (hw.load_tp / 4.0 * (1.0 + mix_ratio)));
    s.s2_port_penalty  = std::max(0.0, (s.s2_port_pressure - 0.40) * 0.65);
    s.s2_port_score    = s.s1_baseline_ratio * std::max(0.20, 1.0 - s.s2_port_penalty);

    // Step 3: Vector Register Liveliness & Stack Spill Risk Estimation
    // 8x unroll -> 14 regs; 4x unroll -> 8 regs; 2x unroll -> 5 regs
    s.s3_regs_needed = (trial_unroll == 8) ? 14 : ((trial_unroll == 4) ? 8 : 5);
    if (s.s3_regs_needed > hw.vector_registers) {
      s.s3_spill_ratio = static_cast<double>(s.s3_regs_needed - hw.vector_registers) /
                         static_cast<double>(hw.vector_registers);
      s.s3_reg_score   = s.s2_port_score / (1.0 + 1.25 * s.s3_spill_ratio);
    } else {
      s.s3_spill_ratio = 0.0;
      s.s3_reg_score   = s.s2_port_score * (hw.has_avx512 ? 1.05 : 1.0);
    }

    // Step 4: Memory Subsystem & Non-Temporal Streaming Convergence
    s.s4_streaming_bonus = 0.15 * std::min(1.0, 1.0 / std::max(0.5, hw.stream_tp));
    const double unroll_scale = std::pow(static_cast<double>(trial_unroll) / 8.0, 0.35);
    s.s4_converged_q_eff = s.s3_reg_score * (1.0 + (use_streaming_stores ? s.s4_streaming_bonus : 0.0)) * unroll_scale;

    if (s.s4_converged_q_eff > best_q_eff) {
      best_q_eff = s.s4_converged_q_eff;
      best_unroll = trial_unroll;
      best_port_pressure = s.s2_port_pressure;
    }

    // codegen.py fast-path: if no register spill at 8x, optimal convergence reached
    if (s.s3_spill_ratio == 0.0 && trial_unroll == 8) {
      break;
    }
  }

  res.q_eff = best_q_eff;
  res.best_unroll = best_unroll;
  res.best_port_pressure = best_port_pressure;

  // Autonomous SLA Verdict Formulation (codegen.py lines 637-645)
  if (sla.require_fma && !hw.has_fma) {
    snprintf(res.disqualify_reason, sizeof(res.disqualify_reason), "Lacks native hardware FMA3 vector execution units");
    res.is_qualified = false;
  } else if (res.q_eff < sla.threshold) {
    snprintf(res.disqualify_reason, sizeof(res.disqualify_reason), "Converged Q_eff (%.2f) fails SLA threshold (%.2f)", res.q_eff, sla.threshold);
    res.is_qualified = false;
  } else if (res.best_port_pressure > sla.max_port_pressure) {
    snprintf(res.disqualify_reason, sizeof(res.disqualify_reason), "Port pressure (%.2f) saturates execution pipeline (> %.2f)", res.best_port_pressure, sla.max_port_pressure);
    res.is_qualified = false;
  } else {
    res.is_qualified = true;
  }

  return res;
}

BranchStrategy DyntoposelSimu::arbitrate(const VAC_BPU_Requirements& vac_bpu, const Hardware_Model& hw) {
  if (vac_bpu.branch_entropy < 30 || (hw.btb_entries > 8192 && vac_bpu.critical_path_depth < 2)) {
    return EMIT_CONDITIONAL_JUMP;
  }
  if (vac_bpu.predication_suitability > 6000 && hw.available_alu_ports >= 2) {
    return EMIT_BRANCHLESS_CMOV;
  }
  if (hw.simd_width_bits >= 256 && vac_bpu.has_vector_context) {
    return EMIT_VECTOR_MASK_BLEND;
  }
  return EMIT_CONDITIONAL_JUMP;
}
