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
#endif

uint64_t isMacrowebHolder::_isa_features = 0;
bool     isMacrowebHolder::_initialized  = false;

void isMacrowebHolder::initialize() {
  if (_initialized) return;

  uint64_t f = 0;

#if defined(__linux__) && defined(__riscv)
  unsigned long hwcap = getauxval(AT_HWCAP);
  // RISC-V standard hwcap bits:
  // 'M' = 1 << ('M' - 'A') = 1 << 12
  // 'A' = 1 << 0
  // 'F' = 1 << ('F' - 'A') = 1 << 5
  // 'D' = 1 << ('D' - 'A') = 1 << 3
  // 'C' = 1 << ('C' - 'A') = 1 << 2
  // 'V' = 1 << ('V' - 'A') = 1 << 21
  if (hwcap & (1 << ('M' - 'A'))) f |= FEATURE_M;
  if (hwcap & (1 << ('A' - 'A'))) f |= FEATURE_A;
  if (hwcap & (1 << ('F' - 'A'))) f |= FEATURE_F;
  if (hwcap & (1 << ('D' - 'A'))) f |= FEATURE_D;
  if (hwcap & (1 << ('C' - 'A'))) f |= FEATURE_C;
  if (hwcap & (1 << ('V' - 'A'))) f |= FEATURE_V;
#else
  // Standard baseline: RV64GC (IMAFDC)
  f |= FEATURE_M | FEATURE_A | FEATURE_F | FEATURE_D | FEATURE_C;
#endif

  _isa_features = f;
  _initialized  = true;
}
