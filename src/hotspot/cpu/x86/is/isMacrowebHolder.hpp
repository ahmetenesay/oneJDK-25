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

#ifndef CPU_X86_IS_ISMACROWEBHOLDER_HPP
#define CPU_X86_IS_ISMACROWEBHOLDER_HPP

#include <cstdint>
#include <cstddef>

class isMacrowebHolder {
private:
  static uint64_t _isa_features;
  static uint32_t _avx10_version;
  static uint32_t _max_vector_bytes;
  static bool     _initialized;

public:
  enum ISAFeatureBits : uint64_t {
    FEATURE_SSE              = 1ULL << 0,
    FEATURE_SSE2             = 1ULL << 1,
    FEATURE_SSE3             = 1ULL << 2,
    FEATURE_SSSE3            = 1ULL << 3,
    FEATURE_SSE4_1           = 1ULL << 4,
    FEATURE_SSE4_2           = 1ULL << 5,
    FEATURE_AVX              = 1ULL << 6,
    FEATURE_AVX2             = 1ULL << 7,
    FEATURE_FMA              = 1ULL << 8,
    FEATURE_BMI1             = 1ULL << 9,
    FEATURE_BMI2             = 1ULL << 10,
    FEATURE_AVX512F          = 1ULL << 11,
    FEATURE_AVX512CD         = 1ULL << 12,
    FEATURE_AVX512BW         = 1ULL << 13,
    FEATURE_AVX512DQ         = 1ULL << 14,
    FEATURE_AVX512VL         = 1ULL << 15,
    FEATURE_AVX10_1          = 1ULL << 16,
    FEATURE_AVX10_2          = 1ULL << 17,
    FEATURE_VNNI             = 1ULL << 18,
    FEATURE_AMX              = 1ULL << 19,
    FEATURE_POPCNT           = 1ULL << 20,
    FEATURE_AES              = 1ULL << 21,
    FEATURE_SHA              = 1ULL << 22,
    FEATURE_APX_F            = 1ULL << 23,
    FEATURE_APX_NDD          = 1ULL << 24,
    FEATURE_APX_NF           = 1ULL << 25,
    FEATURE_APX_CCMP         = 1ULL << 26,
    FEATURE_APX_PPX          = 1ULL << 27,
    FEATURE_AVX512_VBMI      = 1ULL << 28,
    FEATURE_AVX512_VBMI2     = 1ULL << 29,
    FEATURE_AVX512_BITALG    = 1ULL << 30,
    FEATURE_AVX512_VPOPCNTDQ = 1ULL << 31,
    FEATURE_AVX512_FP16      = 1ULL << 32,
    FEATURE_AVX512_BF16      = 1ULL << 33,
    FEATURE_GFNI             = 1ULL << 34,
    FEATURE_VAES             = 1ULL << 35,
    FEATURE_VPCLMULQDQ       = 1ULL << 36,
    FEATURE_CLFLUSHOPT       = 1ULL << 37,
    FEATURE_CLWB             = 1ULL << 38,
    FEATURE_MOVDIRI          = 1ULL << 39,
    FEATURE_MOVDIR64B        = 1ULL << 40,
    FEATURE_WAITPKG          = 1ULL << 41,
    FEATURE_SERIALIZE        = 1ULL << 42,
    FEATURE_LZCNT            = 1ULL << 43
  };

  static void     initialize();
  static void     query_cpuid_leaf(int leaf, int subleaf, int out[4]);
  static uint64_t query_xcr0();

  static inline uint64_t get_isa_features() {
    if (!_initialized) initialize();
    return _isa_features;
  }

  static inline uint32_t get_avx10_version() {
    if (!_initialized) initialize();
    return _avx10_version;
  }

  static inline uint32_t get_max_vector_bytes() {
    if (!_initialized) initialize();
    return _max_vector_bytes;
  }

  static inline bool has_feature(ISAFeatureBits bit) {
    if (!_initialized) initialize();
    return (_isa_features & static_cast<uint64_t>(bit)) != 0;
  }

