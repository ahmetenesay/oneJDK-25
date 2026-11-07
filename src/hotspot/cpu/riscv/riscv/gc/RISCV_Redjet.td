//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// RISCV_Redjet.td — Redjet Garbage Collector Barrier Instructions for RISC-V
// ==========================================================================
// This file defines Redjet GC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for RISC-V (RV64) architecture.

// ============================================================================
// Redjet Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// Redjet StoreP — Store a 64-bit object pointer with Redjet write barriers
class RISCV_RedjetStoreP : GCBarrier<"sd\t$src, 0($mem)\t# redjet store ptr", "Redjet GC StoreP with write barriers (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$mem, (redjet_store_p GPR:$src))];
}

def RISCV_RedjetStoreP_node : RISCV_RedjetStoreP;
def RISCV_RedjetStorePVolatile_node : RISCV_RedjetStoreP;

// Redjet StoreN — Store a 32-bit compressed object pointer with Redjet write barriers
class RISCV_RedjetStoreN : GCBarrier<"sw\t$src, 0($mem)\t# redjet narrow ptr", "Redjet GC StoreN with write barriers (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$mem, (redjet_store_n GPR:$src))];
}

def RISCV_RedjetStoreN_node : RISCV_RedjetStoreN;
def RISCV_RedjetStoreNVolatile_node : RISCV_RedjetStoreN;

// Redjet EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
class RISCV_RedjetEncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tsw\t$src, 0($mem)\t# redjet narrow ptr", "Redjet GC EncodeP and StoreN (RISC-V)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$mem, (redjet_store_n (redjet_encode_p GPR:$src)))];
}

def RISCV_RedjetEncodePAndStoreN_node : RISCV_RedjetEncodePAndStoreN;
def RISCV_RedjetEncodePAndStoreNVolatile_node : RISCV_RedjetEncodePAndStoreN;

// ============================================================================
// Redjet Load Barriers (Concurrent Tracing & Reference Resolution)
// ============================================================================

// Redjet LoadP — Load 64-bit object pointer with Redjet load barrier
class RISCV_RedjetLoadP : GCBarrier<"ld\t$dst, 0($mem)\t# redjet load", "Redjet GC Load Barrier (RISC-V)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$dst, (redjet_load_p GPR:$mem))];
}

def RISCV_RedjetLoadP_node : RISCV_RedjetLoadP;
def RISCV_RedjetLoadPVolatile_node : RISCV_RedjetLoadP;

// Redjet LoadN — Load 32-bit compressed OOP with Redjet load barrier
class RISCV_RedjetLoadN : GCBarrier<"lwu\t$dst, 0($mem)\t# redjet narrow load", "Redjet GC Narrow Load Barrier (RISC-V)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$dst, (redjet_load_n GPR:$mem))];
}

def RISCV_RedjetLoadN_node : RISCV_RedjetLoadN;
def RISCV_RedjetLoadNVolatile_node : RISCV_RedjetLoadN;

// ============================================================================
// Redjet CompareAndExchange (CAE) Barriers
// ============================================================================

// Redjet CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange
class RISCV_RedjetCompareAndExchangeP : GCBarrier<"amoswap.d\t$oldval, $newval, ($mem)\t# redjet cae", "Redjet GC CompareAndExchangeP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$oldval, (redjet_cae_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedjetCompareAndExchangeP_node : RISCV_RedjetCompareAndExchangeP;
def RISCV_RedjetCompareAndExchangePAcq_node : RISCV_RedjetCompareAndExchangeP;

// Redjet CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
class RISCV_RedjetCompareAndExchangeN : GCBarrier<"amoswap.w\t$oldval, $newval, ($mem)\t# redjet cae", "Redjet GC CompareAndExchangeN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$oldval, (redjet_cae_n GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedjetCompareAndExchangeN_node : RISCV_RedjetCompareAndExchangeN;
def RISCV_RedjetCompareAndExchangeNAcq_node : RISCV_RedjetCompareAndExchangeN;

// ============================================================================
// Redjet CompareAndSwap (CAS) Barriers
// ============================================================================

// Redjet CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
class RISCV_RedjetCompareAndSwapP : GCBarrier<"amoswap.d\t$oldval, $newval, ($mem)\t# redjet cas", "Redjet GC CompareAndSwapP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_cas_p GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedjetCompareAndSwapP_node : RISCV_RedjetCompareAndSwapP;
def RISCV_RedjetCompareAndSwapPAcq_node : RISCV_RedjetCompareAndSwapP;
def RISCV_RedjetWeakCompareAndSwapP_node : RISCV_RedjetCompareAndSwapP;

// Redjet CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
class RISCV_RedjetCompareAndSwapN : GCBarrier<"amoswap.w\t$oldval, $newval, ($mem)\t# redjet cas", "Redjet GC CompareAndSwapN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_cas_n GPR:$mem, GPR:$oldval, GPR:$newval))];
}

def RISCV_RedjetCompareAndSwapN_node : RISCV_RedjetCompareAndSwapN;
def RISCV_RedjetCompareAndSwapNAcq_node : RISCV_RedjetCompareAndSwapN;
def RISCV_RedjetWeakCompareAndSwapN_node : RISCV_RedjetCompareAndSwapN;

// ============================================================================
// Redjet Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// Redjet GetAndSetP — 64-bit pointer atomic exchange
class RISCV_RedjetGetAndSetP : GCBarrier<"amoswap.d\t$res, $newval, ($mem)\t# redjet xchg", "Redjet GC GetAndSetP (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_xchg_p GPR:$mem, GPR:$newval))];
}

def RISCV_RedjetGetAndSetP_node : RISCV_RedjetGetAndSetP;

