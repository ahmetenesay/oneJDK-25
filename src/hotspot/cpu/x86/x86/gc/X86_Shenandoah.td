//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
//
// X86_Shenandoah.td — Shenandoah GC Barrier Instructions for x86_64
// =================================================================
// Converted from:
//   - src/hotspot/cpu/x86/gc/shenandoah/shenandoah_x86_64.ad
//
// This file defines Shenandoah GC CAS and CompareAndExchange instructions
// in LLVM TableGen format for x86_64.

// ============================================================================
// Shenandoah CompareAndSwap (CAS) Barriers
// ============================================================================

// Shenandoah CompareAndSwapP — 64-bit pointer CAS with Shenandoah barriers
// Converted from shenandoah_x86_64.ad: instruct compareAndSwapP_shenandoah
class X86_ShenandoahCAS_P : GCBarrier<"shenandoah_cas_oop\t$mem_ptr, $newval", "Shenandoah CompareAndSwapP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GR32:$res, (shenandoah_cas_p i64mem:$mem_ptr, GR64:$oldval, GR64:$newval))];
}

def X86_ShenandoahCAS_P_node : X86_ShenandoahCAS_P;
def X86_ShenandoahWeakCAS_P_node : X86_ShenandoahCAS_P;

// Shenandoah CompareAndSwapN — 32-bit compressed OOP CAS with Shenandoah barriers
// Converted from shenandoah_x86_64.ad: instruct compareAndSwapN_shenandoah
class X86_ShenandoahCAS_N : GCBarrier<"shenandoah_cas_oop\t$mem_ptr, $newval", "Shenandoah CompareAndSwapN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GR32:$res, (shenandoah_cas_n i32mem:$mem_ptr, GR32:$oldval, GR32:$newval))];
}

def X86_ShenandoahCAS_N_node : X86_ShenandoahCAS_N;
def X86_ShenandoahWeakCAS_N_node : X86_ShenandoahCAS_N;

// ============================================================================
// Shenandoah CompareAndExchange (CAE) Barriers
// ============================================================================

// Shenandoah CompareAndExchangeP — 64-bit pointer CompareAndExchange with Shenandoah barriers
// Converted from shenandoah_x86_64.ad: instruct compareAndExchangeP_shenandoah
class X86_ShenandoahCAE_P : GCBarrier<"shenandoah_cas_oop\t$mem_ptr, $newval", "Shenandoah CompareAndExchangeP (x86_64)", 1000, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GR64:$oldval, (shenandoah_cae_p i64mem:$mem_ptr, GR64:$oldval, GR64:$newval))];
}

def X86_ShenandoahCAE_P_node : X86_ShenandoahCAE_P;

// Shenandoah CompareAndExchangeN — 32-bit compressed OOP CompareAndExchange with Shenandoah barriers
// Converted from shenandoah_x86_64.ad: instruct compareAndExchangeN_shenandoah
class X86_ShenandoahCAE_N : GCBarrier<"shenandoah_cas_oop\t$mem_ptr, $newval", "Shenandoah CompareAndExchangeN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GR32:$oldval, (shenandoah_cae_n i32mem:$mem_ptr, GR32:$oldval, GR32:$newval))];
}

def X86_ShenandoahCAE_N_node : X86_ShenandoahCAE_N;

// ============================================================================
// End of X86_Shenandoah.td
// ============================================================================
