//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// AArch64_Redjet.td — Redjet Garbage Collector Barrier Instructions for AArch64 (ARM64)
// ====================================================================================
// This file defines Redjet GC load, store, CAS, CAE, and atomic instructions
// in LLVM TableGen format for AArch64 architecture.

// ============================================================================
// Redjet Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// Redjet StoreP — Store a 64-bit object pointer with Redjet write barriers
class AArch64_RedjetStoreP : GCBarrier<"str\t$src, [$mem]\t# redjet store ptr", "Redjet GC StoreP with write barriers (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64sp:$mem, (redjet_store_p GPR64:$src))];
}

def AArch64_RedjetStoreP_node : AArch64_RedjetStoreP;
def AArch64_RedjetStorePVolatile_node : AArch64_RedjetStoreP;

// Redjet StoreN — Store a 32-bit compressed object pointer with Redjet write barriers
class AArch64_RedjetStoreN : GCBarrier<"str\t$src, [$mem]\t# redjet narrow ptr", "Redjet GC StoreN with write barriers (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64sp:$mem, (redjet_store_n GPR32:$src))];
}

def AArch64_RedjetStoreN_node : AArch64_RedjetStoreN;
def AArch64_RedjetStoreNVolatile_node : AArch64_RedjetStoreN;

// Redjet EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
class AArch64_RedjetEncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tstr\t$src, [$mem]\t# redjet narrow ptr", "Redjet GC EncodeP and StoreN (AArch64)", 125, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64sp:$mem, (redjet_store_n (redjet_encode_p GPR64:$src)))];
}

def AArch64_RedjetEncodePAndStoreN_node : AArch64_RedjetEncodePAndStoreN;
def AArch64_RedjetEncodePAndStoreNVolatile_node : AArch64_RedjetEncodePAndStoreN;

// ============================================================================
// Redjet Load Barriers (Concurrent Tracing & Reference Resolution)
// ============================================================================

// Redjet LoadP — Load 64-bit object pointer with Redjet load barrier
class AArch64_RedjetLoadP : GCBarrier<"ldr\t$dst, [$mem]\t# redjet load", "Redjet GC Load Barrier (AArch64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64:$dst, (redjet_load_p GPR64sp:$mem))];
}

def AArch64_RedjetLoadP_node : AArch64_RedjetLoadP;
def AArch64_RedjetLoadPVolatile_node : AArch64_RedjetLoadP;

// Redjet LoadN — Load 32-bit compressed OOP with Redjet load barrier
class AArch64_RedjetLoadN : GCBarrier<"ldr\t$dst, [$mem]\t# redjet narrow load", "Redjet GC Narrow Load Barrier (AArch64)", 125, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$dst, (redjet_load_n GPR64sp:$mem))];
}

def AArch64_RedjetLoadN_node : AArch64_RedjetLoadN;
def AArch64_RedjetLoadNVolatile_node : AArch64_RedjetLoadN;

// ============================================================================
// Redjet CompareAndExchange (CAE) Barriers
// ============================================================================

// Redjet CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange with barriers
class AArch64_RedjetCompareAndExchangeP : GCBarrier<"casal\t$oldval, $newval, [$mem]\t# redjet cae", "Redjet GC CompareAndExchangeP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64:$oldval, (redjet_cae_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_RedjetCompareAndExchangeP_node : AArch64_RedjetCompareAndExchangeP;
def AArch64_RedjetCompareAndExchangePAcq_node : AArch64_RedjetCompareAndExchangeP;

// Redjet CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
class AArch64_RedjetCompareAndExchangeN : GCBarrier<"casal\t$oldval, $newval, [$mem]\t# redjet cae", "Redjet GC CompareAndExchangeN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$oldval, (redjet_cae_n GPR64sp:$mem, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_RedjetCompareAndExchangeN_node : AArch64_RedjetCompareAndExchangeN;
def AArch64_RedjetCompareAndExchangeNAcq_node : AArch64_RedjetCompareAndExchangeN;

// ============================================================================
// Redjet CompareAndSwap (CAS) Barriers
// ============================================================================

// Redjet CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
class AArch64_RedjetCompareAndSwapP : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq\t# redjet cas", "Redjet GC CompareAndSwapP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$res, (redjet_cas_p GPR64sp:$mem, GPR64:$oldval, GPR64:$newval))];
}

