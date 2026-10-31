//
// Copyright (c) 2026, oneJDK-25 project. All rights reserved.
//
// X86Glue.td - C2/GC Glue for x86_64
//
// This file bridges LLVM TableGen x86 instruction definitions with
// Hotspot C2 compiler's MachNode-based instruction selection.
//
// Layer 1: Include LLVM x86 machine dictionary (transitive include of all 60+ .td files)
// Layer 2: C2 pattern matching - convert .ad match() patterns to TableGen Pat format
// Layer 3: GC barrier patterns (G1, Shenandoah, ZGC)

// ============================================================================
// Layer 1: LLVM x86 Machine Dictionary
// ============================================================================

// Include the main X86.td which transitively includes:
//   - X86RegisterInfo.td & X86RegisterBanks.td
//   - X86Schedule.td & all CPU microarchitecture models (Alderlake, Haswell, Skylake,
//     Sapphire Rapids, Lunarlake, Znver1-4, Arrowlake, Pantherlake, Novalake)
//   - X86InstrInfo.td (which includes X86InstrArithmetic, SSE, AVX512, AVX10, etc.)
//   - X86CallingConv.td, X86Combine.td
include "../X86.td"

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
// MachNode patterns. Each Pat maps a C2 operation to an x86 instruction.
//
// C2 Ideal Node format:  match(Set dst (Op arg1 arg2))
// TableGen Pat format:   def : Pat<(Op arg1, arg2), (INSTR arg1, arg2)>;
//
// Reference: x86_64.ad and x86.ad files contain 1000+ instructions with match() patterns.
// This file connects them to TableGen instruction definitions.
//
// ============================================================================

// --------------------------------------------------------------------------
// Integer Load Instructions (x86_64.ad reference: loadB, loadUB, loadS, loadI, loadL)
// --------------------------------------------------------------------------
// instruct loadB(rRegI dst, memory mem)  => MOVSX32rm8
// instruct loadUB(rRegI dst, memory mem) => MOVZX32rm8
// instruct loadS(rRegI dst, memory mem)  => MOVSX32rm16
// instruct loadUS(rRegI dst, memory mem) => MOVZX32rm16
// instruct loadI(rRegI dst, memory mem)  => MOV32rm
// instruct loadL(rRegL dst, memory mem)  => MOV64rm

// --------------------------------------------------------------------------
// Integer Store Instructions (x86_64.ad reference: storeB, storeC, storeI, storeL)
// --------------------------------------------------------------------------
// instruct storeB(memory mem, rRegI src) => MOV8mr
// instruct storeC(memory mem, rRegI src) => MOV16mr
// instruct storeI(memory mem, rRegI src) => MOV32mr
// instruct storeL(memory mem, rRegL src) => MOV64mr

// --------------------------------------------------------------------------
// 32-bit Integer ALU Instructions (addI, subI, mulI, divI, andI, orI, xorI, shifts)
// --------------------------------------------------------------------------
// instruct addI_rReg(rRegI dst, rRegI src, rFlagsReg cr)  => ADD32rr
// instruct subI_rReg(rRegI dst, rRegI src, rFlagsReg cr)  => SUB32rr
// instruct mulI_rReg(rRegI dst, rRegI src, rFlagsReg cr)  => IMUL32rr
// instruct andI_rReg(rRegI dst, rRegI src, rFlagsReg cr)  => AND32rr
// instruct orI_rReg(rRegI dst, rRegI src, rFlagsReg cr)   => OR32rr
// instruct xorI_rReg(rRegI dst, rRegI src, rFlagsReg cr)  => XOR32rr
// instruct salI_rReg(rRegI dst, rcx_RegI shift)           => SHL32rCL
// instruct sarI_rReg(rRegI dst, rcx_RegI shift)           => SAR32rCL
// instruct shrI_rReg(rRegI dst, rcx_RegI shift)           => SHR32rCL

// --------------------------------------------------------------------------
// 64-bit Long ALU Instructions (addL, subL, mulL, divL, andL, orL, xorL, shifts)
// --------------------------------------------------------------------------
// instruct addL_rReg(rRegL dst, rRegL src, rFlagsReg cr)  => ADD64rr
// instruct subL_rReg(rRegL dst, rRegL src, rFlagsReg cr)  => SUB64rr
// instruct mulL_rReg(rRegL dst, rRegL src, rFlagsReg cr)  => IMUL64rr
// instruct andL_rReg(rRegL dst, rRegL src, rFlagsReg cr)  => AND64rr
// instruct orL_rReg(rRegL dst, rRegL src, rFlagsReg cr)   => OR64rr
// instruct xorL_rReg(rRegL dst, rRegL src, rFlagsReg cr)  => XOR64rr
// instruct salL_rReg(rRegL dst, rcx_RegI shift)           => SHL64rCL
// instruct sarL_rReg(rRegL dst, rcx_RegI shift)           => SAR64rCL
// instruct shrL_rReg(rRegL dst, rcx_RegI shift)           => SHR64rCL

// --------------------------------------------------------------------------
// Floating-Point & Double Instructions (SSE/AVX: addF, subF, mulF, divF, etc.)
// --------------------------------------------------------------------------
// instruct addF_reg(regF dst, regF src) => ADDSSrr / VADDSSrr
// instruct subF_reg(regF dst, regF src) => SUBSSrr / VSUBSSrr
// instruct mulF_reg(regF dst, regF src) => MULSSrr / VMULSSrr
// instruct divF_reg(regF dst, regF src) => DIVSSrr / VDIVSSrr
// instruct addD_reg(regD dst, regD src) => ADDSDrr / VADDSDrr
// instruct subD_reg(regD dst, regD src) => SUBSDrr / VSUBSDrr
// instruct mulD_reg(regD dst, regD src) => MULSDrr / VMULSDrr
// instruct divD_reg(regD dst, regD src) => DIVSDrr / VDIVSDrr

// --------------------------------------------------------------------------
// Compare & Branch Instructions (cmpI, cmpL, cmpP, jmp, jcc)
// --------------------------------------------------------------------------
// instruct compI_rReg(rFlagsReg cr, rRegI op1, rRegI op2) => CMP32rr
// instruct compL_rReg(rFlagsReg cr, rRegL op1, rRegL op2) => CMP64rr
// instruct compP_rReg(rFlagsReg cr, rRegP op1, rRegP op2) => CMP64rr
// instruct jmpLoopEnd(cmpOp cop, rFlagsReg cr, label labl) => JCC (conditional jump)

// ============================================================================
// Layer 3: GC Barrier Patterns
// ============================================================================
// Include the garbage collector specific barrier definitions for x86_64:

include "../gc/X86_G1.td"
include "../gc/X86_Shenandoah.td"
include "../gc/X86_Z.td"

// ============================================================================
// End of X86Glue.td
// ============================================================================
