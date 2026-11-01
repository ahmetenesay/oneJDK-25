//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// X86_Redlet.td — Redlet Garbage Collector Barrier Instructions for x86_64
// ========================================================================
// This file defines Redlet GC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for x86_64 architecture.

// ============================================================================
// Redlet Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// Redlet StoreP — Store a 64-bit object pointer with Redlet write barriers
class X86_RedletStoreP : GCBarrier<"movq\t$mem, $src\t# redlet store ptr", "Redlet GC StoreP with write barriers (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set i64mem:$mem, (redlet_store_p GR64:$src))];
}

def X86_RedletStoreP_node : X86_RedletStoreP;

// Redlet StoreN — Store a 32-bit compressed object pointer with Redlet write barriers
class X86_RedletStoreN : GCBarrier<"movl\t$mem, $src\t# redlet narrow ptr", "Redlet GC StoreN with write barriers (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set i32mem:$mem, (redlet_store_n GR32:$src))];
}

def X86_RedletStoreN_node : X86_RedletStoreN;

// Redlet EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
class X86_RedletEncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tmovl\t$mem, $src\t# redlet narrow ptr", "Redlet GC EncodeP and StoreN (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set i32mem:$mem, (redlet_store_n (redlet_encode_p GR64:$src)))];
}

def X86_RedletEncodePAndStoreN_node : X86_RedletEncodePAndStoreN;

// ============================================================================
// Redlet Load Barriers (Concurrent Tracing & Reference Resolution)
// ============================================================================

// Redlet LoadP — Load 64-bit object pointer with Redlet load barrier
class X86_RedletLoadP : GCBarrier<"movq\t$dst, $mem\t# redlet load", "Redlet GC Load Barrier (x86_64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR64:$dst, (redlet_load_p i64mem:$mem))];
}

def X86_RedletLoadP_node : X86_RedletLoadP;

// Redlet LoadN — Load 32-bit compressed OOP with Redlet load barrier
class X86_RedletLoadN : GCBarrier<"movl\t$dst, $mem\t# redlet narrow load", "Redlet GC Narrow Load Barrier (x86_64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$dst, (redlet_load_n i32mem:$mem))];
}

def X86_RedletLoadN_node : X86_RedletLoadN;

// ============================================================================
// Redlet CompareAndExchange (CAE) Barriers
// ============================================================================

// Redlet CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with barriers
class X86_RedletCompareAndExchangeP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\t# redlet cae", "Redlet GC CompareAndExchangeP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR64:$oldval, (redlet_cae_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_RedletCompareAndExchangeP_node : X86_RedletCompareAndExchangeP;

// Redlet CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
class X86_RedletCompareAndExchangeN : GCBarrier<"lock\n\tcmpxchgl\t$newval, $mem\t# redlet cae", "Redlet GC CompareAndExchangeN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$oldval, (redlet_cae_n i32mem:$mem, GR32:$oldval, GR32:$newval))];
}

def X86_RedletCompareAndExchangeN_node : X86_RedletCompareAndExchangeN;

// ============================================================================
// Redlet CompareAndSwap (CAS) Barriers
// ============================================================================

// Redlet CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
class X86_RedletCompareAndSwapP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\n\tsetcc\t$res", "Redlet GC CompareAndSwapP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$res, (redlet_cas_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_RedletCompareAndSwapP_node : X86_RedletCompareAndSwapP;
def X86_RedletWeakCompareAndSwapP_node : X86_RedletCompareAndSwapP;

// Redlet CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
class X86_RedletCompareAndSwapN : GCBarrier<"lock\n\tcmpxchgl\t$newval, $mem\n\tsetcc\t$res", "Redlet GC CompareAndSwapN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$res, (redlet_cas_n i32mem:$mem, GR32:$oldval, GR32:$newval))];
}

def X86_RedletCompareAndSwapN_node : X86_RedletCompareAndSwapN;
def X86_RedletWeakCompareAndSwapN_node : X86_RedletCompareAndSwapN;

// ============================================================================
// Redlet Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// Redlet GetAndSetP — 64-bit pointer atomic exchange
class X86_RedletGetAndSetP : GCBarrier<"xchgq\t$res, $mem\t# redlet xchg", "Redlet GC GetAndSetP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR64:$res, (redlet_xchg_p i64mem:$mem, GR64:$newval))];
}

def X86_RedletGetAndSetP_node : X86_RedletGetAndSetP;

// Redlet GetAndSetN — 32-bit compressed OOP atomic exchange
class X86_RedletGetAndSetN : GCBarrier<"xchgl\t$res, $mem\t# redlet xchg", "Redlet GC GetAndSetN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$res, (redlet_xchg_n i32mem:$mem, GR32:$newval))];
}

def X86_RedletGetAndSetN_node : X86_RedletGetAndSetN;

// ============================================================================
// Redlet 4-Point Coordination & SIMD Dispatch Instructions (x86_64 / AVX-512)
// ============================================================================

// Redlet SafepointPoll — Check global STW safepoint active flag
class X86_RedletSafepointPoll : GCBarrier<"cmpl\t$mem, $$0\t# redlet safepoint poll", "Redlet GC Safepoint Poll (x86_64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(redlet_safepoint_poll i32mem:$mem)];
}

def X86_RedletSafepointPoll_node : X86_RedletSafepointPoll;

// Redlet PausepointPoll — Non-STW Pitstop ISA hot-swap gate poll
class X86_RedletPausepointPoll : GCBarrier<"cmpl\t$mem, $$0\t# redlet pausepoint poll", "Redlet GC Pausepoint Poll (x86_64)", 40, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(redlet_pausepoint_poll i32mem:$mem)];
}

def X86_RedletPausepointPoll_node : X86_RedletPausepointPoll;

// Redlet HookpointPin — Atomic pin increment for JNI/FFM/SIMD critical region
class X86_RedletHookpointPin : GCBarrier<"lock\n\txaddl\t$res, $mem\t# redlet hookpoint pin", "Redlet GC Hookpoint Pin (x86_64)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$res, (redlet_hookpoint_pin i32mem:$mem, GR32:$delta))];
}

def X86_RedletHookpointPin_node : X86_RedletHookpointPin;

// Redlet ControlpointCheck — Host GC arbitration state load & test
class X86_RedletControlpointCheck : GCBarrier<"movl\t$res, $mem\t# redlet controlpoint check", "Redlet GC Controlpoint Check (x86_64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR32:$res, (redlet_controlpoint_chk i32mem:$mem))];
}

def X86_RedletControlpointCheck_node : X86_RedletControlpointCheck;

// Redlet SimdDispatch — AVX-512 / AVX10.2 fused multiply-add vector kernel dispatch
class X86_RedletSimdDispatch : GCBarrier<"vfmadd231pd\t$src2, $src1, $dst\t# redlet avx512 simd dispatch", "Redlet AVX-512 SIMD Dispatch (x86_64)", 60, "ialu_reg_mem"> {
  let BarrierType = 3; // Redlet
  list<dag> Pattern = [(set GR64:$dst, (redlet_simd_dispatch GR64:$dst, GR64:$src1, GR64:$src2))];
}

def X86_RedletSimdDispatch_node : X86_RedletSimdDispatch;

// ============================================================================
// End of X86_Redlet.td
// ============================================================================
