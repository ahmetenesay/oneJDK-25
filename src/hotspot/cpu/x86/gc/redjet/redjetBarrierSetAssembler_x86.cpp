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
#include "gc/redjet/redjetBarrierSetAssembler_x86.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/gc_globals.hpp"
#include "oops/markWord.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/vm_version.hpp"

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

void RedjetBarrierSetAssembler::cmpxchg_oop(MacroAssembler* masm,
                                            Register res, Address addr, Register oldval, Register newval,
                                            bool exchange, Register tmp1, Register tmp2) {
  assert_different_registers(addr.base(), oldval, newval, res);
  assert_different_registers(addr.index(), oldval, newval, res);

  if (oldval != rax) {
    __ movptr(rax, oldval);
  }

  if (UseCompressedOops) {
    __ lock();
    __ cmpxchgl(newval, addr);
  } else {
    __ lock();
    __ cmpxchgq(newval, addr);
  }

  if (exchange) {
    if (res != rax) {
      __ movptr(res, rax);
    }
  } else {
    __ setcc(Assembler::equal, res);
  }
}

void RedjetBarrierSetAssembler::emit_pausepoint_poll(MacroAssembler* masm, Register tmp1) {
  assert(tmp1 != noreg, "pausepoint poll requires scratch register");
  Label done;
  ExternalAddress gate_addr((address)RedjetPointCoordinator::pausepoint_gate_address());
  __ lea(tmp1, gate_addr);
  __ movl(tmp1, Address(tmp1, 0));
  __ testl(tmp1, tmp1);
  __ jcc(Assembler::zero, done);

  __ push_call_clobbered_registers(true /* save_fpu */);
  __ xorptr(c_rarg1, c_rarg1);
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_pausepoint_poll_slow), r15_thread, c_rarg1);
  __ pop_call_clobbered_registers(true /* restore_fpu */);

  __ bind(done);
}

void RedjetBarrierSetAssembler::emit_safepoint_poll(MacroAssembler* masm, Register tmp1) {
  assert(tmp1 != noreg, "safepoint poll requires scratch register");
  Label done;
  ExternalAddress flag_addr((address)RedjetPointCoordinator::safepoint_flag_address());
  __ lea(tmp1, flag_addr);
  __ movl(tmp1, Address(tmp1, 0));
  __ testl(tmp1, tmp1);
  __ jcc(Assembler::zero, done);

  __ push_call_clobbered_registers(true /* save_fpu */);
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_safepoint_poll_slow), r15_thread);
  __ pop_call_clobbered_registers(true /* restore_fpu */);

  __ bind(done);
}

void RedjetBarrierSetAssembler::clear_cacheline_64(MacroAssembler* masm, Register aligned64_addr) {
  assert(aligned64_addr != noreg, "valid base register required");
  if (VM_Version::supports_avx10_1() || (VM_Version::supports_evex() && UseAVX >= 3)) {
    __ evpxorq(xmm0, xmm0, xmm0, Assembler::AVX_512bit);
    __ evmovdquq(Address(aligned64_addr, 0), xmm0, Assembler::AVX_512bit);
  } else if (UseAVX >= 2) {
    __ vpxor(xmm0, xmm0, xmm0, Assembler::AVX_256bit);
    __ vmovdqu(Address(aligned64_addr, 0), xmm0, Assembler::AVX_256bit);
    __ vmovdqu(Address(aligned64_addr, 32), xmm0, Assembler::AVX_256bit);
  } else {
    for (int offset = 0; offset < 64; offset += 8) {
      __ movq(Address(aligned64_addr, offset), (int32_t)0);
    }
  }
}

void RedjetBarrierSetAssembler::emit_2points_l1_poll(MacroAssembler* masm, Register thread, Register scratch, Label& slowpath) {
  assert(scratch != noreg, "scratch register required for 2-Points L1 polling");
  // 2-Points L1 Cooperative Polling: 0.5-cycle L1 cache line hit
  ExternalAddress flag_addr((address)RedjetPointCoordinator::parkpoint_flag_address());
  __ lea(scratch, flag_addr);
  __ cmpl(Address(scratch, 0), 0);
  __ jcc(Assembler::notZero, slowpath);
}

static const int32_t HS_EMPTYBOX_MAGIC = (int32_t)0xEB0C5107;

void RedjetBarrierSetAssembler::emit_tailtraces_clasp(MacroAssembler* masm, Register obj, Register scratch) {
  if (obj == noreg) return;
  // TailTraces clasping: dead-end liveness seal to Falline quarantine lobby (0xEB0C5107)
  __ movl(Address(obj, oopDesc::mark_offset_in_bytes()), HS_EMPTYBOX_MAGIC);
}

void RedjetBarrierSetAssembler::emit_emptybox_guard(MacroAssembler* masm, Register obj, Label& slowpath) {
  if (obj == noreg) return;
  // Check EmptyBox pretrashing canary guard bit matching TableGen specification
  __ cmpl(Address(obj, oopDesc::mark_offset_in_bytes()), HS_EMPTYBOX_MAGIC);
  __ jcc(Assembler::equal, slowpath);
}

void RedjetBarrierSetAssembler::emit_emptybox_claim(MacroAssembler* masm, Register obj, Register size, Register result, Label& slowpath) {
  assert_different_registers(obj, size, result);
  // 0.3ns branchless claim from EmptyBox cache
  if (result != obj) {
    __ movptr(result, obj);
  }
  // Re-initialize mark word to default prototype
  __ movptr(Address(result, oopDesc::mark_offset_in_bytes()), (intptr_t)markWord::prototype().value());
}

void RedjetBarrierSetAssembler::emit_simd_dispatch_with_vzeroupper(MacroAssembler* masm) {
  // AVX10 removes the penalty for transition between vector lengths, obviating vzeroupper
  if (!VM_Version::supports_avx10_1() && VM_Version::supports_vzeroupper()) {
    __ vzeroupper();
  }
}

#ifdef COMPILER1
#undef __
#define __ sasm->

void RedjetBarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("redjet_c1_pre_barrier", false);
  __ push(rax);
  __ push(rdx);
  const Register thread = r15_thread;
  Address flags(thread, in_bytes(RedjetThreadLocalData::point_flags_offset()));
  Label done;
  __ testl(flags, REDJET_PT_PARKPOINT_ACTIVE);
  __ jcc(Assembler::zero, done);
  __ save_live_registers_no_oop_map(true);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_safepoint_poll_slow), thread);
  __ restore_live_registers(true);
  __ bind(done);
  __ pop(rdx);
  __ pop(rax);
  __ epilogue();
}

void RedjetBarrierSetAssembler::generate_c1_post_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("redjet_c1_post_barrier", false);
  __ push(rax);
  __ push(rdx);
  const Register thread = r15_thread;
  Address flags(thread, in_bytes(RedjetThreadLocalData::point_flags_offset()));
  Label done;
  __ testl(flags, REDJET_PT_DOCKPOINT_ACTIVE);
  __ jcc(Assembler::zero, done);
  __ save_live_registers_no_oop_map(true);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, redjet_rt_safepoint_poll_slow), thread);
  __ restore_live_registers(true);
  __ bind(done);
  __ pop(rdx);
  __ pop(rax);
  __ epilogue();
}
#endif