  // Canonical Universal x86 & Cross-ISA Query Interfaces
  static inline bool IS_SSE()              { return has_feature(FEATURE_SSE); }
  static inline bool IS_SSE2()             { return has_feature(FEATURE_SSE2); }
  static inline bool IS_SSE3()             { return has_feature(FEATURE_SSE3); }
  static inline bool IS_SSSE3()            { return has_feature(FEATURE_SSSE3); }
  static inline bool IS_SSE4_1()           { return has_feature(FEATURE_SSE4_1); }
  static inline bool IS_SSE4_2()           { return has_feature(FEATURE_SSE4_2); }
  static inline bool IS_AVX()              { return has_feature(FEATURE_AVX); }
  static inline bool IS_AVX2()             { return has_feature(FEATURE_AVX2); }
  static inline bool IS_FMA()              { return has_feature(FEATURE_FMA); }
  static inline bool IS_BMI1()             { return has_feature(FEATURE_BMI1); }
  static inline bool IS_BMI2()             { return has_feature(FEATURE_BMI2); }
  static inline bool IS_LZCNT()            { return has_feature(FEATURE_LZCNT); }
  static inline bool IS_POPCNT()           { return has_feature(FEATURE_POPCNT); }
  static inline bool IS_AVX512()           { return has_feature(FEATURE_AVX512F) && has_feature(FEATURE_AVX512BW) && has_feature(FEATURE_AVX512VL); }
  static inline bool IS_AVX512_VBMI2()     { return IS_AVX512() && has_feature(FEATURE_AVX512_VBMI2); }
  static inline bool IS_AVX512_VPOPCNTDQ() { return IS_AVX512() && has_feature(FEATURE_AVX512_VPOPCNTDQ); }
  static inline bool IS_AVX512_FP16()      { return IS_AVX512() && has_feature(FEATURE_AVX512_FP16); }
  static inline bool IS_AVX512_BF16()      { return IS_AVX512() && has_feature(FEATURE_AVX512_BF16); }
  static inline bool IS_AVX10()            { return has_feature(FEATURE_AVX10_1) || has_feature(FEATURE_AVX10_2); }
  static inline bool IS_AVX10_2()          { return has_feature(FEATURE_AVX10_2); }
  static inline bool IS_APX()              { return has_feature(FEATURE_APX_F); }
  static inline bool IS_APX_NDD()          { return has_feature(FEATURE_APX_NDD); }
  static inline bool IS_APX_NF()           { return has_feature(FEATURE_APX_NF); }
  static inline bool IS_APX_CCMP()         { return has_feature(FEATURE_APX_CCMP); }
  static inline bool IS_APX_PPX()          { return has_feature(FEATURE_APX_PPX); }
  static inline bool IS_VNNI()             { return has_feature(FEATURE_VNNI); }
  static inline bool IS_AMX()              { return has_feature(FEATURE_AMX); }
  static inline bool IS_GFNI()             { return has_feature(FEATURE_GFNI); }
  static inline bool IS_VAES()             { return has_feature(FEATURE_VAES); }
  static inline bool IS_CLWB()             { return has_feature(FEATURE_CLWB); }
  static inline bool IS_CLFLUSHOPT()       { return has_feature(FEATURE_CLFLUSHOPT); }
  static inline bool IS_MOVDIR64B()        { return has_feature(FEATURE_MOVDIR64B); }
  static inline bool IS_WAITPKG()          { return has_feature(FEATURE_WAITPKG); }
  static inline bool IS_SERIALIZE()        { return has_feature(FEATURE_SERIALIZE); }

  // Non-x86 universal queries safely return false on x86
  static inline bool IS_NEON()             { return false; }
  static inline bool IS_SVE()              { return false; }
  static inline bool IS_SVE2()             { return false; }
  static inline bool IS_SME()              { return false; }
  static inline bool IS_SME2()             { return false; }
  static inline bool IS_LSE()              { return false; }
  static inline bool IS_RVV()              { return false; }
  static inline bool IS_ZBA()              { return false; }
  static inline bool IS_ZBB()              { return false; }
  static inline bool IS_ZBC()              { return false; }
  static inline bool IS_ZBS()              { return false; }

  // Hierarchical Universal ISA Ladder (with bidirectional step-up / fallback-down)
  enum ISALevel {
    ISA_SCALAR  = 0,
    ISA_SSE     = 1,
    ISA_SSE2    = 2,
    ISA_SSE3    = 3,
    ISA_SSSE3   = 4,
    ISA_SSE4_1  = 5,
    ISA_SSE4_2  = 6,
    ISA_AVX     = 7,
    ISA_AVX2    = 8,
    ISA_AVX512  = 9,
    ISA_AVX10   = 10,
    ISA_AVX10_2 = 11,
    ISA_APX     = 12
  };

  static inline bool can_satisfy(ISALevel level) {
    switch (level) {
      case ISA_APX:     return IS_APX();
      case ISA_AVX10_2: return IS_AVX10_2();
      case ISA_AVX10:   return IS_AVX10();
      case ISA_AVX512:  return IS_AVX512();
      case ISA_AVX2:    return IS_AVX2();
      case ISA_AVX:     return IS_AVX();
      case ISA_SSE4_2:  return IS_SSE4_2();
      case ISA_SSE4_1:  return IS_SSE4_1();
      case ISA_SSSE3:   return IS_SSSE3();
      case ISA_SSE3:    return IS_SSE3();
      case ISA_SSE2:    return IS_SSE2();
      case ISA_SSE:     return IS_SSE();
      case ISA_SCALAR:  return true;
      default:          return false;
    }
  }

