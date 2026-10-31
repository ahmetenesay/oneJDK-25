//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// S390_Z.td — ZGC Barrier Definitions for IBM s390x (SystemZ)
// =============================================================
// Note: ZGC is not officially supported upstream on s390x, but these
// TableGen definitions provide architecture parity with x86, AArch64, and RISC-V.

class S390_ZLoadP : GCBarrier<"lg\t$dst, 0($mem)\t# zload", "ZGC Load Barrier (SystemZ)", 125, "ialu_reg_mem"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GR64:$dst, (z_load_p GR64:$mem))];
}

def S390_ZLoadP_node : S390_ZLoadP;

class S390_ZStoreP : GCBarrier<"stg\t$src, 0($mem)\t# zstore", "ZGC Store Barrier (SystemZ)", 125, "ialu_mem_reg"> {
  let BarrierType = 2; // ZGC
  list<dag> Pattern = [(set GR64:$mem, (z_store_p GR64:$src))];
}

def S390_ZStoreP_node : S390_ZStoreP;
