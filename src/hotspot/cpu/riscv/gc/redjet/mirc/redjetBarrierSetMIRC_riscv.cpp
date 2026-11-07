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

#include "asm/macroAssembler.inline.hpp"
#include "gc/redjet/runtime/redjetBarrierSetC2.hpp"
#include "gc/redjet/runtime/redjetBarrierSet.hpp"
#include "gc/redjet/runtime/redjetBarrierSetAssembler.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"

#ifdef COMPILER2
// Architecture-specific MIRC / C2 match rules and barrier codegen for RISC-V
void redjet_mirc_riscv_emit_pin_barrier(MacroAssembler* masm, Register obj_ptr) {
  assert(obj_ptr != noreg, "obj_ptr must be valid register");
}
#endif
