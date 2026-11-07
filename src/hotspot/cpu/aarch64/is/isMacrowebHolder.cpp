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

#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <sys/prctl.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

uint64_t isMacrowebHolder::_isa_features     = 0;
uint32_t isMacrowebHolder::_sve_vector_bytes = 16;
bool     isMacrowebHolder::_initialized      = false;

uint64_t isMacrowebHolder::query_midr_el1() {
#if defined(__linux__) && defined(__aarch64__)
#ifdef HWCAP_CPUID
  if (getauxval(AT_HWCAP) & HWCAP_CPUID) {
    uint64_t midr = 0;
    __asm__ __volatile__("mrs %0, midr_el1" : "=r"(midr));
    return midr;
  }
#endif
#endif
  return 0;
}

uint32_t isMacrowebHolder::query_sve_vector_length_bytes() {
#if defined(__linux__) && defined(__aarch64__) && defined(PR_SVE_GET_VL)
  long vl = prctl(PR_SVE_GET_VL);
  if (vl > 0) {
    return static_cast<uint32_t>(vl & 0xFFFF);
  }
#endif
  return 16;
}

void isMacrowebHolder::initialize() {
  if (_initialized) return;

  uint64_t f = 0;
  uint32_t vec_bytes = 16;

#if defined(__linux__) && defined(__aarch64__)
  unsigned long hwcap  = getauxval(AT_HWCAP);
  unsigned long hwcap2 = getauxval(AT_HWCAP2);

#ifdef HWCAP_FP
  if (hwcap & HWCAP_FP) f |= FEATURE_FP;
#endif
#ifdef HWCAP_ASIMD
  if (hwcap & HWCAP_ASIMD) f |= FEATURE_NEON;
#endif
#ifdef HWCAP_CRC32
  if (hwcap & HWCAP_CRC32) f |= FEATURE_CRC32;
#endif
#ifdef HWCAP_AES
  if (hwcap & HWCAP_AES) f |= FEATURE_AES;
#endif
#ifdef HWCAP_SHA2
  if (hwcap & HWCAP_SHA2) f |= FEATURE_SHA2;
#endif
#ifdef HWCAP_SHA3
  if (hwcap & HWCAP_SHA3) f |= FEATURE_SHA3;
#endif
#ifdef HWCAP_SHA512
  if (hwcap & HWCAP_SHA512) f |= FEATURE_SHA512;
#endif
#ifdef HWCAP_ATOMICS
  if (hwcap & HWCAP_ATOMICS) f |= FEATURE_ATOMICS_LSE;
#endif
#ifdef HWCAP_ASIMDDP
  if (hwcap & HWCAP_ASIMDDP) f |= FEATURE_DOTPROD;
#endif
#ifdef HWCAP_SVE
  if (hwcap & HWCAP_SVE) {
    f |= FEATURE_SVE;
    vec_bytes = query_sve_vector_length_bytes();
  }
#endif
#ifdef HWCAP2_SVE2
  if (hwcap2 & HWCAP2_SVE2) f |= FEATURE_SVE2;
#endif
#ifdef HWCAP2_SVE2_BITPERM
  if (hwcap2 & HWCAP2_SVE2_BITPERM) f |= FEATURE_SVE2_BITPERM;
#endif
#ifdef HWCAP2_BF16
  if (hwcap2 & HWCAP2_BF16) f |= FEATURE_BF16;
#endif
#ifdef HWCAP2_I8MM
  if (hwcap2 & HWCAP2_I8MM) f |= FEATURE_I8MM;
#endif
#ifdef HWCAP2_SME
  if (hwcap2 & HWCAP2_SME) f |= FEATURE_SME;
#endif
#ifdef HWCAP2_SME2
  if (hwcap2 & HWCAP2_SME2) f |= FEATURE_SME2;
#endif
#ifdef HWCAP2_MOPS
  if (hwcap2 & HWCAP2_MOPS) f |= FEATURE_MOPS;
#endif
#ifdef HWCAP2_CSSC
  if (hwcap2 & HWCAP2_CSSC) f |= FEATURE_CSSC;
#endif
#ifdef HWCAP2_WFXT
  if (hwcap2 & HWCAP2_WFXT) f |= FEATURE_WFXT;
#endif

#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
  if (IsProcessorFeaturePresent(PF_ARM_V8_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_FP | FEATURE_NEON;
  }
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_AES | FEATURE_SHA2;
  }
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_CRC32;
  }
#ifdef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
  if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_ATOMICS_LSE;
  }
#endif
#ifdef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
  if (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_DOTPROD;
  }
#endif
#ifdef PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
  if (IsProcessorFeaturePresent(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_SVE;
    vec_bytes = 32;
  }
#endif
#ifdef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
  if (IsProcessorFeaturePresent(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_SVE2;
    vec_bytes = 32;
  }
#endif
#else
  f |= FEATURE_FP | FEATURE_NEON;
#endif

  _sve_vector_bytes = vec_bytes;
  _isa_features     = f;
  _initialized      = true;
}
