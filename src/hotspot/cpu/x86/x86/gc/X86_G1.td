//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
//
// X86_G1.td — G1 Garbage Collector Barrier Instructions for x86_64
// ================================================================
// Converted from:
//   - src/hotspot/cpu/x86/gc/g1/g1_x86_64.ad
//
// This file defines G1 GC write barrier and CAS/atomic instructions
// in LLVM TableGen format for x86_64.

// ============================================================================
// G1 Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// G1 StoreP — Store a 64-bit object pointer with pre/post write barriers
// Converted from g1_x86_64.ad: instruct g1StoreP
class X86_G1StoreP : GCBarrier<"movq\t$mem, $src\t# ptr", "G1 GC StoreP with write barriers (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set i64mem:$mem, (store_p GR64:$src))];
}

def X86_G1StoreP_node : X86_G1StoreP;

// G1 StoreN — Store a 32-bit compressed object pointer with pre/post write barriers
// Converted from g1_x86_64.ad: instruct g1StoreN
class X86_G1StoreN : GCBarrier<"movl\t$mem, $src\t# narrow ptr", "G1 GC StoreN with write barriers (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set i32mem:$mem, (store_n GR32:$src))];
}

def X86_G1StoreN_node : X86_G1StoreN;

// G1 EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
// Converted from g1_x86_64.ad: instruct g1EncodePAndStoreN
class X86_G1EncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tmovl\t$mem, $src\t# ptr", "G1 GC EncodeP and StoreN (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set i32mem:$mem, (store_n (encode_p GR64:$src)))];
}

def X86_G1EncodePAndStoreN_node : X86_G1EncodePAndStoreN;

// ============================================================================
// G1 CompareAndExchange (CAE) Barriers
// ============================================================================

// G1 CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with barriers
// Converted from g1_x86_64.ad: instruct g1CompareAndExchangeP
class X86_G1CompareAndExchangeP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem", "G1 GC CompareAndExchangeP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$oldval, (g1_cae_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_G1CompareAndExchangeP_node : X86_G1CompareAndExchangeP;

// G1 CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
// Converted from g1_x86_64.ad: instruct g1CompareAndExchangeN
class X86_G1CompareAndExchangeN : GCBarrier<"lock\n\tcmpxchgl\t$newval, $mem", "G1 GC CompareAndExchangeN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$oldval, (g1_cae_n i32mem:$mem, GR32:$oldval, GR32:$newval))];
}

def X86_G1CompareAndExchangeN_node : X86_G1CompareAndExchangeN;

// ============================================================================
// G1 CompareAndSwap (CAS) Barriers
// ============================================================================

// G1 CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from g1_x86_64.ad: instruct g1CompareAndSwapP
class X86_G1CompareAndSwapP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\n\tsetcc\t$res", "G1 GC CompareAndSwapP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$res, (g1_cas_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_G1CompareAndSwapP_node : X86_G1CompareAndSwapP;
def X86_G1WeakCompareAndSwapP_node : X86_G1CompareAndSwapP;

// G1 CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
// Converted from g1_x86_64.ad: instruct g1CompareAndSwapN
class X86_G1CompareAndSwapN : GCBarrier<"lock\n\tcmpxchgl\t$newval, $mem\n\tsetcc\t$res", "G1 GC CompareAndSwapN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$res, (g1_cas_n i32mem:$mem, GR32:$oldval, GR32:$newval))];
}

def X86_G1CompareAndSwapN_node : X86_G1CompareAndSwapN;
def X86_G1WeakCompareAndSwapN_node : X86_G1CompareAndSwapN;

// ============================================================================
// G1 Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// G1 GetAndSetP — 64-bit pointer atomic exchange
// Converted from g1_x86_64.ad: instruct g1GetAndSetP
class X86_G1GetAndSetP : GCBarrier<"xchgq\t$res, $mem", "G1 GC GetAndSetP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$res, (g1_xchg_p i64mem:$mem, GR64:$newval))];
}

def X86_G1GetAndSetP_node : X86_G1GetAndSetP;

// G1 GetAndSetN — 32-bit compressed OOP atomic exchange
// Converted from g1_x86_64.ad: instruct g1GetAndSetN
class X86_G1GetAndSetN : GCBarrier<"xchgl\t$res, $mem", "G1 GC GetAndSetN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$res, (g1_xchg_n i32mem:$mem, GR32:$newval))];
}

def X86_G1GetAndSetN_node : X86_G1GetAndSetN;

// ============================================================================
// End of X86_G1.td
// ============================================================================
