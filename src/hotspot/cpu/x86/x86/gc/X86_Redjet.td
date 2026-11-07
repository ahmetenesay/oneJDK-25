//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// X86_Redjet.td — Redjet Garbage Collector Barrier Instructions for x86_64
// ========================================================================
// This file defines Redjet GC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for x86_64 architecture.

// ============================================================================
// Redjet Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// Redjet StoreP — Store a 64-bit object pointer with Redjet write barriers
class X86_RedjetStoreP : GCBarrier<"movq\t$mem, $src\t# redjet store ptr", "Redjet GC StoreP with write barriers (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set i64mem:$mem, (redjet_store_p GR64:$src))];
}

def X86_RedjetStoreP_node : X86_RedjetStoreP;

// Redjet StoreN — Store a 32-bit compressed object pointer with Redjet write barriers
class X86_RedjetStoreN : GCBarrier<"movl\t$mem, $src\t# redjet narrow ptr", "Redjet GC StoreN with write barriers (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set i32mem:$mem, (redjet_store_n GR32:$src))];
}

def X86_RedjetStoreN_node : X86_RedjetStoreN;

// Redjet EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
class X86_RedjetEncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tmovl\t$mem, $src\t# redjet narrow ptr", "Redjet GC EncodeP and StoreN (x86_64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set i32mem:$mem, (redjet_store_n (redjet_encode_p GR64:$src)))];
}

def X86_RedjetEncodePAndStoreN_node : X86_RedjetEncodePAndStoreN;

// ============================================================================
// Redjet Load Barriers (Concurrent Tracing & Reference Resolution)
// ============================================================================

// Redjet LoadP — Load 64-bit object pointer with Redjet load barrier
class X86_RedjetLoadP : GCBarrier<"movq\t$dst, $mem\t# redjet load", "Redjet GC Load Barrier (x86_64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR64:$dst, (redjet_load_p i64mem:$mem))];
}

def X86_RedjetLoadP_node : X86_RedjetLoadP;

// Redjet LoadN — Load 32-bit compressed OOP with Redjet load barrier
class X86_RedjetLoadN : GCBarrier<"movl\t$dst, $mem\t# redjet narrow load", "Redjet GC Narrow Load Barrier (x86_64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$dst, (redjet_load_n i32mem:$mem))];
}

def X86_RedjetLoadN_node : X86_RedjetLoadN;

// ============================================================================
// Redjet CompareAndExchange (CAE) Barriers
// ============================================================================

// Redjet CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with barriers
class X86_RedjetCompareAndExchangeP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\t# redjet cae", "Redjet GC CompareAndExchangeP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR64:$oldval, (redjet_cae_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_RedjetCompareAndExchangeP_node : X86_RedjetCompareAndExchangeP;

// Redjet CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
class X86_RedjetCompareAndExchangeN : GCBarrier<"lock\n\tcmpxchgl\t$newval, $mem\t# redjet cae", "Redjet GC CompareAndExchangeN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$oldval, (redjet_cae_n i32mem:$mem, GR32:$oldval, GR32:$newval))];
}

def X86_RedjetCompareAndExchangeN_node : X86_RedjetCompareAndExchangeN;

// ============================================================================
// Redjet CompareAndSwap (CAS) Barriers
// ============================================================================

// Redjet CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
class X86_RedjetCompareAndSwapP : GCBarrier<"lock\n\tcmpxchgq\t$newval, $mem\n\tsetcc\t$res", "Redjet GC CompareAndSwapP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$res, (redjet_cas_p i64mem:$mem, GR64:$oldval, GR64:$newval))];
}

def X86_RedjetCompareAndSwapP_node : X86_RedjetCompareAndSwapP;
def X86_RedjetWeakCompareAndSwapP_node : X86_RedjetCompareAndSwapP;

// Redjet CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
class X86_RedjetCompareAndSwapN : GCBarrier<"lock\n\tcmpxchgl\t$newval, $mem\n\tsetcc\t$res", "Redjet GC CompareAndSwapN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$res, (redjet_cas_n i32mem:$mem, GR32:$oldval, GR32:$newval))];
}

def X86_RedjetCompareAndSwapN_node : X86_RedjetCompareAndSwapN;
def X86_RedjetWeakCompareAndSwapN_node : X86_RedjetCompareAndSwapN;

// ============================================================================
// Redjet Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// Redjet GetAndSetP — 64-bit pointer atomic exchange
class X86_RedjetGetAndSetP : GCBarrier<"xchgq\t$res, $mem\t# redjet xchg", "Redjet GC GetAndSetP (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR64:$res, (redjet_xchg_p i64mem:$mem, GR64:$newval))];
}

def X86_RedjetGetAndSetP_node : X86_RedjetGetAndSetP;

// Redjet GetAndSetN — 32-bit compressed OOP atomic exchange
class X86_RedjetGetAndSetN : GCBarrier<"xchgl\t$res, $mem\t# redjet xchg", "Redjet GC GetAndSetN (x86_64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$res, (redjet_xchg_n i32mem:$mem, GR32:$newval))];
}

def X86_RedjetGetAndSetN_node : X86_RedjetGetAndSetN;