def AArch64_RedjetCompareAndSwapP_node : AArch64_RedjetCompareAndSwapP;
def AArch64_RedjetCompareAndSwapPAcq_node : AArch64_RedjetCompareAndSwapP;
def AArch64_RedjetWeakCompareAndSwapP_node : AArch64_RedjetCompareAndSwapP;

// Redjet CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
class AArch64_RedjetCompareAndSwapN : GCBarrier<"cas\t$oldval, $newval, [$mem]\n\tcset\t$res, eq\t# redjet cas", "Redjet GC CompareAndSwapN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$res, (redjet_cas_n GPR64sp:$mem, GPR32:$oldval, GPR32:$newval))];
}

def AArch64_RedjetCompareAndSwapN_node : AArch64_RedjetCompareAndSwapN;
def AArch64_RedjetCompareAndSwapNAcq_node : AArch64_RedjetCompareAndSwapN;
def AArch64_RedjetWeakCompareAndSwapN_node : AArch64_RedjetCompareAndSwapN;

// ============================================================================
// Redjet Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// Redjet GetAndSetP — 64-bit pointer atomic exchange
class AArch64_RedjetGetAndSetP : GCBarrier<"swpal\t$newval, $res, [$mem]\t# redjet xchg", "Redjet GC GetAndSetP (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64:$res, (redjet_xchg_p GPR64sp:$mem, GPR64:$newval))];
}

def AArch64_RedjetGetAndSetP_node : AArch64_RedjetGetAndSetP;

// Redjet GetAndSetN — 32-bit compressed OOP atomic exchange
class AArch64_RedjetGetAndSetN : GCBarrier<"swpal\t$newval, $res, [$mem]\t# redjet xchg", "Redjet GC GetAndSetN (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$res, (redjet_xchg_n GPR64sp:$mem, GPR32:$newval))];
}

def AArch64_RedjetGetAndSetN_node : AArch64_RedjetGetAndSetN;

// ============================================================================
// Redjet 4-Point Coordination & SIMD Dispatch Instructions (AArch64 / SVE2)
// ============================================================================

// Redjet SafepointPoll — Check global STW safepoint active flag
class AArch64_RedjetSafepointPoll : GCBarrier<"ldr\twzr, [$mem]\t# redjet safepoint poll", "Redjet GC Safepoint Poll (AArch64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_safepoint_poll GPR64sp:$mem)];
}

def AArch64_RedjetSafepointPoll_node : AArch64_RedjetSafepointPoll;

// Redjet PausepointPoll — Non-STW Pitstop ISA hot-swap gate poll
class AArch64_RedjetPausepointPoll : GCBarrier<"ldar\twzr, [$mem]\t# redjet pausepoint poll", "Redjet GC Pausepoint Poll (AArch64)", 40, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_pausepoint_poll GPR64sp:$mem)];
}

def AArch64_RedjetPausepointPoll_node : AArch64_RedjetPausepointPoll;

// Redjet HookpointPin — Atomic pin increment for JNI/FFM/SIMD critical region
class AArch64_RedjetHookpointPin : GCBarrier<"ldaddal\t$delta, $res, [$mem]\t# redjet hookpoint pin", "Redjet GC Hookpoint Pin (AArch64)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$res, (redjet_hookpoint_pin GPR64sp:$mem, GPR32:$delta))];
}

def AArch64_RedjetHookpointPin_node : AArch64_RedjetHookpointPin;

// Redjet ControlpointCheck — Host GC arbitration state load & test
class AArch64_RedjetControlpointCheck : GCBarrier<"ldar\t$res, [$mem]\t# redjet controlpoint check", "Redjet GC Controlpoint Check (AArch64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$res, (redjet_controlpoint_chk GPR64sp:$mem))];
}

def AArch64_RedjetControlpointCheck_node : AArch64_RedjetControlpointCheck;

// Redjet SimdDispatch — SVE2 / NEON predicated fused multiply-add vector dispatch
class AArch64_RedjetSimdDispatch : GCBarrier<"fmla\t$dst.d, p0/m, $src1.d, $src2.d\t# redjet sve2 simd dispatch", "Redjet SVE2 SIMD Dispatch (AArch64)", 60, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64:$dst, (redjet_simd_dispatch GPR64:$dst, GPR64:$src1, GPR64:$src2))];
}

def AArch64_RedjetSimdDispatch_node : AArch64_RedjetSimdDispatch;

