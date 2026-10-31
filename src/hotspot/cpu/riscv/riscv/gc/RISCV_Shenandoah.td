//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// RISCV_Shenandoah.td — Shenandoah GC Barrier Instructions for RISC-V
// ===================================================================
// Converted from:
//   - src/hotspot/cpu/riscv/gc/shenandoah/shenandoah_riscv.ad
//
// This file defines Shenandoah GC CAS and CompareAndExchange instructions
// in LLVM TableGen format for RISC-V (RV64).

// ============================================================================
// Shenandoah CompareAndSwap (CAS) Barriers
// ============================================================================

// Shenandoah CompareAndSwapP — 64-bit pointer CAS with Shenandoah barriers
// Converted from shenandoah_riscv.ad: instruct compareAndSwapP_shenandoah
class RISCV_ShenandoahCAS_P : GCBarrier<"shenandoah_cas_oop\t0($mem_ptr), $newval", "Shenandoah CompareAndSwapP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR:$res, (shenandoah_cas_p GPR:$mem_ptr, GPR:$oldval, GPR:$newval))];
}

def RISCV_ShenandoahCAS_P_node : RISCV_ShenandoahCAS_P;
def RISCV_ShenandoahCAS_PAcq_node : RISCV_ShenandoahCAS_P;
def RISCV_ShenandoahWeakCAS_P_node : RISCV_ShenandoahCAS_P;
def RISCV_ShenandoahWeakCAS_PAcq_node : RISCV_ShenandoahCAS_P;

// Shenandoah CompareAndSwapN — 32-bit compressed OOP CAS with Shenandoah barriers
// Converted from shenandoah_riscv.ad: instruct compareAndSwapN_shenandoah
class RISCV_ShenandoahCAS_N : GCBarrier<"shenandoah_cas_oop\t0($mem_ptr), $newval", "Shenandoah CompareAndSwapN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR:$res, (shenandoah_cas_n GPR:$mem_ptr, GPR:$oldval, GPR:$newval))];
}

def RISCV_ShenandoahCAS_N_node : RISCV_ShenandoahCAS_N;
def RISCV_ShenandoahCAS_NAcq_node : RISCV_ShenandoahCAS_N;
def RISCV_ShenandoahWeakCAS_N_node : RISCV_ShenandoahCAS_N;
def RISCV_ShenandoahWeakCAS_NAcq_node : RISCV_ShenandoahCAS_N;

// ============================================================================
// Shenandoah CompareAndExchange (CAE) Barriers
// ============================================================================

// Shenandoah CompareAndExchangeP — 64-bit pointer CompareAndExchange
// Converted from shenandoah_riscv.ad: instruct compareAndExchangeP_shenandoah
class RISCV_ShenandoahCAE_P : GCBarrier<"shenandoah_cas_oop\t0($mem_ptr), $newval", "Shenandoah CompareAndExchangeP (RISC-V)", 1000, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR:$oldval, (shenandoah_cae_p GPR:$mem_ptr, GPR:$oldval, GPR:$newval))];
}

def RISCV_ShenandoahCAE_P_node : RISCV_ShenandoahCAE_P;
def RISCV_ShenandoahCAE_PAcq_node : RISCV_ShenandoahCAE_P;

// Shenandoah CompareAndExchangeN — 32-bit compressed OOP CompareAndExchange
// Converted from shenandoah_riscv.ad: instruct compareAndExchangeN_shenandoah
class RISCV_ShenandoahCAE_N : GCBarrier<"shenandoah_cas_oop\t0($mem_ptr), $newval", "Shenandoah CompareAndExchangeN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR:$oldval, (shenandoah_cae_n GPR:$mem_ptr, GPR:$oldval, GPR:$newval))];
}

def RISCV_ShenandoahCAE_N_node : RISCV_ShenandoahCAE_N;
def RISCV_ShenandoahCAE_NAcq_node : RISCV_ShenandoahCAE_N;

// ============================================================================
// End of RISCV_Shenandoah.td
// ============================================================================