  // Fallback downwards to nearest lower supported tier
  static inline ISALevel resolve_fallback_down(ISALevel requested) {
    for (int lvl = static_cast<int>(requested); lvl >= static_cast<int>(ISA_SCALAR); --lvl) {
      if (can_satisfy(static_cast<ISALevel>(lvl))) {
        return static_cast<ISALevel>(lvl);
      }
    }
    return ISA_SCALAR;
  }

  // Step upwards one or more tiers if requested tier is superseded/unavailable, else fall back down
  static inline ISALevel resolve_closest_isa(ISALevel requested) {
    if (can_satisfy(requested)) return requested;
    for (int lvl = static_cast<int>(requested) + 1; lvl <= static_cast<int>(ISA_APX); ++lvl) {
      if (can_satisfy(static_cast<ISALevel>(lvl))) {
        return static_cast<ISALevel>(lvl);
      }
    }
    return resolve_fallback_down(requested);
  }

  static inline ISALevel resolve_best_vector_isa() {
    if (IS_AVX10_2())          return ISA_AVX10_2;
    if (IS_AVX10())            return ISA_AVX10;
    if (IS_AVX512())           return ISA_AVX512;
    if (IS_AVX2() && IS_FMA()) return ISA_AVX2;
    if (IS_AVX2())             return ISA_AVX2;
    if (IS_AVX())              return ISA_AVX;
    if (IS_SSE4_2())           return ISA_SSE4_2;
    if (IS_SSE4_1())           return ISA_SSE4_1;
    if (IS_SSSE3())            return ISA_SSSE3;
    if (IS_SSE3())             return ISA_SSE3;
    if (IS_SSE2())             return ISA_SSE2;
    if (IS_SSE())              return ISA_SSE;
    return ISA_SCALAR;
  }

  // Universal Command & Closest-ISA Accessors
  static inline ISALevel REQUEST_ISA(ISALevel requested) { return resolve_closest_isa(requested); }
  static inline ISALevel REQUEST_BEST_ISA()              { return resolve_best_vector_isa(); }
  static inline bool     CAN_EXECUTE(ISALevel level)     { return can_satisfy(level); }

  static inline ISALevel GET_APX_OR_FALLBACK()    { return resolve_closest_isa(ISA_APX); }
  static inline ISALevel GET_AVX10_2_OR_FALLBACK(){ return resolve_closest_isa(ISA_AVX10_2); }
  static inline ISALevel GET_AVX10_OR_FALLBACK()  { return resolve_closest_isa(ISA_AVX10); }
  static inline ISALevel GET_AVX512_OR_FALLBACK() { return resolve_closest_isa(ISA_AVX512); }
  static inline ISALevel GET_AVX2_OR_FALLBACK()   { return resolve_closest_isa(ISA_AVX2); }
  static inline ISALevel GET_AVX_OR_FALLBACK()    { return resolve_closest_isa(ISA_AVX); }
  static inline ISALevel GET_SSE_OR_FALLBACK()    { return resolve_closest_isa(ISA_SSE4_2); }
  static inline ISALevel GET_FMA_OR_FALLBACK()    { return IS_FMA() ? resolve_closest_isa(ISA_AVX2) : resolve_fallback_down(ISA_SSE4_2); }
  static inline ISALevel GET_BMI2_OR_FALLBACK()   { return IS_BMI2() ? resolve_closest_isa(ISA_AVX2) : ISA_SCALAR; }

  static inline const char* get_isa_name(ISALevel lvl) {
    switch (lvl) {
      case ISA_APX:     return "APX";
      case ISA_AVX10_2: return "AVX10.2";
      case ISA_AVX10:   return "AVX10.1";
      case ISA_AVX512:  return "AVX-512";
      case ISA_AVX2:    return "AVX2";
      case ISA_AVX:     return "AVX";
      case ISA_SSE4_2:  return "SSE4.2";
      case ISA_SSE4_1:  return "SSE4.1";
      case ISA_SSSE3:   return "SSSE3";
      case ISA_SSE3:    return "SSE3";
      case ISA_SSE2:    return "SSE2";
      case ISA_SSE:     return "SSE";
      case ISA_SCALAR:  return "SCALAR";
      default:          return "UNKNOWN";
    }
  }
};

#endif // CPU_X86_IS_ISMACROWEBHOLDER_HPP
