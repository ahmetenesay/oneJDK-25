//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// AArch64_G1.td — G1 Garbage Collector Barrier Instructions for AArch64 (ARM64)
// ==============================================================================
// Converted from:
//   - src/hotspot/cpu/aarch64/gc/g1/g1_aarch64.ad
//
// This file defines G1 GC write barrier, CAS, and atomic instructions
// in LLVM TableGen format for AArch64.

// ============================================================================
// G1 Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// G1 StoreP — Store a 64-bit object pointer with pre/post write barriers
// Converted from g1_aarch64.ad: instruct g1StoreP
class AArch64_G1StoreP : GCBarrier<"str\t$src, [$mem]\t# ptr", "G1 GC StoreP with write barriers (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR64sp:$mem, (store_p GPR64:$src))];
}

def AArch64_G1StoreP_node : AArch64_G1StoreP;
def AArch64_G1StorePVolatile_node : AArch64_G1StoreP;

// G1 StoreN — Store a 32-bit compressed object pointer with pre/post write barriers
// Converted from g1_aarch64.ad: instruct g1StoreN
class AArch64_G1StoreN : GCBarrier<"str\t$src, [$mem]\t# narrow ptr", "G1 GC StoreN with write barriers (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR64sp:$mem, (store_n GPR32:$src))];
}

def AArch64_G1StoreN_node : AArch64_G1StoreN;
def AArch64_G1StoreNVolatile_node : AArch64_G1StoreN;

// G1 EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
// Converted from g1_aarch64.ad: instruct g1EncodePAndStoreN
class AArch64_G1EncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tstr\t$src, [$mem]\t# ptr", "G1 GC EncodeP and StoreN (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR64sp:$mem, (store_n (encode_p GPR64:$src)))];
}

def AArch64_G1EncodePAndStoreN_node : AArch64_G1EncodePAndStoreN;
def AArch64_G1EncodePAndStoreNVolatile_node : AArch64_G1EncodePAndStoreN;

// ============================================================================
// G1 CompareAndExchange (CAE) Barriers
// ============================================================================

// G1 CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with barriers
// Converted from g1_aarch64.ad: instruct g1CompareAndExchangeP
class AArch64_G1CompareAndExchangeP : GCBarrier<"casal\t$oldval, $newval, [$mem]", "G1 GC CompareAndExchangeP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR64:$oldval, (g1_cae_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_G1CompareAndExchangeP_node : AArch64_G1CompareAndExchangeP;
def AArch64_G1CompareAndExchangePAcq_node : AArch64_G1CompareAndExchangeP;

// G1 CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
// Converted from g1_aarch64.ad: instruct g1CompareAndExchangeN
class AArch64_G1CompareAndExchangeN : GCBarrier<"casal\t$oldval, $newval, [$mem]", "G1 GC CompareAndExchangeN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR32:$oldval, (g1_cae_n GPR64sp:$mem, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_G1CompareAndExchangeN_node : AArch64_G1CompareAndExchangeN;
def AArch64_G1CompareAndExchangeNAcq_node : AArch64_G1CompareAndExchangeN;

// ============================================================================
// G1 CompareAndSwap (CAS) Barriers
// ============================================================================

// G1 CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from g1_aarch64.ad: instruct g1CompareAndSwapP
class AArch64_G1CompareAndSwapP : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq", "G1 GC CompareAndSwapP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR32:$res, (g1_cas_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_G1CompareAndSwapP_node : AArch64_G1CompareAndSwapP;
def AArch64_G1CompareAndSwapPAcq_node : AArch64_G1CompareAndSwapP;

// G1 CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
// Converted from g1_aarch64.ad: instruct g1CompareAndSwapN
class AArch64_G1CompareAndSwapN : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq", "G1 GC CompareAndSwapN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR32:$res, (g1_cas_n GPR64sp:$mem, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_G1CompareAndSwapN_node : AArch64_G1CompareAndSwapN;
def AArch64_G1CompareAndSwapNAcq_node : AArch64_G1CompareAndSwapN;

// ============================================================================
// G1 Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// G1 GetAndSetP — 64-bit pointer atomic exchange
// Converted from g1_aarch64.ad: instruct g1GetAndSetP
class AArch64_G1GetAndSetP : GCBarrier<"swpal\t$newval, $res, [$mem]", "G1 GC GetAndSetP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR64:$res, (g1_xchg_p GPR64sp:$mem, GPR64:$newval))];
}

def AArch64_G1GetAndSetP_node : AArch64_G1GetAndSetP;
def AArch64_G1GetAndSetPAcq_node : AArch64_G1GetAndSetP;

// G1 GetAndSetN — 32-bit compressed OOP atomic exchange
// Converted from g1_aarch64.ad: instruct g1GetAndSetN
class AArch64_G1GetAndSetN : GCBarrier<"swpal\t$newval, $res, [$mem]", "G1 GC GetAndSetN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR32:$res, (g1_xchg_n GPR64sp:$mem, GPR32:$newval))];
}

def AArch64_G1GetAndSetN_node : AArch64_G1GetAndSetN;
def AArch64_G1GetAndSetNAcq_node : AArch64_G1GetAndSetN;

// ============================================================================
// End of AArch64_G1.td
// ============================================================================
