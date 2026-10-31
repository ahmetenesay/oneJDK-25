//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// AArch64_Z.td — Z Garbage Collector (ZGC) Barrier Instructions for AArch64 (ARM64)
// =================================================================================
// Converted from:
//   - src/hotspot/cpu/aarch64/gc/z/z_aarch64.ad
//
// This file defines ZGC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for AArch64.

// ============================================================================
// ZGC Load Barriers
// ============================================================================

// ZLoadP — Load 64-bit object pointer with ZGC load barrier
// Converted from z_aarch64.ad: instruct zLoadP
class AArch64_ZLoadP : GCBarrier<"z_load_barrier\t[$mem], $dst", "ZGC Load Barrier (AArch64)", 125, "ialu_reg_mem"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR64:$dst, (z_load_p GPR64sp:$mem))];
}

def AArch64_ZLoadP_node : AArch64_ZLoadP;
def AArch64_ZLoadPVolatile_node : AArch64_ZLoadP;

// ============================================================================
// ZGC Store Barriers
// ============================================================================

// ZStoreP — Store 64-bit object pointer with ZGC store barrier
// Converted from z_aarch64.ad: instruct zStoreP
class AArch64_ZStoreP : GCBarrier<"z_store_barrier\t[$mem], $src", "ZGC Store Barrier (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR64sp:$mem, (z_store_p GPR64:$src))];
}

def AArch64_ZStoreP_node : AArch64_ZStoreP;
def AArch64_ZStorePVolatile_node : AArch64_ZStoreP;

// ============================================================================
// ZGC CompareAndExchange / CompareAndSwap Barriers
// ============================================================================

// ZCompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with ZGC barrier
// Converted from z_aarch64.ad: instruct zCompareAndExchangeP
class AArch64_ZCompareAndExchangeP : GCBarrier<"casal\t$oldval, $newval, [$mem]\t# zcae", "ZGC CompareAndExchangeP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR64:$oldval, (z_cae_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_ZCompareAndExchangeP_node : AArch64_ZCompareAndExchangeP;
def AArch64_ZCompareAndExchangePAcq_node : AArch64_ZCompareAndExchangeP;

// ZCompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from z_aarch64.ad: instruct zCompareAndSwapP
class AArch64_ZCompareAndSwapP : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq\t# zcas", "ZGC CompareAndSwapP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR32:$res, (z_cas_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_ZCompareAndSwapP_node : AArch64_ZCompareAndSwapP;
def AArch64_ZCompareAndSwapPAcq_node : AArch64_ZCompareAndSwapP;

// ============================================================================
// ZGC Atomic Exchange (GetAndSet) Barrier
// ============================================================================

// ZXChgP — 64-bit pointer atomic exchange
// Converted from z_aarch64.ad: instruct zGetAndSetP
class AArch64_ZXChgP : GCBarrier<"swpal\t$newv, $prev, [$mem]\t# zxchg", "ZGC Atomic Exchange (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GPR64:$prev, (z_xchg_p GPR64sp:$mem, GPR64:$newv))];
}

def AArch64_ZXChgP_node : AArch64_ZXChgP;
def AArch64_ZXChgPAcq_node : AArch64_ZXChgP;

// ============================================================================
// End of AArch64_Z.td
// ============================================================================
