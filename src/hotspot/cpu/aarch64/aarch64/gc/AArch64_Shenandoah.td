//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// AArch64_Shenandoah.td — Shenandoah GC Barrier Instructions for AArch64 (ARM64)
// ==============================================================================
// Converted from:
//   - src/hotspot/cpu/aarch64/gc/shenandoah/shenandoah_aarch64.ad
//
// This file defines Shenandoah GC CAS and CompareAndExchange instructions
// in LLVM TableGen format for AArch64.

// ============================================================================
// Shenandoah CompareAndSwap (CAS) Barriers
// ============================================================================

// Shenandoah CompareAndSwapP — 64-bit pointer CAS with Shenandoah barriers
// Converted from shenandoah_aarch64.ad: instruct compareAndSwapP_shenandoah
class AArch64_ShenandoahCAS_P : GCBarrier<"shenandoah_cas_oop\t[$mem_ptr], $newval", "Shenandoah CompareAndSwapP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR32:$res, (shenandoah_cas_p GPR64sp:$mem_ptr, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_ShenandoahCAS_P_node : AArch64_ShenandoahCAS_P;
def AArch64_ShenandoahCAS_PAcq_node : AArch64_ShenandoahCAS_P;
def AArch64_ShenandoahWeakCAS_P_node : AArch64_ShenandoahCAS_P;
def AArch64_ShenandoahWeakCAS_PAcq_node : AArch64_ShenandoahCAS_P;

// Shenandoah CompareAndSwapN — 32-bit compressed OOP CAS with Shenandoah barriers
// Converted from shenandoah_aarch64.ad: instruct compareAndSwapN_shenandoah
class AArch64_ShenandoahCAS_N : GCBarrier<"shenandoah_cas_oop\t[$mem_ptr], $newval", "Shenandoah CompareAndSwapN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR32:$res, (shenandoah_cas_n GPR64sp:$mem_ptr, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_ShenandoahCAS_N_node : AArch64_ShenandoahCAS_N;
def AArch64_ShenandoahCAS_NAcq_node : AArch64_ShenandoahCAS_N;
def AArch64_ShenandoahWeakCAS_N_node : AArch64_ShenandoahCAS_N;
def AArch64_ShenandoahWeakCAS_NAcq_node : AArch64_ShenandoahCAS_N;

// ============================================================================
// Shenandoah CompareAndExchange (CAE) Barriers
// ============================================================================

// Shenandoah CompareAndExchangeP — 64-bit pointer CompareAndExchange with Shenandoah barriers
// Converted from shenandoah_aarch64.ad: instruct compareAndExchangeP_shenandoah
class AArch64_ShenandoahCAE_P : GCBarrier<"shenandoah_cas_oop\t[$mem_ptr], $newval", "Shenandoah CompareAndExchangeP (AArch64)", 1000, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR64:$oldval, (shenandoah_cae_p GPR64sp:$mem_ptr, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_ShenandoahCAE_P_node : AArch64_ShenandoahCAE_P;
def AArch64_ShenandoahCAE_PAcq_node : AArch64_ShenandoahCAE_P;

// Shenandoah CompareAndExchangeN — 32-bit compressed OOP CompareAndExchange with Shenandoah barriers
// Converted from shenandoah_aarch64.ad: instruct compareAndExchangeN_shenandoah
class AArch64_ShenandoahCAE_N : GCBarrier<"shenandoah_cas_oop\t[$mem_ptr], $newval", "Shenandoah CompareAndExchangeN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GPR32:$oldval, (shenandoah_cae_n GPR64sp:$mem_ptr, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_ShenandoahCAE_N_node : AArch64_ShenandoahCAE_N;
def AArch64_ShenandoahCAE_NAcq_node : AArch64_ShenandoahCAE_N;

// ============================================================================
// End of AArch64_Shenandoah.td
// ============================================================================
