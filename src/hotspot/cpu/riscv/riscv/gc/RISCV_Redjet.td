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

// Redjet DeadReferenceSignal — Clasp emission to RPSS ring buffer (64-bit)
class RISCV_RedjetDeadRefSignal : GCBarrier<"sd\t$dead_oop, 0($rpss_addr)\t# redjet tailtraces signal", "Redjet TailTraces Clasp Dead Reference Signal (RISC-V)", 15, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dead_ref_signal GPR:$rpss_addr, GPR:$dead_oop)];
}

def RISCV_RedjetDeadRefSignal_node : RISCV_RedjetDeadRefSignal;
def RISCV_RedjetDeadRefSignalVolatile_node : RISCV_RedjetDeadRefSignal;

// Redjet DeadReferenceSignalN — Narrow 32-bit compressed OOP clasp emission
class RISCV_RedjetDeadRefSignalN : GCBarrier<"sw\t$dead_oop, 0($rpss_addr)\t# redjet tailtraces narrow signal", "Redjet TailTraces Clasp Narrow Dead Reference Signal (RISC-V)", 15, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dead_ref_signal_n GPR:$rpss_addr, GPR:$dead_oop)];
}

def RISCV_RedjetDeadRefSignalN_node : RISCV_RedjetDeadRefSignalN;

// Redjet DeadReferenceSignalAtomic — Atomic exchange clasp emission
class RISCV_RedjetDeadRefSignalAtomic : GCBarrier<"amoswap.d.aqrl\t$res, $dead_oop, ($rpss_addr)\t# redjet tailtraces atomic signal", "Redjet TailTraces Atomic Clasp Signal (RISC-V)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_dead_ref_signal_atomic GPR:$rpss_addr, GPR:$dead_oop))];
}

def RISCV_RedjetDeadRefSignalAtomic_node : RISCV_RedjetDeadRefSignalAtomic;

// Redjet OffHeapEvict — Deterministic RAII eviction for @RJGC(Mark = "ImmediateEviction")
class RISCV_RedjetOffHeapEvict : GCBarrier<"call\tredjet_offheap_evict\t# redjet offheap evict", "Redjet Off-Heap Immediate Eviction Barrier (RISC-V)", 40, "call"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_offheap_evict GPR:$handle_ptr)];
}

def RISCV_RedjetOffHeapEvict_node : RISCV_RedjetOffHeapEvict;

// ============================================================================
// Redjet EmptyBox Guard (Phantom Slot & Tombstone Hardware Protection)
// Magic: 0xEB0C5107
// ============================================================================

// Redjet EmptyBoxGuard — Tests slot header against HS_EMPTYBOX_MAGIC (0xEB0C5107)
class RISCV_RedjetEmptyBoxGuard : GCBarrier<"lwu\tt0, 0($mem)\n\tli\tt1, 0xEB0C5107\n\tsub\tt0, t0, t1\t# redjet emptybox guard", "Redjet GC EmptyBox Guard Test (RISC-V)", 15, "ialu_mem_imm"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_emptybox_guard GPR:$mem)];
}

def RISCV_RedjetEmptyBoxGuard_node : RISCV_RedjetEmptyBoxGuard;

// Redjet EmptyBoxGuardBranch — Tests magic and branches if slot is a protected tombstone
class RISCV_RedjetEmptyBoxGuardBranch : GCBarrier<"lwu\tt0, 0($mem)\n\tli\tt1, 0xEB0C5107\n\tbeq\tt0, t1, $parked_protect\t# redjet emptybox branch", "Redjet GC EmptyBox Branch Guard (RISC-V)", 20, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_emptybox_guard_br GPR:$mem, bare_simm13_lsb0:$parked_protect)];
}

def RISCV_RedjetEmptyBoxGuardBranch_node : RISCV_RedjetEmptyBoxGuardBranch;

// Redjet EmptyBoxClaim — Atomic compare-and-exchange to reclaim parked slot
class RISCV_RedjetEmptyBoxClaim : GCBarrier<"lr.d/sc.d\t$res, $new_oop, ($mem)\t# redjet emptybox claim", "Redjet GC EmptyBox Atomic Claim (RISC-V)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$res, (redjet_emptybox_claim GPR:$mem, GPR:$expected, GPR:$new_oop))];
}

def RISCV_RedjetEmptyBoxClaim_node : RISCV_RedjetEmptyBoxClaim;

// Redjet EmptyBoxTimeoutCheck — Compares current timestamp against timeout deadline
class RISCV_RedjetEmptyBoxTimeoutCheck : GCBarrier<"rdtime\tt0\n\tsltu\tt1, t0, $timeout_tsc\n\txori\t$is_expired, t1, 1\t# redjet emptybox timeout", "Redjet GC EmptyBox Timeout Check (RISC-V)", 30, "ialu_reg_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR:$is_expired, (redjet_emptybox_timeout GPR:$timeout_tsc))];
}

def RISCV_RedjetEmptyBoxTimeoutCheck_node : RISCV_RedjetEmptyBoxTimeoutCheck;

// ============================================================================
// Redjet RVV 1.0 Accelerated Barrier Instructions
// ============================================================================

// Redjet SimdDispatch_RVV10 — RVV 1.0 vector fused multiply-accumulate dispatch
class RISCV_RedjetSimdDispatch_RVV10 : GCBarrier<"vfmacc.vv\t$dst, $src1, $src2\t# redjet rvv 1.0 simd dispatch", "Redjet RVV 1.0 Vector Dispatch (RISC-V)", 40, "ialu_reg_mem"> {
  let BarrierType = 3;
  let Predicates = [HasStdExtV];
  list<dag> Pattern = [(set GPR:$dst, (redjet_simd_dispatch GPR:$dst, GPR:$src1, GPR:$src2))];
}

def RISCV_RedjetSimdDispatch_RVV10_node : RISCV_RedjetSimdDispatch_RVV10;

// Redjet ClearCacheline_RVV10 — 64-byte zeroing via RVV 1.0 unit-stride store
class RISCV_RedjetClearCacheline_RVV10 : GCBarrier<"vsetivli\tzero, 8, e64, m1, ta, ma\n\tvmv.v.i\tv0, 0\n\tvse64.v\tv0, ($mem)\t# redjet rvv cacheline clear", "Redjet RVV 1.0 Cacheline Clear (RISC-V)", 15, "ialu_mem_reg"> {
  let BarrierType = 3;
  let Predicates = [HasStdExtV];
  list<dag> Pattern = [(redjet_dead_ref_signal GPR:$mem, GPR:$mem)];
}

def RISCV_RedjetClearCacheline_RVV10_node : RISCV_RedjetClearCacheline_RVV10;

// ============================================================================
// End of RISCV_Redjet.td
// ============================================================================


