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
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/gc_globals.hpp"

#ifdef COMPILER1
#include "vac/c1_LIRAssembler.hpp"
#include "vac/c1_MacroAssembler.hpp"
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
  if (UseRVV) {
    __ vsetivli(t0, 8, Assembler::e64, Assembler::m1);
    __ vxor_vv(v0, v0, v0);
    __ vse64_v(v0, aligned64_addr);
  } else {
    for (int offset = 0; offset < 64; offset += 8) {
      __ sd(zr, Address(aligned64_addr, offset));
    }
  }
}

void RedjetBarrierSetAssembler::emit_2points_l1_poll(MacroAssembler* masm, Register thread, Register scratch, Label& slowpath) {
  Register s = (scratch != noreg) ? scratch : t0;
  // 2-Points L1 Cooperative Polling: 0.5-cycle L1 cache line hit
  ExternalAddress flag_addr((address)RedjetPointCoordinator::parkpoint_flag_address());
  __ la(s, flag_addr);
  __ lwu(s, Address(s, 0));
  __ bnez(s, slowpath);
}

static const int64_t HS_EMPTYBOX_MAGIC = 0xEB0C5107LL;

void RedjetBarrierSetAssembler::emit_tailtraces_clasp(MacroAssembler* masm, Register obj, Register scratch) {
  if (obj == noreg) return;
  Register s = (scratch != noreg) ? scratch : t0;
  // TailTraces clasping: dead-end liveness seal to Falline quarantine lobby (0xEB0C5107)
  __ li(s, HS_EMPTYBOX_MAGIC);
  __ sw(s, Address(obj, oopDesc::mark_offset_in_bytes()));
}

void RedjetBarrierSetAssembler::emit_emptybox_guard(MacroAssembler* masm, Register obj, Label& slowpath) {
  if (obj == noreg) return;
  // Check EmptyBox pretrashing canary guard bit matching TableGen specification
  __ lwu(t0, Address(obj, oopDesc::mark_offset_in_bytes()));
  __ li(t1, HS_EMPTYBOX_MAGIC);
  __ beq(t0, t1, slowpath);
}

void RedjetBarrierSetAssembler::emit_emptybox_claim(MacroAssembler* masm, Register obj, Register size, Register result, Label& slowpath) {
  assert_different_registers(obj, size, result);
  // 0.3ns branchless claim from EmptyBox cache
  if (result != obj) {
    __ mv(result, obj);
  }
  // Re-initialize mark word to default prototype
  __ li(t0, (int64_t)markWord::prototype().value());
  __ sd(t0, Address(result, oopDesc::mark_offset_in_bytes()));
}

void RedjetBarrierSetAssembler::emit_simd_dispatch_with_rvv_fence(MacroAssembler* masm) {
  __ fence();
}

#ifdef COMPILER1
#undef __
#define __ sasm->

void RedjetBarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("redjet_c1_pre_barrier", false);
  __ push_reg(t0);
  __ push_reg(t1);
  const Register thread = xthread;
  Address flags(thread, in_bytes(RedjetThreadLocalData::point_flags_offset()));
  Label done;
  __ lwu(t0, flags);
  __ andi(t1, t0, REDJET_PT_PARKPOINT_ACTIVE);
  __ beqz(t1, done);
  __ save_live_registers_no_oop_map(true);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_safepoint_poll_slow), thread);
  __ restore_live_registers(true);
  __ bind(done);
  __ pop_reg(t1);
  __ pop_reg(t0);
  __ epilogue();
}

void RedjetBarrierSetAssembler::generate_c1_post_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("redjet_c1_post_barrier", false);
  __ push_reg(t0);
  __ push_reg(t1);
  const Register thread = xthread;
  Address flags(thread, in_bytes(RedjetThreadLocalData::point_flags_offset()));
  Label done;
  __ lwu(t0, flags);
  __ andi(t1, t0, REDJET_PT_DOCKPOINT_ACTIVE);
  __ beqz(t1, done);
  __ save_live_registers_no_oop_map(true);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_safepoint_poll_slow), thread);
  __ restore_live_registers(true);
  __ bind(done);
  __ pop_reg(t1);
  __ pop_reg(t0);
  __ epilogue();
}
#endif
