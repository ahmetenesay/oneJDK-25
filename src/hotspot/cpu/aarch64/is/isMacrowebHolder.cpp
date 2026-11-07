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
#elif defined(_WIN32)
#include <windows.h>
#endif

uint64_t isMacrowebHolder::_isa_features = 0;
bool     isMacrowebHolder::_initialized  = false;

void isMacrowebHolder::initialize() {
  if (_initialized) return;

  uint64_t f = 0;

#if defined(__linux__) && defined(__aarch64__)
  unsigned long hwcap = getauxval(AT_HWCAP);
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
#ifdef HWCAP_ATOMICS
  if (hwcap & HWCAP_ATOMICS) f |= FEATURE_ATOMICS_LSE;
#endif
#ifdef HWCAP_SVE
  if (hwcap & HWCAP_SVE) f |= FEATURE_SVE;
#endif
#ifdef HWCAP2_SVE2
  if (hwcap2 & HWCAP2_SVE2) f |= FEATURE_SVE2;
#endif
#ifdef HWCAP2_SVE2_BITPERM
  if (hwcap2 & HWCAP2_SVE2_BITPERM) f |= FEATURE_SVE2_BITPERM;
#endif

#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
  // Windows on ARM64 query
  if (IsProcessorFeaturePresent(PF_ARM_V8_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_FP | FEATURE_NEON;
  }
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_AES | FEATURE_SHA2;
  }
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
    f |= FEATURE_CRC32;
  }
#else
  // Conservative fallback baseline for 64-bit ARM
  f |= FEATURE_FP | FEATURE_NEON;
#endif

  _isa_features = f;
  _initialized  = true;
}
