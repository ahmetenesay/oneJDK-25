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

#ifndef CPU_AARCH64_IS_ISMACROWEBHOLDER_HPP
#define CPU_AARCH64_IS_ISMACROWEBHOLDER_HPP

#include <cstdint>
#include <cstddef>

class isMacrowebHolder {
private:
  static uint64_t _isa_features;
  static bool     _initialized;

public:
  enum ISAFeatureBits : uint64_t {
    FEATURE_FP            = 1ULL << 0,
    FEATURE_NEON          = 1ULL << 1,
    FEATURE_CRC32         = 1ULL << 2,
    FEATURE_AES           = 1ULL << 3,
    FEATURE_SHA2          = 1ULL << 4,
    FEATURE_SVE           = 1ULL << 5,
    FEATURE_SVE2          = 1ULL << 6,
    FEATURE_SVE2_BITPERM  = 1ULL << 7,
    FEATURE_ATOMICS_LSE   = 1ULL << 8
  };

  static void initialize();

  static inline uint64_t get_isa_features() {
    if (!_initialized) initialize();
    return _isa_features;
  }

  static inline bool has_feature(ISAFeatureBits bit) {
    if (!_initialized) initialize();
    return (_isa_features & static_cast<uint64_t>(bit)) != 0;
  }

  // Canonical Universal ISA Query Interfaces
  static inline bool IS_NEON()    { return has_feature(FEATURE_NEON); }
  static inline bool IS_SVE()     { return has_feature(FEATURE_SVE); }
  static inline bool IS_SVE2()    { return has_feature(FEATURE_SVE2); }
  static inline bool IS_AVX512()  { return false; }
  static inline bool IS_AVX10()   { return false; }
  static inline bool IS_AVX10_2() { return false; }
  static inline bool IS_AVX2()    { return false; }
  static inline bool IS_AVX()     { return false; }
  static inline bool IS_SSE()     { return false; }
  static inline bool IS_SSE2()    { return false; }
  static inline bool IS_SSE4_2()  { return false; }
  static inline bool IS_FMA()     { return has_feature(FEATURE_FP); }
  static inline bool IS_BMI2()    { return false; }
  static inline bool IS_AMX()     { return false; }
  static inline bool IS_VNNI()    { return false; }
  static inline bool IS_RVV()     { return false; }

  // Hierarchical Fallback Resolution for ARM: SVE2 -> SVE -> NEON -> Scalar
  enum ISALevel {
    ISA_SCALAR = 0,
    ISA_NEON   = 1,
    ISA_SVE    = 2,
    ISA_SVE2   = 3
  };

  static inline bool can_satisfy(ISALevel level) {
    switch (level) {
      case ISA_SVE2:   return IS_SVE2();
      case ISA_SVE:    return IS_SVE();
      case ISA_NEON:   return IS_NEON();
      case ISA_SCALAR: return true;
      default:         return false;
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
    for (int lvl = static_cast<int>(requested) + 1; lvl <= static_cast<int>(ISA_SVE2); ++lvl) {
      if (can_satisfy(static_cast<ISALevel>(lvl))) {
        return static_cast<ISALevel>(lvl);
      }
    }
    return resolve_fallback_down(requested);
  }

  static inline ISALevel resolve_best_vector_isa() {
    if (IS_SVE2()) return ISA_SVE2;
    if (IS_SVE())  return ISA_SVE;
    if (IS_NEON()) return ISA_NEON;
    return ISA_SCALAR;
  }

  // Universal Command & Fallback Accessors
  static inline ISALevel REQUEST_ISA(ISALevel requested) { return resolve_closest_isa(requested); }
  static inline ISALevel REQUEST_BEST_ISA()              { return resolve_best_vector_isa(); }
  static inline bool     CAN_EXECUTE(ISALevel level)     { return can_satisfy(level); }

  static inline ISALevel GET_SVE2_OR_FALLBACK() { return resolve_closest_isa(ISA_SVE2); }
  static inline ISALevel GET_SVE_OR_FALLBACK()  { return resolve_closest_isa(ISA_SVE); }
  static inline ISALevel GET_NEON_OR_FALLBACK() { return resolve_closest_isa(ISA_NEON); }

  static inline const char* get_isa_name(ISALevel lvl) {
    switch (lvl) {
      case ISA_SVE2:   return "SVE2";
      case ISA_SVE:    return "SVE";
      case ISA_NEON:   return "NEON";
      case ISA_SCALAR: return "SCALAR";
      default:         return "UNKNOWN";
    }
  }
};

#endif // CPU_AARCH64_IS_ISMACROWEBHOLDER_HPP
