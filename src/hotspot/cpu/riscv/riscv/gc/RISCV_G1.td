//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// RISCV_G1.td — G1 Garbage Collector Barrier Instructions for RISC-V
// ==================================================================
// Converted from:
//   - src/hotspot/cpu/riscv/gc/g1/g1_riscv.ad
//
// This file defines G1 GC write barrier, CAS, and atomic instructions
// in LLVM TableGen format for RISC-V (RV64).

// ============================================================================
// G1 Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// G1 StoreP — Store a 64-bit object pointer with pre/post write barriers
// Converted from g1_riscv.ad: instruct g1StoreP
class RISCV_G1StoreP : GCBarrier<"sd\t$src, 0($mem)\t# ptr", "G1 GC StoreP with write barriers (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$mem, (store_p GPR:$src))];
}

def RISCV_G1StoreP_node : RISCV_G1StoreP;

// G1 StoreN — Store a 32-bit compressed object pointer with pre/post write barriers
// Converted from g1_riscv.ad: instruct g1StoreN
class RISCV_G1StoreN : GCBarrier<"sw\t$src, 0($mem)\t# narrow ptr", "G1 GC StoreN with write barriers (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$mem, (store_n GPR:$src))];
}

def RISCV_G1StoreN_node : RISCV_G1StoreN;

// G1 EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
// Converted from g1_riscv.ad: instruct g1EncodePAndStoreN
class RISCV_G1EncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tsw\t$src, 0($mem)\t# ptr", "G1 GC EncodeP and StoreN (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$mem, (store_n (encode_p GPR:$src)))];
}

def RISCV_G1EncodePAndStoreN_node : RISCV_G1EncodePAndStoreN;

// ============================================================================
// G1 CompareAndExchange (CAE) Barriers
// ============================================================================

// G1 CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange
// Converted from g1_riscv.ad: instruct g1CompareAndExchangeP
class RISCV_G1CompareAndExchangeP : GCBarrier<"amoswap.d\t$res, $newval, ($mem)", "G1 GC CompareAndExchangeP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$oldval, (g1_cae_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_G1CompareAndExchangeP_node : RISCV_G1CompareAndExchangeP;
def RISCV_G1CompareAndExchangePAcq_node : RISCV_G1CompareAndExchangeP;

// G1 CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
// Converted from g1_riscv.ad: instruct g1CompareAndExchangeN
class RISCV_G1CompareAndExchangeN : GCBarrier<"amoswap.w\t$res, $newval, ($mem)", "G1 GC CompareAndExchangeN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$oldval, (g1_cae_n GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_G1CompareAndExchangeN_node : RISCV_G1CompareAndExchangeN;
def RISCV_G1CompareAndExchangeNAcq_node : RISCV_G1CompareAndExchangeN;

// ============================================================================
// G1 CompareAndSwap (CAS) Barriers
// ============================================================================

// G1 CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from g1_riscv.ad: instruct g1CompareAndSwapP
class RISCV_G1CompareAndSwapP : GCBarrier<"lr.d/sc.d\t$res, $newval, ($mem)", "G1 GC CompareAndSwapP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$res, (g1_cas_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_G1CompareAndSwapP_node : RISCV_G1CompareAndSwapP;
def RISCV_G1CompareAndSwapPAcq_node : RISCV_G1CompareAndSwapP;

// G1 CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
// Converted from g1_riscv.ad: instruct g1CompareAndSwapN
class RISCV_G1CompareAndSwapN : GCBarrier<"lr.w/sc.w\t$res, $newval, ($mem)", "G1 GC CompareAndSwapN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$res, (g1_cas_n GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_G1CompareAndSwapN_node : RISCV_G1CompareAndSwapN;
def RISCV_G1CompareAndSwapNAcq_node : RISCV_G1CompareAndSwapN;

// ============================================================================
// G1 Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// G1 GetAndSetP — 64-bit pointer atomic exchange
// Converted from g1_riscv.ad: instruct g1GetAndSetP
class RISCV_G1GetAndSetP : GCBarrier<"amoswap.d\t$res, $newval, ($mem)", "G1 GC GetAndSetP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$res, (g1_xchg_p GPR:$mem, GPR:$newval))];
}

def RISCV_G1GetAndSetP_node : RISCV_G1GetAndSetP;
def RISCV_G1GetAndSetPAcq_node : RISCV_G1GetAndSetP;

// G1 GetAndSetN — 32-bit compressed OOP atomic exchange
// Converted from g1_riscv.ad: instruct g1GetAndSetN
class RISCV_G1GetAndSetN : GCBarrier<"amoswap.w\t$res, $newval, ($mem)", "G1 GC GetAndSetN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GPR:$res, (g1_xchg_n GPR:$mem, GPR:$newval))];
}

def RISCV_G1GetAndSetN_node : RISCV_G1GetAndSetN;
def RISCV_G1GetAndSetNAcq_node : RISCV_G1GetAndSetN;

// ============================================================================
// End of RISCV_G1.td
// ============================================================================
