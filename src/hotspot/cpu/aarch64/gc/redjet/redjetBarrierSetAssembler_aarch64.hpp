/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
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

#ifndef CPU_AARCH64_GC_REDJET_REDJETBARRIERSETASSEMBLER_AARCH64_HPP
#define CPU_AARCH64_GC_REDJET_REDJETBARRIERSETASSEMBLER_AARCH64_HPP

#include "asm/macroAssembler.hpp"
#include "gc/shared/barrierSetAssembler.hpp"

#ifdef COMPILER1
class LIR_Assembler;
class StubAssembler;
#endif

class RedjetBarrierSetAssembler: public BarrierSetAssembler {
public:
  RedjetBarrierSetAssembler() = default;

  void arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                          Register src, Register dst, Register count) override;
  void arraycopy_epilogue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                          Register src, Register dst, Register count) override;

  void load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
               Register dst, Address src, Register tmp1) override;
  void store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) override;

  void try_resolve_jobject_in_native(MacroAssembler* masm, Register jni_env,
                                     Register obj, Register tmp, Label& slowpath) override;

  // Redjet HSVM 4-Point Polling & 64-Byte Cacheline TLAB/Egg Zeroing
  void emit_pausepoint_poll(MacroAssembler* masm, Register tmp1);
  void emit_safepoint_poll(MacroAssembler* masm, Register tmp1);
  void clear_cacheline_64(MacroAssembler* masm, Register aligned64_addr);

#ifdef COMPILER1
  void generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm);
  void generate_c1_post_barrier_runtime_stub(StubAssembler* sasm);
#endif
};

#endif // CPU_AARCH64_GC_REDJET_REDJETBARRIERSETASSEMBLER_AARCH64_HPP