// Redjet GetAndSetN — 32-bit compressed OOP atomic exchange
class RISCV_RedjetGetAndSetN : GCBarrier<"amoswap.w\t$res, $newval, ($mem)\t# redjet xchg", "Redjet GC GetAndSetN (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_xchg_n GPR:$mem, GPR:$newval))];
}

def RISCV_RedjetGetAndSetN_node : RISCV_RedjetGetAndSetN;

// ============================================================================
// Redjet 4-Point Coordination & SIMD Dispatch Instructions (RISC-V / RVV 1.0)
// ============================================================================

// Redjet SafepointPoll — Check global STW safepoint active flag
class RISCV_RedjetSafepointPoll : GCBarrier<"lwu\tzero, 0($mem)\t# redjet safepoint poll", "Redjet GC Safepoint Poll (RISC-V)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_safepoint_poll GPR:$mem)];
}

def RISCV_RedjetSafepointPoll_node : RISCV_RedjetSafepointPoll;

// Redjet PausepointPoll — Non-STW Pitstop ISA hot-swap gate poll
class RISCV_RedjetPausepointPoll : GCBarrier<"lwu\tzero, 0($mem)\t# redjet pausepoint poll", "Redjet GC Pausepoint Poll (RISC-V)", 40, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_pausepoint_poll GPR:$mem)];
}

def RISCV_RedjetPausepointPoll_node : RISCV_RedjetPausepointPoll;

// Redjet HookpointPin — Atomic pin increment for JNI/FFM/SIMD critical region
class RISCV_RedjetHookpointPin : GCBarrier<"amoadd.w.aqrl\t$res, $delta, ($mem)\t# redjet hookpoint pin", "Redjet GC Hookpoint Pin (RISC-V)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_hookpoint_pin GPR:$mem, GPR:$delta))];
}

def RISCV_RedjetHookpointPin_node : RISCV_RedjetHookpointPin;

// Redjet ControlpointCheck — Host GC arbitration state load & test
class RISCV_RedjetControlpointCheck : GCBarrier<"lwu\t$res, 0($mem)\t# redjet controlpoint check", "Redjet GC Controlpoint Check (RISC-V)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_controlpoint_chk GPR:$mem))];
}

def RISCV_RedjetControlpointCheck_node : RISCV_RedjetControlpointCheck;

// Redjet SimdDispatch — RVV 1.0 vector fused multiply-accumulate dispatch
class RISCV_RedjetSimdDispatch : GCBarrier<"vfmacc.vv\t$dst, $src1, $src2\t# redjet rvv simd dispatch", "Redjet RVV SIMD Dispatch (RISC-V)", 60, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$dst, (redjet_simd_dispatch GPR:$dst, GPR:$src1, GPR:$src2))];
}

def RISCV_RedjetSimdDispatch_node : RISCV_RedjetSimdDispatch;

// ============================================================================
// Redjet 2-Point Zero-STW Hardware Sync & Universal MachPollPoint
// ============================================================================

// Redjet Dockpoint — Method entry 16-byte trampoline to Slot 16 target
class RISCV_RedjetDockpoint : GCBarrier<"j\t$slot_target\t# redjet dockpoint slot16 trampoline", "Redjet GC Dockpoint Method Entry Trampoline (RISC-V)", 10, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dockpoint simm21_lsb0_jal:$slot_target)];
}

def RISCV_RedjetDockpoint_node : RISCV_RedjetDockpoint;

// Redjet Parkpoint — Loop bottom 0.5-cycle L1 D-Cache thread sync check
class RISCV_RedjetParkpoint : GCBarrier<"andi\t$tmp, $park_word, 1\n\tbnez\t$tmp, $cold_park_and_swap\t# redjet parkpoint", "Redjet GC Parkpoint Loop Poll (RISC-V)", 20, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_parkpoint GPR:$tmp, GPR:$park_word, bare_simm13_lsb0:$cold_park_and_swap)];
}

def RISCV_RedjetParkpoint_node : RISCV_RedjetParkpoint;

// Redjet MachPollPoint — Universal hardware poll point for Redjet, G1, Shenandoah, and ZGC
class RISCV_MachPollPoint : GCBarrier<"lwu\tzero, 0($safepoint_poll_addr)\t# MachPollPoint", "HotSpot Universal MachPollPoint (RISC-V)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(mach_poll_point GPR:$safepoint_poll_addr, i32imm:$poll_type)];
}

def RISCV_MachPollPoint_node : RISCV_MachPollPoint;

// ============================================================================
// Redjet TailTraces Clasp Dead Reference Signal & Off-Heap Eviction
// ============================================================================

// Redjet DeadReferenceSignal — Clasp emission to RPSS ring buffer
class RISCV_RedjetDeadRefSignal : GCBarrier<"sd\t$dead_oop, 0($rpss_addr)\t# redjet tailtraces signal", "Redjet TailTraces Clasp Dead Reference Signal (RISC-V)", 15, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dead_ref_signal GPR:$rpss_addr, GPR:$dead_oop)];
}

def RISCV_RedjetDeadRefSignal_node : RISCV_RedjetDeadRefSignal;

// Redjet OffHeapEvict — Deterministic RAII eviction for @RJGC(Mark = "ImmediateEviction")
class RISCV_RedjetOffHeapEvict : GCBarrier<"call\tredjet_offheap_evict\t# redjet offheap evict", "Redjet Off-Heap Immediate Eviction Barrier (RISC-V)", 40, "call"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_offheap_evict GPR:$handle_ptr)];
}

def RISCV_RedjetOffHeapEvict_node : RISCV_RedjetOffHeapEvict;

// ============================================================================
// End of RISCV_Redjet.td
// ============================================================================
