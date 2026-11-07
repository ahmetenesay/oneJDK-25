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

uint64_t isMacrowebHolder::_isa_features     = 0;
uint32_t isMacrowebHolder::_avx10_version    = 0;
uint32_t isMacrowebHolder::_max_vector_bytes = 8;
bool     isMacrowebHolder::_initialized      = false;

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

static uint64_t read_xcr0_safe(bool osxsave_enabled) {
  if (!osxsave_enabled) return 0;
#if defined(_MSC_VER)
  return _xgetbv(0);
#elif defined(__GNUC__) || defined(__clang__)
  uint32_t eax = 0, edx = 0;
  __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
  return (static_cast<uint64_t>(edx) << 32) | eax;
#else
  return 0;
#endif
}

void isMacrowebHolder::query_cpuid_leaf(int leaf, int subleaf, int out[4]) {
  get_cpuidex(out, leaf, subleaf);
}

uint64_t isMacrowebHolder::query_xcr0() {
  int info[4] = { 0 };
  get_cpuid(info, 1);
  bool osxsave = (info[2] & (1 << 27)) != 0;
  return read_xcr0_safe(osxsave);
}

void isMacrowebHolder::initialize() {
  if (_initialized) return;

  uint64_t f = 0;
  uint32_t avx10_ver = 0;
  uint32_t max_vec_bytes = 8;
  int info[4] = { 0 };

  get_cpuid(info, 0);
  int max_std_level = info[0];

  bool osxsave_enabled = false;
  uint64_t xcr0 = 0;

  if (max_std_level >= 1) {
    get_cpuid(info, 1);
    // EDX: SSE (25), SSE2 (26)
    if (info[3] & (1 << 25)) { f |= FEATURE_SSE;  max_vec_bytes = 16; }
    if (info[3] & (1 << 26)) { f |= FEATURE_SSE2; max_vec_bytes = 16; }

    // ECX: SSE3 (0), SSSE3 (9), FMA (12), SSE4.1 (19), SSE4.2 (20), POPCNT (23), AES (25), OSXSAVE (27), AVX (28)
    if (info[2] & (1 << 0))  f |= FEATURE_SSE3;
    if (info[2] & (1 << 9))  f |= FEATURE_SSSE3;
    if (info[2] & (1 << 19)) f |= FEATURE_SSE4_1;
    if (info[2] & (1 << 20)) f |= FEATURE_SSE4_2;
    if (info[2] & (1 << 23)) f |= FEATURE_POPCNT;
    if (info[2] & (1 << 25)) f |= FEATURE_AES;

    osxsave_enabled = (info[2] & (1 << 27)) != 0;
    xcr0 = read_xcr0_safe(osxsave_enabled);
    bool os_avx_ok = ((xcr0 & 0x6ULL) == 0x6ULL);

    if (os_avx_ok && (info[2] & (1 << 28))) {
      f |= FEATURE_AVX;
      max_vec_bytes = 32;
      if (info[2] & (1 << 12)) f |= FEATURE_FMA;
    }
  }

  // Extended leaf 0x80000001 for LZCNT (ECX bit 5)
  int ext_info[4] = { 0 };
  get_cpuid(ext_info, 0x80000000);
  if (static_cast<uint32_t>(ext_info[0]) >= 0x80000001u) {
    get_cpuid(ext_info, 0x80000001);
    if (ext_info[2] & (1 << 5)) f |= FEATURE_LZCNT;
  }

  bool os_avx_ok    = ((xcr0 & 0x6ULL) == 0x6ULL);
  bool os_avx512_ok = os_avx_ok && ((xcr0 & 0xE0ULL) == 0xE0ULL);
  bool os_apx_ok    = ((xcr0 & (1ULL << 19)) != 0);

  if (max_std_level >= 7) {
    get_cpuidex(info, 7, 0);
    // EBX: BMI1 (3), AVX2 (5), BMI2 (8), AVX512F (16), AVX512DQ (17), CLFLUSHOPT (23), CLWB (24), AVX512CD (28), SHA (29), AVX512BW (30), AVX512VL (31)
    if (info[1] & (1 << 3))  f |= FEATURE_BMI1;
    if (os_avx_ok && (info[1] & (1 << 5))) f |= FEATURE_AVX2;
    if (info[1] & (1 << 8))  f |= FEATURE_BMI2;
    if (info[1] & (1 << 23)) f |= FEATURE_CLFLUSHOPT;
    if (info[1] & (1 << 24)) f |= FEATURE_CLWB;
    if (info[1] & (1 << 29)) f |= FEATURE_SHA;

    if (os_avx512_ok) {
      if (info[1] & (1 << 16)) { f |= FEATURE_AVX512F; max_vec_bytes = 64; }
      if (info[1] & (1 << 17)) f |= FEATURE_AVX512DQ;
      if (info[1] & (1 << 28)) f |= FEATURE_AVX512CD;
      if (info[1] & (1u << 30)) f |= FEATURE_AVX512BW;
      if (info[1] & (1u << 31)) f |= FEATURE_AVX512VL;
    }

    // ECX: AVX512_VBMI (1), WAITPKG (5), AVX512_VBMI2 (6), GFNI (8), VAES (9), VPCLMULQDQ (10), AVX512_VNNI (11), AVX512_BITALG (12), AVX512_VPOPCNTDQ (14), MOVDIRI (27), MOVDIR64B (28)
    if (info[2] & (1 << 5))  f |= FEATURE_WAITPKG;
    if (info[2] & (1 << 8))  f |= FEATURE_GFNI;
    if (os_avx_ok && (info[2] & (1 << 9)))  f |= FEATURE_VAES;
    if (os_avx_ok && (info[2] & (1 << 10))) f |= FEATURE_VPCLMULQDQ;
    if (info[2] & (1 << 27)) f |= FEATURE_MOVDIRI;
    if (info[2] & (1 << 28)) f |= FEATURE_MOVDIR64B;

    if (os_avx512_ok) {
      if (info[2] & (1 << 1))  f |= FEATURE_AVX512_VBMI;
      if (info[2] & (1 << 6))  f |= FEATURE_AVX512_VBMI2;
      if (info[2] & (1 << 11)) f |= FEATURE_VNNI;
      if (info[2] & (1 << 12)) f |= FEATURE_AVX512_BITALG;
      if (info[2] & (1 << 14)) f |= FEATURE_AVX512_VPOPCNTDQ;
    }

    // EDX: SERIALIZE (14), AVX512_FP16 (23), AMX-TILE (24)
    if (info[3] & (1 << 14)) f |= FEATURE_SERIALIZE;
    if (os_avx512_ok && (info[3] & (1 << 23))) f |= FEATURE_AVX512_FP16;
    if ((xcr0 & (0x3ULL << 17)) == (0x3ULL << 17) && (info[3] & (1 << 24))) {
      f |= FEATURE_AMX;
    }

    // Leaf 7 Subleaf 1: AVX-VNNI (EAX bit 4), AVX512_BF16 (EAX bit 5), AVX10 (EDX bit 19), APX_F (EDX bit 21)
    int sub1[4] = { 0 };
    get_cpuidex(sub1, 7, 1);
    if (os_avx_ok && (sub1[0] & (1 << 4))) f |= FEATURE_VNNI;
    if (os_avx512_ok && (sub1[0] & (1 << 5))) f |= FEATURE_AVX512_BF16;

    // Intel AVX10 Converged Vector ISA (EDX bit 19 + Leaf 0x24)
    if (os_avx_ok && (sub1[3] & (1 << 19))) {
      f |= FEATURE_AVX10_1;
      avx10_ver = 1;
      if (max_std_level >= 0x24) {
        int avx10_info[4] = { 0 };
        get_cpuidex(avx10_info, 0x24, 0);
        uint32_t ver = static_cast<uint32_t>(avx10_info[1]) & 0xFFu;
        if (ver > avx10_ver) avx10_ver = ver;
        if (ver >= 2) f |= FEATURE_AVX10_2;
        // EBX bit 18: 512-bit vector support in AVX10
        if (os_avx512_ok && (avx10_info[1] & (1 << 18))) {
          max_vec_bytes = 64;
        } else if (avx10_info[1] & (1 << 17)) {
          if (max_vec_bytes < 32) max_vec_bytes = 32;
        }
      }
    }

    // Intel APX (Advanced Performance Extensions: 32 GPRs R16-R31, NDD, NF, CCMP, PPX)
    if (os_apx_ok && (sub1[3] & (1 << 21))) {
      f |= FEATURE_APX_F | FEATURE_APX_NDD | FEATURE_APX_NF | FEATURE_APX_CCMP | FEATURE_APX_PPX;
      if (max_std_level >= 0x29) {
        int apx_info[4] = { 0 };
        get_cpuidex(apx_info, 0x29, 0);
        // Leaf 0x29 EBX sub-feature bitmask refinement when non-zero
        if (apx_info[1] != 0) {
          if ((apx_info[1] & (1 << 0)) == 0) f &= ~FEATURE_APX_NDD;
          if ((apx_info[1] & (1 << 1)) == 0) f &= ~FEATURE_APX_NF;
          if ((apx_info[1] & (1 << 2)) == 0) f &= ~FEATURE_APX_CCMP;
          if ((apx_info[1] & (1 << 3)) == 0) f &= ~FEATURE_APX_PPX;
        }
      }
    }
  }

  _avx10_version    = avx10_ver;
  _max_vector_bytes = max_vec_bytes;
  _isa_features     = f;
  _initialized      = true;
}
