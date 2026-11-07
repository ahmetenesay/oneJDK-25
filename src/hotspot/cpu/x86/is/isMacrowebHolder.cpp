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

#include "is/isMacrowebHolder.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

uint64_t isMacrowebHolder::_isa_features = 0;
bool     isMacrowebHolder::_initialized  = false;

static void get_cpuid(int cpuInfo[4], int function_id) {
#if defined(_MSC_VER)
  __cpuid(cpuInfo, function_id);
#elif defined(__GNUC__) || defined(__clang__)
  __cpuid(function_id, cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);
#else
  cpuInfo[0] = cpuInfo[1] = cpuInfo[2] = cpuInfo[3] = 0;
#endif
}

static void get_cpuidex(int cpuInfo[4], int function_id, int subfunction_id) {
#if defined(_MSC_VER)
  __cpuidex(cpuInfo, function_id, subfunction_id);
#elif defined(__GNUC__) || defined(__clang__)
  __cpuid_count(function_id, subfunction_id, cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);
#else
  cpuInfo[0] = cpuInfo[1] = cpuInfo[2] = cpuInfo[3] = 0;
#endif
}

void isMacrowebHolder::initialize() {
  if (_initialized) return;

  uint64_t f = 0;
  int info[4] = { 0 };

  get_cpuid(info, 0);
  int max_std_level = info[0];

  if (max_std_level >= 1) {
    get_cpuid(info, 1);
    // ECX flags:
    // bit 19: SSE4.2
    // bit 12: FMA
    // bit 23: POPCNT
    // bit 28: AVX
    if (info[2] & (1 << 19)) f |= FEATURE_SSE4_2;
    if (info[2] & (1 << 12)) f |= FEATURE_FMA;
    if (info[2] & (1 << 23)) f |= FEATURE_POPCNT;
    if (info[2] & (1 << 28)) f |= FEATURE_AVX;
  }

  if (max_std_level >= 7) {
    get_cpuidex(info, 7, 0);
    // EBX flags:
    // bit 3: BMI1
    // bit 5: AVX2
    // bit 8: BMI2
    // bit 16: AVX512F
    // bit 17: AVX512DQ
    // bit 28: AVX512CD
    // bit 30: AVX512BW
    // bit 31: AVX512VL
    if (info[1] & (1 << 3))  f |= FEATURE_BMI1;
    if (info[1] & (1 << 5))  f |= FEATURE_AVX2;
    if (info[1] & (1 << 8))  f |= FEATURE_BMI2;
    if (info[1] & (1 << 16)) f |= FEATURE_AVX512F;
    if (info[1] & (1 << 17)) f |= FEATURE_AVX512DQ;
    if (info[1] & (1 << 28)) f |= FEATURE_AVX512CD;
    if (info[1] & (1 << 30)) f |= FEATURE_AVX512BW;
    if (info[1] & (1 << 31)) f |= FEATURE_AVX512VL;

    // ECX flags:
    // bit 11: VNNI
    if (info[2] & (1 << 11)) f |= FEATURE_VNNI;

    // EDX flags:
    // bit 22: AMX-BF16, bit 24: AMX-TILE, bit 25: AMX-INT8
    if (info[3] & (1 << 24)) f |= FEATURE_AMX;

    // Leaf 7 Subleaf 1: AVX10 detection
    get_cpuidex(info, 7, 1);
    // EDX bit 19: AVX10
    if (info[3] & (1 << 19)) {
      f |= FEATURE_AVX10_1;
      // Leaf 0x24: AVX10 Converged Vector ISA properties
      int avx10_info[4] = { 0 };
      get_cpuid(avx10_info, 0x24);
      uint32_t avx10_version = avx10_info[1] & 0xFF;
      if (avx10_version >= 2) {
        f |= FEATURE_AVX10_2;
      }
    }
  }

  _isa_features = f;
  _initialized  = true;
}
