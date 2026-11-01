//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// RISCV_Redlet.td — Redlet Garbage Collector Barrier Instructions for RISC-V
// ==========================================================================
// This file defines Redlet GC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for RISC-V (RV64) architecture.

// ============================================================================
// Redlet Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// Redlet StoreP — Store a 64-bit object pointer with Redlet write barriers
class RISCV_RedletStoreP : GCBarrier<"sd\t$src, 0($mem)\t# redlet store ptr", "Redlet GC StoreP with write barriers (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$mem, (redlet_store_p GPR:$src))];
}

def RISCV_RedletStoreP_node : RISCV_RedletStoreP;
def RISCV_RedletStorePVolatile_node : RISCV_RedletStoreP;

// Redlet StoreN — Store a 32-bit compressed object pointer with Redlet write barriers
class RISCV_RedletStoreN : GCBarrier<"sw\t$src, 0($mem)\t# redlet narrow ptr", "Redlet GC StoreN with write barriers (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$mem, (redlet_store_n GPR:$src))];
}

def RISCV_RedletStoreN_node : RISCV_RedletStoreN;
def RISCV_RedletStoreNVolatile_node : RISCV_RedletStoreN;

// Redlet EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
class RISCV_RedletEncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tsw\t$src, 0($mem)\t# redlet narrow ptr", "Redlet GC EncodeP and StoreN (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$mem, (redlet_store_n (redlet_encode_p GPR:$src)))];
}

def RISCV_RedletEncodePAndStoreN_node : RISCV_RedletEncodePAndStoreN;
def RISCV_RedletEncodePAndStoreNVolatile_node : RISCV_RedletEncodePAndStoreN;

// ============================================================================
// Redlet Load Barriers (Concurrent Tracing & Reference Resolution)
// ============================================================================

// Redlet LoadP — Load 64-bit object pointer with Redlet load barrier
class RISCV_RedletLoadP : GCBarrier<"ld\t$dst, 0($mem)\t# redlet load", "Redlet GC Load Barrier (RISC-V)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$dst, (redlet_load_p GPR:$mem))];
}

def RISCV_RedletLoadP_node : RISCV_RedletLoadP;
def RISCV_RedletLoadPVolatile_node : RISCV_RedletLoadP;

// Redlet LoadN — Load 32-bit compressed OOP with Redlet load barrier
class RISCV_RedletLoadN : GCBarrier<"lwu\t$dst, 0($mem)\t# redlet narrow load", "Redlet GC Narrow Load Barrier (RISC-V)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$dst, (redlet_load_n GPR:$mem))];
}

def RISCV_RedletLoadN_node : RISCV_RedletLoadN;
def RISCV_RedletLoadNVolatile_node : RISCV_RedletLoadN;

// ============================================================================
// Redlet CompareAndExchange (CAE) Barriers
// ============================================================================

// Redlet CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange
class RISCV_RedletCompareAndExchangeP : GCBarrier<"amoswap.d\t$oldval, $newval, ($mem)\t# redlet cae", "Redlet GC CompareAndExchangeP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$oldval, (redlet_cae_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedletCompareAndExchangeP_node : RISCV_RedletCompareAndExchangeP;
def RISCV_RedletCompareAndExchangePAcq_node : RISCV_RedletCompareAndExchangeP;

// Redlet CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
class RISCV_RedletCompareAndExchangeN : GCBarrier<"amoswap.w\t$oldval, $newval, ($mem)\t# redlet cae", "Redlet GC CompareAndExchangeN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$oldval, (redlet_cae_n GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedletCompareAndExchangeN_node : RISCV_RedletCompareAndExchangeN;
def RISCV_RedletCompareAndExchangeNAcq_node : RISCV_RedletCompareAndExchangeN;

// ============================================================================
// Redlet CompareAndSwap (CAS) Barriers
// ============================================================================

// Redlet CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
class RISCV_RedletCompareAndSwapP : GCBarrier<"amoswap.d\t$oldval, $newval, ($mem)\t# redlet cas", "Redlet GC CompareAndSwapP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$res, (redlet_cas_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedletCompareAndSwapP_node : RISCV_RedletCompareAndSwapP;
def RISCV_RedletCompareAndSwapPAcq_node : RISCV_RedletCompareAndSwapP;
def RISCV_RedletWeakCompareAndSwapP_node : RISCV_RedletCompareAndSwapP;

