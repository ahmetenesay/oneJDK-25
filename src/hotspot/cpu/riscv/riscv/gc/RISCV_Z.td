//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// RISCV_Z.td — Z Garbage Collector (ZGC) Barrier Instructions for RISC-V
// ======================================================================
// Converted from:
//   - src/hotspot/cpu/riscv/gc/z/z_riscv.ad
//
// This file defines ZGC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for RISC-V (RV64).

// ============================================================================
// ZGC Load Barriers
// ============================================================================

// ZLoadP — Load 64-bit object pointer with ZGC load barrier
// Converted from z_riscv.ad: instruct zLoadP
class RISCV_ZLoadP : GCBarrier<"z_load_barrier\t$dst, 0($mem)", "ZGC Load Barrier (RISC-V)", 125, "ialu_reg_mem"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR:$dst, (z_load_p GPR:$mem))];
}

def RISCV_ZLoadP_node : RISCV_ZLoadP;

// ============================================================================
// ZGC Store Barriers
// ============================================================================

// ZStoreP — Store 64-bit object pointer with ZGC store barrier
// Converted from z_riscv.ad: instruct zStoreP
class RISCV_ZStoreP : GCBarrier<"z_store_barrier\t$src, 0($mem)", "ZGC Store Barrier (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR:$mem, (z_store_p GPR:$src))];
}

def RISCV_ZStoreP_node : RISCV_ZStoreP;

// ============================================================================
// ZGC CompareAndExchange / CompareAndSwap Barriers
// ============================================================================

// ZCompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with ZGC barrier
// Converted from z_riscv.ad: instruct zCompareAndExchangeP
class RISCV_ZCompareAndExchangeP : GCBarrier<"amoswap.d\t$oldval, $newval, ($mem)\t# zcae", "ZGC CompareAndExchangeP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR:$oldval, (z_cae_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_ZCompareAndExchangeP_node : RISCV_ZCompareAndExchangeP;
def RISCV_ZCompareAndExchangePAcq_node : RISCV_ZCompareAndExchangeP;

// ZCompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from z_riscv.ad: instruct zCompareAndSwapP
class RISCV_ZCompareAndSwapP : GCBarrier<"lr.d/sc.d\t$res, $newval, ($mem)\t# zcas", "ZGC CompareAndSwapP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR:$res, (z_cas_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_ZCompareAndSwapP_node : RISCV_ZCompareAndSwapP;
def RISCV_ZCompareAndSwapPAcq_node : RISCV_ZCompareAndSwapP;

// ============================================================================
// ZGC Atomic Exchange (GetAndSet) Barrier
// ============================================================================

// ZXChgP — 64-bit pointer atomic exchange
// Converted from z_riscv.ad: instruct zGetAndSetP
class RISCV_ZXChgP : GCBarrier<"amoswap.d\t$prev, $newv, ($mem)\t# zxchg", "ZGC Atomic Exchange (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR:$prev, (z_xchg_p GPR:$mem, GPR:$newv))];
}

def RISCV_ZXChgP_node : RISCV_ZXChgP;
def RISCV_ZXChgPAcq_node : RISCV_ZXChgP;

// ============================================================================
// End of RISCV_Z.td
// ============================================================================
