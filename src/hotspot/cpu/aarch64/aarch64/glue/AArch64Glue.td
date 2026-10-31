//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// AArch64Glue.td - C2/GC Glue for AArch64 (ARM64)
//
// This file bridges LLVM TableGen AArch64 instruction definitions with
// Hotspot C2 compiler's MachNode-based instruction selection.
//
// Layer 1: Include LLVM AArch64 machine dictionary (transitive include of all 59+ .td files)
// Layer 2: C2 pattern matching - convert .ad match() patterns to TableGen Pat format
// Layer 3: GC barrier patterns (G1, Shenandoah, ZGC)

// ============================================================================
// Layer 1: LLVM AArch64 Machine Dictionary
// ============================================================================

// Include the main AArch64.td which transitively includes all instruction definitions
include "../AArch64.td"

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
// MachNode patterns. Each Pat maps a C2 operation to an AArch64 instruction.
//
// C2 Ideal Node format:  match(Set dst (Op arg1 arg2))
// TableGen Pat format:   def : Pat<(Op arg1, arg2), (INSTR arg1, arg2)>;
//
// Reference: aarch64.ad and aarch64_vector.ad files contain 650+ instructions with match() patterns.
// This file connects them to TableGen instruction definitions.
//
// ============================================================================

// --------------------------------------------------------------------------
// Integer Load Instructions (aarch64.ad reference: loadB, loadUB, loadS, loadI, loadL)
// --------------------------------------------------------------------------
// instruct loadB(rRegI dst, memory mem)  => LDRSBWroX / LDRSBWui
// instruct loadUB(rRegI dst, memory mem) => LDRBBroX / LDRBBui
// instruct loadS(rRegI dst, memory mem)  => LDRSHWroX / LDRSHWui
// instruct loadUS(rRegI dst, memory mem) => LDRHHroX / LDRHHui
// instruct loadI(rRegI dst, memory mem)  => LDRWroX / LDRWui
// instruct loadL(rRegL dst, memory mem)  => LDRXroX / LDRXui

// --------------------------------------------------------------------------
// Integer Store Instructions (aarch64.ad reference: storeB, storeC, storeI, storeL)
// --------------------------------------------------------------------------
// instruct storeB(memory mem, rRegI src) => STRBBroX / STRBBui
// instruct storeC(memory mem, rRegI src) => STRHHroX / STRHHui
// instruct storeI(memory mem, rRegI src) => STRWroX / STRWui
// instruct storeL(memory mem, rRegL src) => STRXroX / STRXui

// --------------------------------------------------------------------------
// 32-bit Integer ALU Instructions (addI, subI, mulI, divI, andI, orI, xorI, shifts)
// --------------------------------------------------------------------------
// instruct addI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => ADDWrr
// instruct subI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => SUBWrr
// instruct mulI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => MADDWrrr (with WZR)
// instruct sdivI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => SDIVWr
// instruct udivI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => UDIVWr
// instruct andI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => ANDWrr
// instruct orI_reg_reg(rRegI dst, rRegI src1, rRegI src2)  => ORRWrr
// instruct xorI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => EORWrr
// instruct lshiftI_reg_reg(rRegI dst, rRegI src, rRegI shift) => LSLVWr
// instruct rshiftI_reg_reg(rRegI dst, rRegI src, rRegI shift) => ASRVWr
// instruct urshiftI_reg_reg(rRegI dst, rRegI src, rRegI shift) => LSRVWr

// --------------------------------------------------------------------------
// 64-bit Long ALU Instructions (addL, subL, mulL, divL, andL, orL, xorL, shifts)
// --------------------------------------------------------------------------
// instruct addL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => ADDXrr
// instruct subL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => SUBXrr
// instruct mulL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => MADDXrrr (with XZR)
// instruct sdivL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => SDIVXr
// instruct udivL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => UDIVXr
// instruct andL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => ANDXrr
// instruct orL_reg_reg(rRegL dst, rRegL src1, rRegL src2)  => ORRXrr
// instruct xorL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => EORXrr
// instruct lshiftL_reg_reg(rRegL dst, rRegL src, rRegI shift) => LSLVXr
// instruct rshiftL_reg_reg(rRegL dst, rRegL src, rRegI shift) => ASRVXr
// instruct urshiftL_reg_reg(rRegL dst, rRegL src, rRegI shift) => LSRVXr

// --------------------------------------------------------------------------
// Floating-Point & Double Instructions (FADD, FSUB, FMUL, FDIV)
// --------------------------------------------------------------------------
// instruct addF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FADDSrr
// instruct subF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FSUBSrr
// instruct mulF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FMULSrr
// instruct divF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FDIVSrr
// instruct addD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FADDDrr
// instruct subD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => SUBSDrr
// instruct mulD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FMULDrr
// instruct divD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FDIVDrr

// --------------------------------------------------------------------------
// Compare & Branch Instructions (cmpI, cmpL, cmpP, b, b.cond)
// --------------------------------------------------------------------------
// instruct compI_reg_reg(rFlagsReg cr, rRegI op1, rRegI op2) => SUBSWrr (with WZR)
// instruct compL_reg_reg(rFlagsReg cr, rRegL op1, rRegL op2) => SUBSXrr (with XZR)
// instruct compP_reg_reg(rFlagsReg cr, rRegP op1, rRegP op2) => SUBSXrr (with XZR)
// instruct branch(label labl) => B
// instruct branchCond(cmpOp cop, rFlagsReg cr, label labl) => Bcc

// ============================================================================
// Layer 3: GC Barrier Patterns
// ============================================================================
// Include the garbage collector specific barrier definitions for AArch64:

include "../gc/AArch64_G1.td"
include "../gc/AArch64_Shenandoah.td"
include "../gc/AArch64_Z.td"

// ============================================================================
// End of AArch64Glue.td
// ============================================================================
