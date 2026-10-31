//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// S390_G1.td — G1 Garbage Collector Barrier Instructions for IBM s390x (SystemZ)
// ==============================================================================
// Converted from:
//   - src/hotspot/cpu/s390/gc/g1/g1_s390.ad
//
// This file defines G1 GC write barrier, CAS, and atomic instructions
// in LLVM TableGen format for IBM SystemZ (s390x).

// ============================================================================
// G1 Store Barriers (Pointer and Compressed OOP)
// ============================================================================

// G1 StoreP — Store a 64-bit object pointer with pre/post write barriers
// Converted from g1_s390.ad: instruct g1StoreP
class S390_G1StoreP : GCBarrier<"stg\t$src, 0($mem)\t# ptr", "G1 GC StoreP with write barriers (SystemZ)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$mem, (store_p GR64:$src))];
}

def S390_G1StoreP_node : S390_G1StoreP;

// G1 StoreN — Store a 32-bit compressed object pointer with pre/post write barriers
// Converted from g1_s390.ad: instruct g1StoreN
class S390_G1StoreN : GCBarrier<"sty\t$src, 0($mem)\t# narrow ptr", "G1 GC StoreN with write barriers (SystemZ)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$mem, (store_n GR32:$src))];
}

def S390_G1StoreN_node : S390_G1StoreN;

// G1 EncodePAndStoreN — Encode 64-bit pointer and store as 32-bit narrow OOP
// Converted from g1_s390.ad: instruct g1EncodePAndStoreN
class S390_G1EncodePAndStoreN : GCBarrier<"encode_heap_oop $src\n\tsty\t$src, 0($mem)\t# ptr", "G1 GC EncodeP and StoreN (SystemZ)", 125, "ialu_mem_reg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$mem, (store_n (encode_p GR64:$src)))];
}

def S390_G1EncodePAndStoreN_node : S390_G1EncodePAndStoreN;

// ============================================================================
// G1 CompareAndExchange (CAE) Barriers
// ============================================================================

// G1 CompareAndExchangeP — 64-bit pointer atomic compare-and-exchange
// Converted from g1_s390.ad: instruct g1CompareAndExchangeP
class S390_G1CompareAndExchangeP : GCBarrier<"csg\t$oldval, $newval, 0($mem)", "G1 GC CompareAndExchangeP (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$oldval, (g1_cae_p GR64:$mem, GR64:$oldval, GR64:$newval))];
}

def S390_G1CompareAndExchangeP_node : S390_G1CompareAndExchangeP;

// G1 CompareAndExchangeN — 32-bit compressed OOP atomic compare-and-exchange
// Converted from g1_s390.ad: instruct g1CompareAndExchangeN
class S390_G1CompareAndExchangeN : GCBarrier<"csy\t$oldval, $newval, 0($mem)", "G1 GC CompareAndExchangeN (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$oldval, (g1_cae_n GR64:$mem, GR32:$oldval, GR32:$newval))];
}

def S390_G1CompareAndExchangeN_node : S390_G1CompareAndExchangeN;

// ============================================================================
// G1 CompareAndSwap (CAS) Barriers
// ============================================================================

// G1 CompareAndSwapP — 64-bit pointer atomic compare-and-swap returning boolean
// Converted from g1_s390.ad: instruct g1CompareAndSwapP
class S390_G1CompareAndSwapP : GCBarrier<"csg\t$oldval, $newval, 0($mem)", "G1 GC CompareAndSwapP (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$res, (g1_cas_p GR64:$mem, GR64:$oldval, GR64:$newval))];
}

def S390_G1CompareAndSwapP_node : S390_G1CompareAndSwapP;

// G1 CompareAndSwapN — 32-bit compressed OOP atomic compare-and-swap returning boolean
// Converted from g1_s390.ad: instruct g1CompareAndSwapN
class S390_G1CompareAndSwapN : GCBarrier<"csy\t$oldval, $newval, 0($mem)", "G1 GC CompareAndSwapN (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$res, (g1_cas_n GR64:$mem, GR32:$oldval, GR32:$newval))];
}

def S390_G1CompareAndSwapN_node : S390_G1CompareAndSwapN;

// ============================================================================
// G1 Atomic Exchange (GetAndSet) Barriers
// ============================================================================

// G1 GetAndSetP — 64-bit pointer atomic exchange
// Converted from g1_s390.ad: instruct g1GetAndSetP
class S390_G1GetAndSetP : GCBarrier<"csg_loop\t$res, $newval, 0($mem)", "G1 GC GetAndSetP (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR64:$res, (g1_xchg_p GR64:$mem, GR64:$newval))];
}

def S390_G1GetAndSetP_node : S390_G1GetAndSetP;

// G1 GetAndSetN — 32-bit compressed OOP atomic exchange
// Converted from g1_s390.ad: instruct g1GetAndSetN
class S390_G1GetAndSetN : GCBarrier<"csy_loop\t$res, $newval, 0($mem)", "G1 GC GetAndSetN (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 0; // G1
  list<dag> Pattern = [(set GR32:$res, (g1_xchg_n GR64:$mem, GR32:$newval))];
}

def S390_G1GetAndSetN_node : S390_G1GetAndSetN;
