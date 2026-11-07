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

#if defined(__linux__) && defined(__riscv)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

uint64_t isMacrowebHolder::_isa_features = 0;
uint32_t isMacrowebHolder::_rvv_vlenb    = 8;
bool     isMacrowebHolder::_initialized  = false;

#if defined(__linux__) && defined(__riscv)
struct riscv_hwprobe_pair_local {
  int64_t  key;
  uint64_t value;
};
#endif

uint32_t isMacrowebHolder::query_rvv_vlenb() {
#if defined(__riscv_vector)
  unsigned long vlenb = 0;
  __asm__ __volatile__("csrr %0, vlenb" : "=r"(vlenb));
  if (vlenb >= 16) return static_cast<uint32_t>(vlenb);
#endif
  return 16;
}

void isMacrowebHolder::initialize() {
  if (_initialized) return;

  uint64_t f = 0;
  uint32_t vlenb = 8;

#if defined(__linux__) && defined(__riscv)
  unsigned long hwcap = getauxval(AT_HWCAP);
  if (hwcap & (1UL << ('M' - 'A'))) f |= FEATURE_M;
  if (hwcap & (1UL << ('A' - 'A'))) f |= FEATURE_A;
  if (hwcap & (1UL << ('F' - 'A'))) f |= FEATURE_F;
  if (hwcap & (1UL << ('D' - 'A'))) f |= FEATURE_D;
  if (hwcap & (1UL << ('C' - 'A'))) f |= FEATURE_C;
  if (hwcap & (1UL << ('V' - 'A'))) {
    f |= FEATURE_V;
    vlenb = query_rvv_vlenb();
  }

#ifdef __NR_riscv_hwprobe
  // Linux 6.4+ riscv_hwprobe key 4 = RISCV_HWPROBE_KEY_IMA_EXT_0
  riscv_hwprobe_pair_local probe = { 4, 0 };
  if (syscall(__NR_riscv_hwprobe, &probe, 1, 0, nullptr, 0) == 0) {
    const uint64_t ext = probe.value;
    if (ext & (1ULL << 2))  { f |= FEATURE_V; if (vlenb < 16) vlenb = 16; } // IMA_V
    if (ext & (1ULL << 3))  f |= FEATURE_ZBA;                                // ZBA
    if (ext & (1ULL << 4))  f |= FEATURE_ZBB;                                // ZBB
    if (ext & (1ULL << 5))  f |= FEATURE_ZBS;                                // ZBS
    if (ext & (1ULL << 6))  f |= FEATURE_ZICBOZ;                             // ZICBOZ
    if (ext & (1ULL << 7))  f |= FEATURE_ZBC;                                // ZBC
    if (ext & (1ULL << 17)) f |= FEATURE_ZVBB;                               // ZVBB
    if (ext & (1ULL << 18)) f |= FEATURE_ZVBC;                               // ZVBC
    if (ext & (1ULL << 27)) f |= FEATURE_ZFH;                                // ZFH
    if (ext & (1ULL << 35)) f |= FEATURE_ZIHINTPAUSE;                        // ZIHINTPAUSE
  }
#endif
#else
  // Standard RV64GC baseline
  f |= FEATURE_M | FEATURE_A | FEATURE_F | FEATURE_D | FEATURE_C;
#ifdef __riscv_zba
  f |= FEATURE_ZBA;
#endif
#ifdef __riscv_zbb
  f |= FEATURE_ZBB;
#endif
#ifdef __riscv_zbc
  f |= FEATURE_ZBC;
#endif
#ifdef __riscv_zbs
  f |= FEATURE_ZBS;
#endif
#ifdef __riscv_vector
  f |= FEATURE_V;
  vlenb = 16;
#endif
#endif

  _rvv_vlenb    = vlenb;
  _isa_features = f;
  _initialized  = true;
}
