/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation. Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
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

#ifndef CPU_RISCV_IS_ISMACROWEBHOLDER_HPP
#define CPU_RISCV_IS_ISMACROWEBHOLDER_HPP

#include <cstdint>
#include <cstddef>

class isMacrowebHolder {
private:
  static uint64_t _isa_features;
  static uint32_t _rvv_vlenb;
  static bool     _initialized;

public:
  enum ISAFeatureBits : uint64_t {
    FEATURE_M          = 1ULL << 0,
    FEATURE_A          = 1ULL << 1,
    FEATURE_F          = 1ULL << 2,
    FEATURE_D          = 1ULL << 3,
    FEATURE_C          = 1ULL << 4,
    FEATURE_V          = 1ULL << 5,  // RVV 1.0 Vector Extension
    FEATURE_ZVBB       = 1ULL << 6,  // Vector Basic Bit-manipulation
    FEATURE_ZVBC       = 1ULL << 7,  // Vector Carryless Multiplication
    FEATURE_ZBA        = 1ULL << 8,  // Address Generation Bitmanip
    FEATURE_ZBB        = 1ULL << 9,  // Basic Bit-manipulation (CLZ/CTZ/CPOP/ORC.B)
    FEATURE_ZBC        = 1ULL << 10, // Carry-less Multiplication
    FEATURE_ZBS        = 1ULL << 11, // Single-bit Instructions
    FEATURE_ZFH        = 1ULL << 12, // Half-Precision Floating-Point
    FEATURE_ZICBOM     = 1ULL << 13, // Cache-Block Management (CBO.CLEAN/FLUSH/INVAL)
    FEATURE_ZICBOZ     = 1ULL << 14, // Cache-Block Zero (CBO.ZERO)
    FEATURE_ZIHINTPAUSE= 1ULL << 15  // Spin-wait PAUSE hint
  };

  static void     initialize();
  static uint32_t query_rvv_vlenb();

  static inline uint64_t get_isa_features() {
    if (!_initialized) initialize();
    return _isa_features;
  }

  static inline uint32_t get_max_vector_bytes() {
    if (!_initialized) initialize();
    return _rvv_vlenb;
  }

  static inline bool has_feature(ISAFeatureBits bit) {
    if (!_initialized) initialize();
    return (_isa_features & static_cast<uint64_t>(bit)) != 0;
  }

  // Canonical Universal RISC-V & Cross-ISA Query Interfaces
  static inline bool IS_RVV()          { return has_feature(FEATURE_V); }
  static inline bool IS_ZVBB()         { return has_feature(FEATURE_ZVBB); }
  static inline bool IS_ZVBC()         { return has_feature(FEATURE_ZVBC); }
  static inline bool IS_ZBA()          { return has_feature(FEATURE_ZBA); }
  static inline bool IS_ZBB()          { return has_feature(FEATURE_ZBB); }
  static inline bool IS_ZBC()          { return has_feature(FEATURE_ZBC); }
  static inline bool IS_ZBS()          { return has_feature(FEATURE_ZBS); }
  static inline bool IS_ZICBOM()       { return has_feature(FEATURE_ZICBOM); }
  static inline bool IS_ZICBOZ()       { return has_feature(FEATURE_ZICBOZ); }
  static inline bool IS_ZIHINTPAUSE()  { return has_feature(FEATURE_ZIHINTPAUSE); }
  static inline bool IS_FMA()          { return has_feature(FEATURE_F) && has_feature(FEATURE_D); }
  static inline bool IS_POPCNT()       { return has_feature(FEATURE_ZBB); }
  static inline bool IS_LZCNT()        { return has_feature(FEATURE_ZBB); }