// Redlet CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
class RISCV_RedletCompareAndSwapN : GCBarrier<"amoswap.w\t$oldval, $newval, ($mem)\t# redlet cas", "Redlet GC CompareAndSwapN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$res, (redlet_cas_n GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedletCompareAndSwapN_node : RISCV_RedletCompareAndSwapN;
def RISCV_RedletCompareAndSwapNAcq_node : RISCV_RedletCompareAndSwapN;
def RISCV_RedletWeakCompareAndSwapN_node : RISCV_RedletCompareAndSwapN;

// ============================================================================
// Redlet Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// Redlet GetAndSetP — 64-bit pointer atomic exchange
class RISCV_RedletGetAndSetP : GCBarrier<"amoswap.d\t$res, $newval, ($mem)\t# redlet xchg", "Redlet GC GetAndSetP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$res, (redlet_xchg_p GPR:$mem, GPR:$newval))];
}

def RISCV_RedletGetAndSetP_node : RISCV_RedletGetAndSetP;

// Redlet GetAndSetN — 32-bit compressed OOP atomic exchange
class RISCV_RedletGetAndSetN : GCBarrier<"amoswap.w\t$res, $newval, ($mem)\t# redlet xchg", "Redlet GC GetAndSetN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$res, (redlet_xchg_n GPR:$mem, GPR:$newval))];
}

def RISCV_RedletGetAndSetN_node : RISCV_RedletGetAndSetN;

// ============================================================================
// Redlet 4-Point Coordination & SIMD Dispatch Instructions (RISC-V / RVV 1.0)
// ============================================================================

// Redlet SafepointPoll — Check global STW safepoint active flag
class RISCV_RedletSafepointPoll : GCBarrier<"lwu\tzero, 0($mem)\t# redlet safepoint poll", "Redlet GC Safepoint Poll (RISC-V)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(redlet_safepoint_poll GPR:$mem)];
}

def RISCV_RedletSafepointPoll_node : RISCV_RedletSafepointPoll;

// Redlet PausepointPoll — Non-STW Pitstop ISA hot-swap gate poll
class RISCV_RedletPausepointPoll : GCBarrier<"lwu\tzero, 0($mem)\t# redlet pausepoint poll", "Redlet GC Pausepoint Poll (RISC-V)", 40, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(redlet_pausepoint_poll GPR:$mem)];
}

def RISCV_RedletPausepointPoll_node : RISCV_RedletPausepointPoll;

// Redlet HookpointPin — Atomic pin increment for JNI/FFM/SIMD critical region
class RISCV_RedletHookpointPin : GCBarrier<"amoadd.w.aqrl\t$res, $delta, ($mem)\t# redlet hookpoint pin", "Redlet GC Hookpoint Pin (RISC-V)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$res, (redlet_hookpoint_pin GPR:$mem, GPR:$delta))];
}

def RISCV_RedletHookpointPin_node : RISCV_RedletHookpointPin;

// Redlet ControlpointCheck — Host GC arbitration state load & test
class RISCV_RedletControlpointCheck : GCBarrier<"lwu\t$res, 0($mem)\t# redlet controlpoint check", "Redlet GC Controlpoint Check (RISC-V)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$res, (redlet_controlpoint_chk GPR:$mem))];
}

def RISCV_RedletControlpointCheck_node : RISCV_RedletControlpointCheck;

// Redlet SimdDispatch — RVV 1.0 vector fused multiply-accumulate dispatch
class RISCV_RedletSimdDispatch : GCBarrier<"vfmacc.vv\t$dst, $src1, $src2\t# redlet rvv simd dispatch", "Redlet RVV SIMD Dispatch (RISC-V)", 60, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR:$dst, (redlet_simd_dispatch GPR:$dst, GPR:$src1, GPR:$src2))];
}

def RISCV_RedletSimdDispatch_node : RISCV_RedletSimdDispatch;

// ============================================================================
// End of RISCV_Redlet.td
// ============================================================================
