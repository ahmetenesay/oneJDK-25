//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// S390Glue.td - C2/GC Glue for IBM s390x (SystemZ)
//
// This file bridges LLVM TableGen SystemZ instruction definitions with
// Hotspot C2 compiler's MachNode-based instruction selection.
//
// Layer 1: Include LLVM SystemZ machine dictionary (transitive include of all 22+ .td files)
// Layer 2: C2 pattern matching - convert .ad match() patterns to TableGen Pat format
// Layer 3: GC barrier patterns (G1, Shenandoah, ZGC)

// ============================================================================
// Layer 1: LLVM SystemZ Machine Dictionary
// ============================================================================

// Include the main S390.td which transitively includes SystemZ.td
include "../S390.td"

// ============================================================================
// GC Barrier Base Class & HotSpot SDNodes
// ============================================================================

include "HotSpotSDNodes.td"

class GCBarrier<string asmstr, string desc, int c = 100, string pipe = "ialu_mem_reg">
    : Instruction {
  string Assembly = asmstr;
  string Description = desc;
  int Cost = c;
  string Pipeline = pipe;
  int BarrierType = 0;  // 0=G1, 1=Shenandoah, 2=ZGC
}

// ============================================================================
// Layer 2: C2 Pattern Matching
// ============================================================================
//
// These Pat definitions convert C2 Ideal Node patterns to LLVM SelectionDAG /
// MachNode patterns. Each Pat maps a C2 operation to a SystemZ instruction.
//
// C2 Ideal Node format:  match(Set dst (Op arg1 arg2))
// TableGen Pat format:   def : Pat<(Op arg1, arg2), (INSTR arg1, arg2)>;
//
// Reference: s390.ad contains instructions with match() patterns.
// This file connects them to TableGen instruction definitions.
//
// ============================================================================

// --------------------------------------------------------------------------
// Integer Load Instructions (s390.ad reference: loadB, loadUB, loadS, loadI, loadL)
// --------------------------------------------------------------------------
// instruct loadB(iRegI dst, memory mem)  => LGBR / LB
// instruct loadUB(iRegI dst, memory mem) => LLGC
// instruct loadS(iRegI dst, memory mem)  => LGHR / LH / LHY
// instruct loadUS(iRegI dst, memory mem) => LLGH
// instruct loadI(iRegI dst, memory mem)  => LGF / L / LY
// instruct loadL(iRegL dst, memory mem)  => LG

// --------------------------------------------------------------------------
// Integer Store Instructions (s390.ad reference: storeB, storeC, storeI, storeL)
// --------------------------------------------------------------------------
// instruct storeB(memory mem, iRegI src) => STC / STCY
// instruct storeC(memory mem, iRegI src) => STH / STHY
// instruct storeI(memory mem, iRegI src) => ST / STY
// instruct storeL(memory mem, iRegL src) => STG

// --------------------------------------------------------------------------
// 32-bit Integer ALU Instructions (addI, subI, mulI, divI, andI, orI, xorI, shifts)
// --------------------------------------------------------------------------
// instruct addI_reg_reg(iRegI dst, iRegI src1, iRegI src2) => AR / AY
// instruct subI_reg_reg(iRegI dst, iRegI src1, iRegI src2) => SR / SY
// instruct mulI_reg_reg(iRegI dst, iRegI src1, iRegI src2) => MSR / MS
// instruct andI_reg_reg(iRegI dst, iRegI src1, iRegI src2) => NR / NY
// instruct orI_reg_reg(iRegI dst, iRegI src1, iRegI src2)  => OR / OY
// instruct xorI_reg_reg(iRegI dst, iRegI src1, iRegI src2) => XR / XY
// instruct lshiftI_reg_reg(iRegI dst, iRegI src, iRegI shift) => SLLK
// instruct rshiftI_reg_reg(iRegI dst, iRegI src, iRegI shift) => SRAK
// instruct urshiftI_reg_reg(iRegI dst, iRegI src, iRegI shift) => SRLK

// --------------------------------------------------------------------------
// 64-bit Long ALU Instructions (addL, subL, mulL, divL, andL, orL, xorL, shifts)
// --------------------------------------------------------------------------
// instruct addL_reg_reg(iRegL dst, iRegL src1, iRegL src2) => AGR / AG
// instruct subL_reg_reg(iRegL dst, iRegL src1, iRegL src2) => SGR / SG
// instruct mulL_reg_reg(iRegL dst, iRegL src1, iRegL src2) => MSGR / MSG
// instruct andL_reg_reg(iRegL dst, iRegL src1, iRegL src2) => NGR / NG
// instruct orL_reg_reg(iRegL dst, iRegL src1, iRegL src2)  => OGR / OG
// instruct xorL_reg_reg(iRegL dst, iRegL src1, iRegL src2) => XGR / XG
// instruct lshiftL_reg_reg(iRegL dst, iRegL src, iRegI shift) => SLLG
// instruct rshiftL_reg_reg(iRegL dst, iRegL src, iRegI shift) => SRAG
// instruct urshiftL_reg_reg(iRegL dst, iRegL src, iRegI shift) => SRLG

// --------------------------------------------------------------------------
// Floating-Point & Double Instructions (FADD, FSUB, FMUL, FDIV)
// --------------------------------------------------------------------------
// instruct addF_reg_reg(regF dst, regF src1, regF src2) => AEBR
// instruct subF_reg_reg(regF dst, regF src1, regF src2) => SEBR
// instruct mulF_reg_reg(regF dst, regF src1, regF src2) => MEEBR
// instruct divF_reg_reg(regF dst, regF src1, regF src2) => DEBR
// instruct addD_reg_reg(regD dst, regD src1, regD src2) => ADBR
// instruct subD_reg_reg(regD dst, regD src1, regD src2) => SDBR
// instruct mulD_reg_reg(regD dst, regD src1, regD src2) => MDBR
// instruct divD_reg_reg(regD dst, regD src1, regD src2) => DDBR

// --------------------------------------------------------------------------
// Compare & Branch Instructions (cmpI, cmpL, cmpP, j, bne, beq, etc.)
// --------------------------------------------------------------------------
// instruct compI_reg_reg(flagsReg cr, iRegI op1, iRegI op2) => CR
// instruct compL_reg_reg(flagsReg cr, iRegL op1, iRegL op2) => CGR
// instruct compP_reg_reg(flagsReg cr, iRegP op1, iRegP op2) => CLGR
// instruct branch(label labl) => BRC / BRCL / J
// instruct branchCond(cmpOp cop, flagsReg cr, label labl) => BRC / BRCL

// ============================================================================
// Layer 3: GC Barrier Patterns
// ============================================================================
// Include the garbage collector specific barrier definitions for SystemZ:

include "../gc/S390_G1.td"
include "../gc/S390_Shenandoah.td"
include "../gc/S390_Z.td"

// ============================================================================
// End of S390Glue.td
// ============================================================================
