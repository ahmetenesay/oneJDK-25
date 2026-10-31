//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// S390_Shenandoah.td — Shenandoah GC Barrier Definitions for IBM s390x (SystemZ)
// ==============================================================================
// Note: Shenandoah is not officially supported upstream on s390x, but these
// TableGen definitions provide architecture parity with x86, AArch64, and RISC-V.

class S390_ShenandoahCAS_P : GCBarrier<"csg\t$oldval, $newval, 0($mem_ptr)", "Shenandoah CompareAndSwapP (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GR32:$res, (shenandoah_cas_p GR64:$mem_ptr, GR64:$oldval, GR64:$newval))];
}

def S390_ShenandoahCAS_P_node : S390_ShenandoahCAS_P;

class S390_ShenandoahCAS_N : GCBarrier<"csy\t$oldval, $newval, 0($mem_ptr)", "Shenandoah CompareAndSwapN (SystemZ)", 100, "pipe_cmpxchg"> {
  let BarrierType = 1; // Shenandoah
  list<dag> Pattern = [(set GR32:$res, (shenandoah_cas_n GR64:$mem_ptr, GR32:$oldval, GR32:$newval))];
}

def S390_ShenandoahCAS_N_node : S390_ShenandoahCAS_N;
