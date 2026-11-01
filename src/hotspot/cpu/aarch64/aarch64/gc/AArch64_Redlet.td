//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// AArch64_Redlet.td — Redlet Garbage Collector Barrier Instructions for AArch64 (ARM64)
// ====================================================================================
// This file defines Redlet GC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for AArch64 architecture.

// ============================================================================
// Redlet Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// Redlet StoreP — Store a 64-bit object pointer with Redlet write barriers
class AArch64_RedletStoreP : GCBarrier<"str\t$src, [$mem]\t# redlet store ptr", "Redlet GC StoreP with write barriers (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64sp:$mem, (redlet_store_p GPR64:$src))];
}

def AArch64_RedletStoreP_node : AArch64_RedletStoreP;
def AArch64_RedletStorePVolatile_node : AArch64_RedletStoreP;

// Redlet StoreN — Store a 32-bit compressed object pointer with Redlet write barriers
class AArch64_RedletStoreN : GCBarrier<"str\t$src, [$mem]\t# redlet narrow ptr", "Redlet GC StoreN with write barriers (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64sp:$mem, (redlet_store_n GPR32:$src))];
}

def AArch64_RedletStoreN_node : AArch64_RedletStoreN;
def AArch64_RedletStoreNVolatile_node : AArch64_RedletStoreN;

// Redlet EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
class AArch64_RedletEncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tstr\t$src, [$mem]\t# redlet narrow ptr", "Redlet GC EncodeP and StoreN (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64sp:$mem, (redlet_store_n (redlet_encode_p GPR64:$src)))];
}

def AArch64_RedletEncodePAndStoreN_node : AArch64_RedletEncodePAndStoreN;
def AArch64_RedletEncodePAndStoreNVolatile_node : AArch64_RedletEncodePAndStoreN;

// ============================================================================
// Redlet Load Barriers (Concurrent Tracing & Reference Resolution)
// ============================================================================

// Redlet LoadP — Load 64-bit object pointer with Redlet load barrier
class AArch64_RedletLoadP : GCBarrier<"ldr\t$dst, [$mem]\t# redlet load", "Redlet GC Load Barrier (AArch64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64:$dst, (redlet_load_p GPR64sp:$mem))];
}

def AArch64_RedletLoadP_node : AArch64_RedletLoadP;
def AArch64_RedletLoadPVolatile_node : AArch64_RedletLoadP;

// Redlet LoadN — Load 32-bit compressed OOP with Redlet load barrier
class AArch64_RedletLoadN : GCBarrier<"ldr\t$dst, [$mem]\t# redlet narrow load", "Redlet GC Narrow Load Barrier (AArch64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$dst, (redlet_load_n GPR64sp:$mem))];
}

def AArch64_RedletLoadN_node : AArch64_RedletLoadN;
def AArch64_RedletLoadNVolatile_node : AArch64_RedletLoadN;

// ============================================================================
// Redlet CompareAndExchange (CAE) Barriers
// ============================================================================

// Redlet CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with barriers
class AArch64_RedletCompareAndExchangeP : GCBarrier<"casal\t$oldval, $newval, [$mem]\t# redlet cae", "Redlet GC CompareAndExchangeP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64:$oldval, (redlet_cae_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_RedletCompareAndExchangeP_node : AArch64_RedletCompareAndExchangeP;
def AArch64_RedletCompareAndExchangePAcq_node : AArch64_RedletCompareAndExchangeP;

// Redlet CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
class AArch64_RedletCompareAndExchangeN : GCBarrier<"casal\t$oldval, $newval, [$mem]\t# redlet cae", "Redlet GC CompareAndExchangeN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$oldval, (redlet_cae_n GPR64sp:$mem, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_RedletCompareAndExchangeN_node : AArch64_RedletCompareAndExchangeN;
def AArch64_RedletCompareAndExchangeNAcq_node : AArch64_RedletCompareAndExchangeN;

// ============================================================================
// Redlet CompareAndSwap (CAS) Barriers
// ============================================================================