  // Non-RISC-V universal queries safely return false
  static inline bool IS_APX()          { return false; }
  static inline bool IS_APX_NDD()      { return false; }
  static inline bool IS_APX_NF()       { return false; }
  static inline bool IS_AVX512()       { return false; }
  static inline bool IS_AVX10()        { return false; }
  static inline bool IS_AVX10_2()      { return false; }
  static inline bool IS_AVX2()         { return false; }
  static inline bool IS_AVX()          { return false; }
  static inline bool IS_SSE()          { return false; }
  static inline bool IS_SSE2()         { return false; }
  static inline bool IS_SSE4_2()       { return false; }
  static inline bool IS_BMI1()         { return false; }
  static inline bool IS_BMI2()         { return false; }
  static inline bool IS_SVE()          { return false; }
  static inline bool IS_SVE2()         { return false; }
  static inline bool IS_SME()          { return false; }
  static inline bool IS_SME2()         { return false; }
  static inline bool IS_LSE()          { return has_feature(FEATURE_A); }
  static inline bool IS_NEON()         { return false; }
  static inline bool IS_AMX()          { return false; }
  static inline bool IS_VNNI()         { return false; }

  // Hierarchical Fallback Resolution for RISC-V: RVV+Zvbb -> RVV 1.0 -> Bitmanip(Zba/Zbb) -> FD_FMA -> Scalar
  enum ISALevel {
    ISA_SCALAR        = 0,
    ISA_FD_FMA        = 1,
    ISA_BITMANIP_ZBB  = 2,
    ISA_RVV           = 3,
    ISA_RVV_ZVBB      = 4
  };

  static inline bool can_satisfy(ISALevel level) {
    switch (level) {
      case ISA_RVV_ZVBB:     return IS_RVV() && IS_ZVBB();
      case ISA_RVV:          return IS_RVV();
      case ISA_BITMANIP_ZBB: return IS_ZBA() && IS_ZBB();
      case ISA_FD_FMA:       return IS_FMA();
      case ISA_SCALAR:       return true;
      default:               return false;
    }
  }

  static inline ISALevel resolve_fallback_down(ISALevel requested) {
    for (int lvl = static_cast<int>(requested); lvl >= static_cast<int>(ISA_SCALAR); --lvl) {
      if (can_satisfy(static_cast<ISALevel>(lvl))) {
        return static_cast<ISALevel>(lvl);
      }
    }
    return ISA_SCALAR;
  }

  static inline ISALevel resolve_closest_isa(ISALevel requested) {
    if (can_satisfy(requested)) return requested;
    for (int lvl = static_cast<int>(requested) + 1; lvl <= static_cast<int>(ISA_RVV_ZVBB); ++lvl) {
      if (can_satisfy(static_cast<ISALevel>(lvl))) {
        return static_cast<ISALevel>(lvl);
      }
    }
    return resolve_fallback_down(requested);
  }

  static inline ISALevel resolve_best_vector_isa() {
    if (IS_RVV() && IS_ZVBB()) return ISA_RVV_ZVBB;
    if (IS_RVV())              return ISA_RVV;
    if (IS_ZBA() && IS_ZBB())  return ISA_BITMANIP_ZBB;
    if (IS_FMA())              return ISA_FD_FMA;
    return ISA_SCALAR;
  }

  // Universal Command & Closest-ISA Accessors
  static inline ISALevel REQUEST_ISA(ISALevel requested) { return resolve_closest_isa(requested); }
  static inline ISALevel REQUEST_BEST_ISA()              { return resolve_best_vector_isa(); }
  static inline bool     CAN_EXECUTE(ISALevel level)     { return can_satisfy(level); }

  static inline ISALevel GET_RVV_ZVBB_OR_FALLBACK() { return resolve_closest_isa(ISA_RVV_ZVBB); }
  static inline ISALevel GET_RVV_OR_FALLBACK()      { return resolve_closest_isa(ISA_RVV); }
  static inline ISALevel GET_ZBB_OR_FALLBACK()      { return resolve_closest_isa(ISA_BITMANIP_ZBB); }
  static inline ISALevel GET_FMA_OR_FALLBACK()      { return resolve_closest_isa(ISA_FD_FMA); }

  static inline const char* get_isa_name(ISALevel lvl) {
    switch (lvl) {
      case ISA_RVV_ZVBB:     return "RVV_ZVBB";
      case ISA_RVV:          return "RVV_1.0";
      case ISA_BITMANIP_ZBB: return "ZBA_ZBB";
      case ISA_FD_FMA:       return "FD_FMA";
      case ISA_SCALAR:       return "SCALAR";
      default:               return "UNKNOWN";
    }
  }
};

#endif // CPU_RISCV_IS_ISMACROWEBHOLDER_HPP