// ============================================================================
// Redjet 2-Point Zero-STW Hardware Sync & Universal MachPollPoint
// ============================================================================

// Redjet Dockpoint — Method entry 16-byte trampoline to Slot 16 target
class AArch64_RedjetDockpoint : GCBarrier<"b\t$slot_target\t# redjet dockpoint slot16 trampoline", "Redjet GC Dockpoint Method Entry Trampoline (AArch64)", 10, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dockpoint bb:$slot_target)];
}

def AArch64_RedjetDockpoint_node : AArch64_RedjetDockpoint;

// Redjet Parkpoint — Loop bottom 0.5-cycle L1 D-Cache thread sync check
class AArch64_RedjetParkpoint : GCBarrier<"tbnz\t$thread_reg, #$park_bit, $cold_park_and_swap\t# redjet parkpoint", "Redjet GC Parkpoint Loop Poll (AArch64)", 20, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_parkpoint GPR64:$thread_reg, i32imm:$park_bit, bb:$cold_park_and_swap)];
}

def AArch64_RedjetParkpoint_node : AArch64_RedjetParkpoint;

// Redjet MachPollPoint — Universal hardware poll point for Redjet, G1, Shenandoah, and ZGC
class AArch64_MachPollPoint : GCBarrier<"ldr\twzr, [$safepoint_poll_addr]\t# MachPollPoint", "HotSpot Universal MachPollPoint (AArch64)", 50, "ialu_reg_mem"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(mach_poll_point GPR64sp:$safepoint_poll_addr, i32imm:$poll_type)];
}

def AArch64_MachPollPoint_node : AArch64_MachPollPoint;

// ============================================================================
// Redjet TailTraces Clasp Dead Reference Signal & Off-Heap Eviction
// ============================================================================

// Redjet DeadReferenceSignal — Clasp emission to RPSS ring buffer (64-bit)
class AArch64_RedjetDeadRefSignal : GCBarrier<"str\t$dead_oop, [$rpss_addr]\t# redjet tailtraces signal", "Redjet TailTraces Clasp Dead Reference Signal (AArch64)", 15, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dead_ref_signal GPR64sp:$rpss_addr, GPR64:$dead_oop)];
}

def AArch64_RedjetDeadRefSignal_node : AArch64_RedjetDeadRefSignal;
def AArch64_RedjetDeadRefSignalVolatile_node : AArch64_RedjetDeadRefSignal;

// Redjet DeadReferenceSignalN — Narrow 32-bit compressed OOP clasp emission
class AArch64_RedjetDeadRefSignalN : GCBarrier<"str\t$dead_oop, [$rpss_addr]\t# redjet tailtraces narrow signal", "Redjet TailTraces Clasp Narrow Dead Reference Signal (AArch64)", 15, "ialu_mem_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_dead_ref_signal_n GPR64sp:$rpss_addr, GPR32:$dead_oop)];
}

def AArch64_RedjetDeadRefSignalN_node : AArch64_RedjetDeadRefSignalN;

// Redjet DeadReferenceSignalAtomic — Atomic exchange clasp emission
class AArch64_RedjetDeadRefSignalAtomic : GCBarrier<"swpal\t$dead_oop, $res, [$rpss_addr]\t# redjet tailtraces atomic signal", "Redjet TailTraces Atomic Clasp Signal (AArch64)", 80, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64:$res, (redjet_dead_ref_signal_atomic GPR64sp:$rpss_addr, GPR64:$dead_oop))];
}

def AArch64_RedjetDeadRefSignalAtomic_node : AArch64_RedjetDeadRefSignalAtomic;

// Redjet OffHeapEvict — Deterministic RAII eviction for @RJGC(Mark = "ImmediateEviction")
class AArch64_RedjetOffHeapEvict : GCBarrier<"bl\tredjet_offheap_evict\t# redjet offheap evict", "Redjet Off-Heap Immediate Eviction Barrier (AArch64)", 40, "call"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_offheap_evict GPR64:$handle_ptr)];
}

def AArch64_RedjetOffHeapEvict_node : AArch64_RedjetOffHeapEvict;

// ============================================================================
// Redjet EmptyBox Guard (Phantom Slot & Tombstone Hardware Protection)
// Magic: 0xEB0C5107
// ============================================================================

