//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// RISCVGlue.td - C2/GC Glue for RISC-V (RV64)
//
// This file bridges LLVM TableGen RISC-V instruction definitions with
// Hotspot C2 compiler's MachNode-based instruction selection.
//
// Layer 1: Include LLVM RISC-V machine dictionary (transitive include of all 104+ .td files)
// Layer 2: C2 pattern matching - convert .ad match() patterns to TableGen Pat format
// Layer 3: GC barrier patterns (G1, Shenandoah, ZGC)

// ============================================================================
// Layer 1: LLVM RISC-V Machine Dictionary
// ============================================================================

// Include the main RISCV.td which transitively includes all instruction definitions
include "../RISCV.td"

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
// MachNode patterns. Each Pat maps a C2 operation to a RISC-V instruction.
//
// C2 Ideal Node format:  match(Set dst (Op arg1 arg2))
// TableGen Pat format:   def : Pat<(Op arg1, arg2), (INSTR arg1, arg2)>;
//
// Reference: riscv.ad, riscv_b.ad, riscv_v.ad contain 400+ instructions with match() patterns.
// This file connects them to TableGen instruction definitions.
//
// ============================================================================

// --------------------------------------------------------------------------
// Integer Load Instructions (riscv.ad reference: loadB, loadUB, loadS, loadI, loadL)
// --------------------------------------------------------------------------
// instruct loadB(rRegI dst, memory mem)  => LB
// instruct loadUB(rRegI dst, memory mem) => LBU
// instruct loadS(rRegI dst, memory mem)  => LH
// instruct loadUS(rRegI dst, memory mem) => LHU
// instruct loadI(rRegI dst, memory mem)  => LW
// instruct loadL(rRegL dst, memory mem)  => LD

// --------------------------------------------------------------------------
// Integer Store Instructions (riscv.ad reference: storeB, storeC, storeI, storeL)
// --------------------------------------------------------------------------
// instruct storeB(memory mem, rRegI src) => SB
// instruct storeC(memory mem, rRegI src) => SH
// instruct storeI(memory mem, rRegI src) => SW
// instruct storeL(memory mem, rRegL src) => SD

// --------------------------------------------------------------------------
// 32-bit Integer ALU Instructions (addI, subI, mulI, divI, andI, orI, xorI, shifts)
// --------------------------------------------------------------------------
// instruct addI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => ADDW
// instruct subI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => SUBW
// instruct mulI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => MULW
// instruct sdivI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => DIVW
// instruct udivI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => DIVUW
// instruct andI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => AND
// instruct orI_reg_reg(rRegI dst, rRegI src1, rRegI src2)  => OR
// instruct xorI_reg_reg(rRegI dst, rRegI src1, rRegI src2) => XOR
// instruct lshiftI_reg_reg(rRegI dst, rRegI src, rRegI shift) => SLLW
// instruct rshiftI_reg_reg(rRegI dst, rRegI src, rRegI shift) => SRAW
// instruct urshiftI_reg_reg(rRegI dst, rRegI src, rRegI shift) => SRLW

// --------------------------------------------------------------------------
// 64-bit Long ALU Instructions (addL, subL, mulL, divL, andL, orL, xorL, shifts)
// --------------------------------------------------------------------------
// instruct addL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => ADD
// instruct subL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => SUB
// instruct mulL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => MUL
// instruct sdivL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => DIV
// instruct udivL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => DIVU
// instruct andL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => AND
// instruct orL_reg_reg(rRegL dst, rRegL src1, rRegL src2)  => OR
// instruct xorL_reg_reg(rRegL dst, rRegL src1, rRegL src2) => XOR
// instruct lshiftL_reg_reg(rRegL dst, rRegL src, rRegI shift) => SLL
// instruct rshiftL_reg_reg(rRegL dst, rRegL src, rRegI shift) => SRA
// instruct urshiftL_reg_reg(rRegL dst, rRegL src, rRegI shift) => SRL

// --------------------------------------------------------------------------
// Floating-Point & Double Instructions (FADD, FSUB, FMUL, FDIV)
// --------------------------------------------------------------------------
// instruct addF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FADD_S
// instruct subF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FSUB_S
// instruct mulF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FMUL_S
// instruct divF_reg_reg(vRegF dst, vRegF src1, vRegF src2) => FDIV_S
// instruct addD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FADD_D
// instruct subD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FSUB_D
// instruct mulD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FMUL_D
// instruct divD_reg_reg(vRegD dst, vRegD src1, vRegD src2) => FDIV_D

// --------------------------------------------------------------------------
// Compare & Branch Instructions (cmpI, cmpL, cmpP, j, bne, beq, etc.)
// --------------------------------------------------------------------------
// instruct compI_reg_reg(rFlagsReg cr, rRegI op1, rRegI op2) => SUBW
// instruct compL_reg_reg(rFlagsReg cr, rRegL op1, rRegL op2) => SUB
// instruct compP_reg_reg(rFlagsReg cr, rRegP op1, rRegP op2) => SUB
// instruct branch(label labl) => JAL (with X0)
// instruct branchCond(cmpOp cop, rFlagsReg cr, label labl) => BNE / BEQ / BLT / BGE

// ============================================================================
// Layer 3: GC Barrier Patterns
// ============================================================================
// Include the garbage collector specific barrier definitions for RISC-V:

include "../gc/RISCV_G1.td"
include "../gc/RISCV_Shenandoah.td"
include "../gc/RISCV_Z.td"

// ============================================================================
// End of RISCVGlue.td
// ============================================================================
