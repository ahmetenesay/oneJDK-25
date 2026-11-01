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

#include "asm/macroAssembler.inline.hpp"
#include "gc/redjet/redjetBarrierSetAssembler_riscv.hpp"
#include "gc/redjet/rjnrml/redjetPointCoordinator.hpp"
#include "gc/redjet/rjnrml/redjetThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/gc_globals.hpp"

#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#endif

#define __ masm->

void RedjetBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                                   Register src, Register dst, Register count) {
  BarrierSetAssembler::arraycopy_prologue(masm, decorators, type, src, dst, count);
}

void RedjetBarrierSetAssembler::arraycopy_epilogue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                                   Register src, Register dst, Register count) {
  BarrierSetAssembler::arraycopy_epilogue(masm, decorators, type, src, dst, count);
}

void RedjetBarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                        Register dst, Address src, Register tmp1) {
  BarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1);
}

void RedjetBarrierSetAssembler::store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                         Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) {
  BarrierSetAssembler::store_at(masm, decorators, type, dst, val, tmp1, tmp2, tmp3);
}

void RedjetBarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm, Register jni_env,
                                                              Register obj, Register tmp, Label& slowpath) {
  BarrierSetAssembler::try_resolve_jobject_in_native(masm, jni_env, obj, tmp, slowpath);
}

void RedjetBarrierSetAssembler::emit_pausepoint_poll(MacroAssembler* masm, Register tmp1) {
  Register scratch = (tmp1 != noreg) ? tmp1 : t0;
  Label done;
  ExternalAddress gate_addr((address)RedjetPointCoordinator::pausepoint_gate_address());
  __ la(scratch, gate_addr);
  __ lwu(scratch, Address(scratch, 0));
  __ beqz(scratch, done);

  __ push_call_clobbered_registers();
  __ mv(c_rarg0, xthread);
  __ mv(c_rarg1, zr);
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_pausepoint_poll_slow), c_rarg0, c_rarg1);
  __ pop_call_clobbered_registers();

  __ bind(done);
}

void RedjetBarrierSetAssembler::emit_safepoint_poll(MacroAssembler* masm, Register tmp1) {
  Register scratch = (tmp1 != noreg) ? tmp1 : t0;
  Label done;
  ExternalAddress flag_addr((address)RedjetPointCoordinator::safepoint_flag_address());
  __ la(scratch, flag_addr);
  __ lwu(scratch, Address(scratch, 0));
  __ beqz(scratch, done);

  __ push_call_clobbered_registers();
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_safepoint_poll_slow), xthread);
  __ pop_call_clobbered_registers();

  __ bind(done);
}

void RedjetBarrierSetAssembler::clear_cacheline_64(MacroAssembler* masm, Register aligned64_addr) {
  assert(aligned64_addr != noreg, "valid base register required");
  for (int offset = 0; offset < 64; offset += 8) {
    __ sd(zr, Address(aligned64_addr, offset));
  }
}

#ifdef COMPILER1
#undef __
#define __ sasm->

void RedjetBarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("redjet_c1_pre_barrier", false);
  __ epilogue();
}

void RedjetBarrierSetAssembler::generate_c1_post_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("redjet_c1_post_barrier", false);
  __ epilogue();
}
#endif