// Redjet EmptyBoxGuard — Tests slot header against HS_EMPTYBOX_MAGIC (0xEB0C5107)
class AArch64_RedjetEmptyBoxGuard : GCBarrier<"ldr\tw16, [$mem]\n\tmov\tw17, #0x5107\n\tmovk\tw17, #0xEB0C, lsl #16\n\tcmp\tw16, w17\t# redjet emptybox guard", "Redjet GC EmptyBox Guard Test (AArch64)", 15, "ialu_mem_imm"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_emptybox_guard GPR64sp:$mem)];
}

def AArch64_RedjetEmptyBoxGuard_node : AArch64_RedjetEmptyBoxGuard;

// Redjet EmptyBoxGuardBranch — Tests magic and branches if slot is a protected tombstone
class AArch64_RedjetEmptyBoxGuardBranch : GCBarrier<"ldr\tw16, [$mem]\n\tmov\tw17, #0x5107\n\tmovk\tw17, #0xEB0C, lsl #16\n\tcmp\tw16, w17\n\tb.eq\t$parked_protect\t# redjet emptybox branch", "Redjet GC EmptyBox Branch Guard (AArch64)", 20, "branch"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(redjet_emptybox_guard_br GPR64sp:$mem, bb:$parked_protect)];
}

def AArch64_RedjetEmptyBoxGuardBranch_node : AArch64_RedjetEmptyBoxGuardBranch;

// Redjet EmptyBoxClaim — Atomic compare-and-exchange to reclaim parked slot
class AArch64_RedjetEmptyBoxClaim : GCBarrier<"casal\t$expected, $new_oop, [$mem]\t# redjet emptybox claim", "Redjet GC EmptyBox Atomic Claim (AArch64)", 100, "pipe_cmpxchg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR64:$expected, (redjet_emptybox_claim GPR64sp:$mem, GPR64:$expected, GPR64:$new_oop))];
}

def AArch64_RedjetEmptyBoxClaim_node : AArch64_RedjetEmptyBoxClaim;

// Redjet EmptyBoxTimeoutCheck — Compares current timestamp against timeout deadline
class AArch64_RedjetEmptyBoxTimeoutCheck : GCBarrier<"mrs\tx16, cntvct_el0\n\tcmp\tx16, $timeout_tsc\n\tcset\t$is_expired, ge\t# redjet emptybox timeout", "Redjet GC EmptyBox Timeout Check (AArch64)", 30, "ialu_reg_reg"> {
  let BarrierType = 3; // Redjet
  list<dag> Pattern = [(set GPR32:$is_expired, (redjet_emptybox_timeout GPR64:$timeout_tsc))];
}

def AArch64_RedjetEmptyBoxTimeoutCheck_node : AArch64_RedjetEmptyBoxTimeoutCheck;

// ============================================================================
// Redjet SVE2 Accelerated Barrier Instructions
// ============================================================================

// Redjet SimdDispatch_SVE2 — SVE2 multi-vector non-widening fused multiply-add dispatch
class AArch64_RedjetSimdDispatch_SVE2 : GCBarrier<"fmla\t$dst.d, p0/m, $src1.d, $src2.d\t# redjet sve2 simd dispatch", "Redjet SVE2 Full Vector Dispatch (AArch64)", 40, "ialu_reg_mem"> {
  let BarrierType = 3;
  let Predicates = [HasSVE2];
  list<dag> Pattern = [(set GPR64:$dst, (redjet_simd_dispatch GPR64:$dst, GPR64:$src1, GPR64:$src2))];
}

def AArch64_RedjetSimdDispatch_SVE2_node : AArch64_RedjetSimdDispatch_SVE2;

// Redjet ClearCacheline_SVE2 — 64-byte zeroing via DC ZVA or SVE vector clear
class AArch64_RedjetClearCacheline_SVE2 : GCBarrier<"dc\tzva, $mem\t# redjet sve2 cacheline clear", "Redjet SVE2 / DC ZVA Cacheline Zero (AArch64)", 15, "ialu_mem_reg"> {
  let BarrierType = 3;
  let Predicates = [HasSVE2];
  list<dag> Pattern = [(redjet_dead_ref_signal GPR64sp:$mem, GPR64:$mem)];
}

def AArch64_RedjetClearCacheline_SVE2_node : AArch64_RedjetClearCacheline_SVE2;

// ============================================================================
// End of AArch64_Redjet.td
// ============================================================================