// Redlet CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
class AArch64_RedletCompareAndSwapP : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq\t# redlet cas", "Redlet GC CompareAndSwapP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$res, (redlet_cas_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_RedletCompareAndSwapP_node : AArch64_RedletCompareAndSwapP;
def AArch64_RedletCompareAndSwapPAcq_node : AArch64_RedletCompareAndSwapP;
def AArch64_RedletWeakCompareAndSwapP_node : AArch64_RedletCompareAndSwapP;

// Redlet CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
class AArch64_RedletCompareAndSwapN : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq\t# redlet cas", "Redlet GC CompareAndSwapN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$res, (redlet_cas_n GPR64sp:$mem, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_RedletCompareAndSwapN_node : AArch64_RedletCompareAndSwapN;
def AArch64_RedletCompareAndSwapNAcq_node : AArch64_RedletCompareAndSwapN;
def AArch64_RedletWeakCompareAndSwapN_node : AArch64_RedletCompareAndSwapN;

// ============================================================================
// Redlet Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// Redlet GetAndSetP — 64-bit pointer atomic exchange
class AArch64_RedletGetAndSetP : GCBarrier<"swpal\t$newval, $res, [$mem]\t# redlet xchg", "Redlet GC GetAndSetP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64:$res, (redlet_xchg_p GPR64sp:$mem, GPR64:$newval))];
}

def AArch64_RedletGetAndSetP_node : AArch64_RedletGetAndSetP;

// Redlet GetAndSetN — 32-bit compressed OOP atomic exchange
class AArch64_RedletGetAndSetN : GCBarrier<"swpal\t$newval, $res, [$mem]\t# redlet xchg", "Redlet GC GetAndSetN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$res, (redlet_xchg_n GPR64sp:$mem, GPR32:$newval))];
}

def AArch64_RedletGetAndSetN_node : AArch64_RedletGetAndSetN;

// ============================================================================
// Redlet 4-Point Coordination & SIMD Dispatch Instructions (AArch64 / SVE2)
// ============================================================================

// Redlet SafepointPoll — Check global STW safepoint active flag
class AArch64_RedletSafepointPoll : GCBarrier<"ldr\twzr, [$mem]\t# redlet safepoint poll", "Redlet GC Safepoint Poll (AArch64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(redlet_safepoint_poll GPR64sp:$mem)];
}

def AArch64_RedletSafepointPoll_node : AArch64_RedletSafepointPoll;

// Redlet PausepointPoll — Non-STW Pitstop ISA hot-swap gate poll
class AArch64_RedletPausepointPoll : GCBarrier<"ldar\twzr, [$mem]\t# redlet pausepoint poll", "Redlet GC Pausepoint Poll (AArch64)", 40, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(redlet_pausepoint_poll GPR64sp:$mem)];
}

def AArch64_RedletPausepointPoll_node : AArch64_RedletPausepointPoll;

// Redlet HookpointPin — Atomic pin increment for JNI/FFM/SIMD critical region
class AArch64_RedletHookpointPin : GCBarrier<"ldaddal\t$delta, $res, [$mem]\t# redlet hookpoint pin", "Redlet GC Hookpoint Pin (AArch64)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$res, (redlet_hookpoint_pin GPR64sp:$mem, GPR32:$delta))];
}

def AArch64_RedletHookpointPin_node : AArch64_RedletHookpointPin;

// Redlet ControlpointCheck — Host GC arbitration state load & test
class AArch64_RedletControlpointCheck : GCBarrier<"ldar\t$res, [$mem]\t# redlet controlpoint check", "Redlet GC Controlpoint Check (AArch64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR32:$res, (redlet_controlpoint_chk GPR64sp:$mem))];
}

def AArch64_RedletControlpointCheck_node : AArch64_RedletControlpointCheck;

// Redlet SimdDispatch — SVE2 / NEON predicated fused multiply-add vector dispatch
class AArch64_RedletSimdDispatch : GCBarrier<"fmla\t$dst.d, p0/m, $src1.d, $src2.d\t# redlet sve2 simd dispatch", "Redlet SVE2 SIMD Dispatch (AArch64)", 60, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GPR64:$dst, (redlet_simd_dispatch GPR64:$dst, GPR64:$src1, GPR64:$src2))];
}

def AArch64_RedletSimdDispatch_node : AArch64_RedletSimdDispatch;

// ============================================================================
// End of AArch64_Redlet.td
// ============================================================================