// ============================================================================
// Redjet 4-Point Coordination & SIMD Dispatch Instructions (x86_64 / AVX-512)
// ============================================================================

// Redjet SafepointPoll — Check global STW safepoint active flag
class X86_RedjetSafepointPoll : GCBarrier<"cmpl\t$mem, $$0\t# redjet safepoint poll", "Redjet GC Safepoint Poll (x86_64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_safepoint_poll i32mem:$mem)];
}

def X86_RedjetSafepointPoll_node : X86_RedjetSafepointPoll;

// Redjet PausepointPoll — Non-STW Pitstop ISA hot-swap gate poll
class X86_RedjetPausepointPoll : GCBarrier<"cmpl\t$mem, $$0\t# redjet pausepoint poll", "Redjet GC Pausepoint Poll (x86_64)", 40, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_pausepoint_poll i32mem:$mem)];
}

def X86_RedjetPausepointPoll_node : X86_RedjetPausepointPoll;

// Redjet HookpointPin — Atomic pin increment for JNI/FFM/SIMD critical region
class X86_RedjetHookpointPin : GCBarrier<"lock\n\txaddl\t$res, $mem\t# redjet hookpoint pin", "Redjet GC Hookpoint Pin (x86_64)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$res, (redjet_hookpoint_pin i32mem:$mem, GR32:$delta))];
}

def X86_RedjetHookpointPin_node : X86_RedjetHookpointPin;

// Redjet ControlpointCheck — Host GC arbitration state load & test
class X86_RedjetControlpointCheck : GCBarrier<"movl\t$res, $mem\t# redjet controlpoint check", "Redjet GC Controlpoint Check (x86_64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR32:$res, (redjet_controlpoint_chk i32mem:$mem))];
}

def X86_RedjetControlpointCheck_node : X86_RedjetControlpointCheck;

// Redjet SimdDispatch — AVX-512 / AVX10.2 fused multiply-add vector kernel dispatch
class X86_RedjetSimdDispatch : GCBarrier<"vfmadd231pd\t$src2, $src1, $dst\t# redjet avx512 simd dispatch", "Redjet AVX-512 SIMD Dispatch (x86_64)", 60, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GR64:$dst, (redjet_simd_dispatch GR64:$dst, GR64:$src1, GR64:$src2))];
}

def X86_RedjetSimdDispatch_node : X86_RedjetSimdDispatch;

// ============================================================================
// Redjet 2-Point Zero-STW Hardware Sync & Universal MachPollPoint
// ============================================================================

// Redjet Dockpoint — Method entry 16-byte trampoline to Slot 16 target
class X86_RedjetDockpoint : GCBarrier<"jmp\t$slot_target\t# redjet dockpoint slot16 trampoline", "Redjet GC Dockpoint Method Entry Trampoline (x86_64)", 10, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dockpoint brtarget:$slot_target)];
}

def X86_RedjetDockpoint_node : X86_RedjetDockpoint;

// Redjet Parkpoint — Loop bottom 0.5-cycle L1 D-Cache thread sync check
class X86_RedjetParkpoint : GCBarrier<"testb\t$$1, [r15 + $park_offset]\n\tjnz\t$cold_park_and_swap\t# redjet parkpoint", "Redjet GC Parkpoint Loop Poll (x86_64)", 20, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_parkpoint i32imm:$park_offset, brtarget:$cold_park_and_swap)];
}

def X86_RedjetParkpoint_node : X86_RedjetParkpoint;

// Redjet MachPollPoint — Universal hardware poll point for Redjet, G1, Shenandoah, and ZGC
class X86_MachPollPoint : GCBarrier<"testl\teax, [$safepoint_poll_addr]\t# MachPollPoint", "HotSpot Universal MachPollPoint (x86_64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(mach_poll_point i64mem:$safepoint_poll_addr, i32imm:$poll_type)];
}

def X86_MachPollPoint_node : X86_MachPollPoint;

// ============================================================================
// Redjet TailTraces Clasp Dead Reference Signal & Off-Heap Eviction
// ============================================================================

// Redjet DeadReferenceSignal — Clasp emission to RPSS ring buffer
class X86_RedjetDeadRefSignal : GCBarrier<"movq\t[$rpss_addr], $dead_oop\t# redjet tailtraces signal", "Redjet TailTraces Clasp Dead Reference Signal (x86_64)", 15, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dead_ref_signal i64mem:$rpss_addr, GR64:$dead_oop)];
}

def X86_RedjetDeadRefSignal_node : X86_RedjetDeadRefSignal;

// Redjet OffHeapEvict — Deterministic RAII eviction for @RJGC(Mark = "ImmediateEviction")
class X86_RedjetOffHeapEvict : GCBarrier<"call\tredjet_offheap_evict\t# redjet offheap evict", "Redjet Off-Heap Immediate Eviction Barrier (x86_64)", 40, "call"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_offheap_evict GR64:$handle_ptr)];
}

def X86_RedjetOffHeapEvict_node : X86_RedjetOffHeapEvict;

// ============================================================================
// End of X86_Redjet.td
// ============================================================================
