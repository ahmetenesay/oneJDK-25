//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// X86_Z.td — Z Garbage Collector (ZGC) Barrier Instructions for x86_64
// ====================================================================
// Converted from:
//   - src/hotspot/cpu/x86/gc/z/z_x86_64.ad
//
// This file defines ZGC load, store, CAS, and atomic instructions
// in LLVM TableGen format for x86_64.

// ============================================================================
// ZGC Load Barriers
// ============================================================================

// ZLoadP — Load 64-bit object pointer with ZGC load barrier
// Converted from z_x86_64.ad: instruct zLoadP
class X86_ZLoadP : GCBarrier<"movq\t$dst, $mem\t# zload", "ZGC Load Barrier (x86_64)", 125, "ialu_reg_mem"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GR64:$dst, (z_load_p i64mem:$mem))];
}

def X86_ZLoadP_node : X86_ZLoadP;

// ZLoadPNullCheck — Load pointer and test against ZPointerAddressMask
// Converted from z_x86_64.ad: instruct zLoadPNullCheck
class X86_ZLoadPNullCheck : GCBarrier<"testq\t$op, 0xffffffffffff0000\t# znullcheck", "ZGC Load Null Check (x86_64)", 500, "ialu_cr_reg_imm"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set CCR:$cr, (z_null_check_p i64mem:$op))];
}

def X86_ZLoadPNullCheck_node : X86_ZLoadPNullCheck;

// ============================================================================
// ZGC Store Barriers
// ============================================================================

// ZStoreP — Store 64-bit object pointer with ZGC store barrier
// Converted from z_x86_64.ad: instruct zStoreP
class X86_ZStoreP : GCBarrier<"movq\t$mem, $src\t# zstore", "ZGC Store Barrier (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set i64mem:$mem, (z_store_p GR64:$src))];
}

def X86_ZStoreP_node : X86_ZStoreP;

// ZStorePNull — Store colored null with ZGC store barrier
// Converted from z_x86_64.ad: instruct zStorePNull
class X86_ZStorePNull : GCBarrier<"movq\t$mem, 0\t# zstore null", "ZGC Store Null Barrier (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set i64mem:$mem, (z_store_null_p))];
}

def X86_ZStorePNull_node : X86_ZStorePNull;

// ============================================================================
// ZGC CompareAndExchange / CompareAndSwap Barriers
// ============================================================================

// ZCompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with ZGC barrier
// Converted from z_x86_64.ad: instruct zCompareAndExchangeP
class X86_ZCompareAndExchangeP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\t# zcae", "ZGC CompareAndExchangeP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GR64:$oldval, (z_cae_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_ZCompareAndExchangeP_node : X86_ZCompareAndExchangeP;

// ZCompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from z_x86_64.ad: instruct zCompareAndSwapP
class X86_ZCompareAndSwapP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\n\tsetcc\t$res\t# zcas", "ZGC CompareAndSwapP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GR32:$res, (z_cas_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_ZCompareAndSwapP_node : X86_ZCompareAndSwapP;
def X86_ZWeakCompareAndSwapP_node : X86_ZCompareAndSwapP;

// ============================================================================
// ZGC Atomic Exchange (XChg) Barrier
// ============================================================================

// ZXChgP — 64-bit pointer atomic exchange with ZGC uncoloring
// Converted from z_x86_64.ad: instruct zXChgP
class X86_ZXChgP : GCBarrier<"xchgq\t$newval, $mem\t# zxchg", "ZGC Atomic Exchange (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GR64:$newval, (z_xchg_p i64mem:$mem, GR64:$newval))];
}

def X86_ZXChgP_node : X86_ZXChgP;

// ============================================================================
// End of X86_Z.td
// ============================================================================
