/*
 * Copyright (c) oneJDK-25 contributors. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef SHARE_OPTO_DAG_BUILDER_HPP
#define SHARE_OPTO_DAG_BUILDER_HPP

// ============================================================================
// dag_builder.hpp — C2 Ideal Node → LLVM SelectionDAG Converter
// ============================================================================
//
// This file defines the DAGBuilder class, which converts C2's Ideal Node
// graph into LLVM SelectionDAG nodes. It is the bridge between C2's
// high-level IR (Ideal Graph) and LLVM's low-level IR (SelectionDAG).
//
// Pipeline:
//   Bytecode → Ideal Node graph (C2, unchanged)
//       ↓
//   dag_builder.cpp: Ideal Node → SelectionDAG nodes (THIS FILE)
//       ↓
//   C2SelectionDAG.cpp: SelectionDAG wrapper (GC barrier, safepoint polling)
//       ↓
//   X86DAGISel.cpp: tablegen pattern matching → X86 instructions
//       ↓
//   C2ScheduleDAG.cpp: instruction scheduling
//       ↓
//   C2RegAlloc.cpp: register allocation
//       ↓
//   C2CodeEmitter.cpp: MachineInstr → machine code (MacroAssembler)
//       ↓
//   Machine code
//
// ============================================================================

#include "globals.hpp"
#include "opto/node.hpp"
#include "opto/opcodes.hpp"

// Forward declarations
class C2SelectionDAG;
class PhaseCFG;

// ============================================================================
// C2NodeType — C2 Ideal Node type to SelectionDAG node type mapping
// ============================================================================
//
// Each C2 Ideal Node opcode maps to a SelectionDAG node type.
// This enum is used by the DAGBuilder to create the correct SelectionDAG node.
//
namespace C2NodeTypes {

enum C2NodeType {
  // --- Control flow ---
  C2_BLOCK_END,           // Block end (phi points)
  C2_IF,                  // Conditional branch
  C2_STOP,                // Return/stop
  C2_CATCH_END,           // Exception catch

  // --- Integer operations ---
  C2_ADD_I,               // int add
  C2_SUB_I,               // int sub
  C2_MUL_I,               // int mul
  C2_DIV_I,               // int div (signed)
  C2_UDIV_I,              // int div (unsigned)
  C2_REM_I,               // int rem (signed)
  C2_UREM_I,              // int rem (unsigned)
  C2_AND_I,               // int and
  C2_OR_I,                // int or
  C2_XOR_I,               // int xor
  C2_LSHIFT_I,            // int shl
  C2_RSHIFT_I,            // int sar (signed)
  C2.URSHIFT_I,           // int shr (unsigned)

  // --- Long operations ---
  C2_ADD_L,               // long add
  C2_SUB_L,               // long sub
  C2_MUL_L,               // long mul
  C2_DIV_L,               // long div (signed)
  C2_UDIV_L,              // long div (unsigned)
  C2_REM_L,               // long rem (signed)
  C2_UREM_L,              // long rem (unsigned)
  C2_AND_L,               // long and
  C2_OR_L,                // long or
  C2_XOR_L,               // long xor
  C2_LSHIFT_L,            // long shl
  C2_RSHIFT_L,            // long sar (signed)
  C2.URSHIFT_L,           // long shr (unsigned)

  // --- Float operations ---
  C2_ADD_F,               // float add
  C2_SUB_F,               // float sub
  C2_MUL_F,               // float mul
  C2_DIV_F,               // float div
  C2_REM_F,               // float rem
  C2_NEG_F,               // float neg
  C2_SQRT_F,              // float sqrt

  // --- Double operations ---
  C2_ADD_D,               // double add
  C2_SUB_D,               // double sub
  C2_MUL_D,               // double mul
  C2_DIV_D,               // double div
  C2_REM_D,               // double rem
  C2_NEG_D,               // double neg
  C2_SQRT_D,              // double sqrt

  // --- Load operations ---
  C2_LOAD_B,              // byte load (signed)
  C2_LOAD_UB,             // byte load (unsigned)
  C2_LOAD_S,              // short load (signed)
  C2_LOAD_US,             // short load (unsigned)
  C2_LOAD_I,              // int load
  C2_LOAD_L,              // long load
  C2_LOAD_F,              // float load
  C2_LOAD_D,              // double load
  C2_LOAD_P,              // pointer load

  // --- Store operations ---
  C2_STORE_B,             // byte store
  C2_STORE_S,             // short store
  C2_STORE_I,             // int store
  C2_STORE_L,             // long store
  C2_STORE_F,             // float store
  C2_STORE_D,             // double store
  C2_STORE_P,             // pointer store

  // --- Convert operations ---
  C2_I2L,                 // int → long
  C2_I2F,                 // int → float
  C2_I2D,                 // int → double
  C2_L2I,                 // long → int
  C2_L2F,                 // long → float
  C2_L2D,                 // long → double
  C2_F2I,                 // float → int
  C2_F2L,                 // float → long
  C2_F2D,                 // float → double
  C2_D2I,                 // double → int
  C2_D2L,                 // double → long
  C2_D2F,                 // double → float

  // --- Compare operations ---
  C2_CMP_I,               // int compare
  C2_CMP_L,               // long compare
  C2_CMP_F,               // float compare
  C2_CMP_D,               // double compare
  C2_CMP_P,               // pointer compare

  // --- SIMD operations (SSE/AVX) ---
  C2_ADD_FVEC,            // float vector add
  C2_ADD_DVEC,            // double vector add
  C2_MUL_FVEC,            // float vector mul
  C2_MUL_DVEC,            // double vector mul
  C2_SUB_FVEC,            // float vector sub
  C2_SUB_DVEC,            // double vector sub
  C2_DIV_FVEC,            // float vector div
  C2_DIV_DVEC,            // double vector div

  // --- GC barrier operations ---
  C2_GC_BARRIER_LOAD,     // GC barrier load
  C2_GC_BARRIER_STORE,    // GC barrier store
  C2_GC_LOAD_P,           // G1 load barrier
  C2_GC_STORE_P,          // G1 store barrier

  // --- Special operations ---
  C2_CONSTANT,            // Constant value
  C2_MEMORY,              // Memory dependency
  C2_PROJ,                // Projection (from multi-result nodes)
  C2_REGION,              // Region (merge point)
  C2_IME,                 // Ideal Memory Entity
  C2_UNKNOWN              // Unknown node type
};

} // namespace C2NodeTypes

// ============================================================================
// DAGBuilder — Ideal Node → SelectionDAG Converter
// ============================================================================
//
// The DAGBuilder traverses C2's Ideal Node graph and creates corresponding
// SelectionDAG nodes. It maintains a mapping from Ideal Node* to SelectionDAG
// SDValue to avoid duplicate conversions.
//
class DAGBuilder : public ResourceObj {
private:
  C2SelectionDAG* _dag;           // Target SelectionDAG
  PhaseCFG* _cfg;                 // C2 control flow graph

  // Node mapping: Ideal Node → SDValue
  // Uses a simple hash table (Node* → SDValue)
  struct NodeMapping {
    Node* _node;
    void* _sd_value;              // SDValue pointer (opaque)
    NodeMapping* _next;
  };
  NodeMapping* _node_map[256];    // Hash table (256 buckets)

  // Current basic block
  void* _current_block;           // MachineBasicBlock pointer (opaque)

  // ========================================================================
  // Hash table operations
  // ========================================================================
  uint _hash(Node* node) const {
    return (uint)(uintptr_t)node % 256;
  }

  void* _lookup(Node* node) {
    uint bucket = _hash(node);
    for (NodeMapping* entry = _node_map[bucket]; entry; entry = entry->_next) {
      if (entry->_node == node) return entry->_sd_value;
    }
    return nullptr;
  }

  void _insert(Node* node, void* sd_value) {
    uint bucket = _hash(node);
    NodeMapping* entry = new NodeMapping();
    entry->_node = node;
    entry->_sd_value = sd_value;
    entry->_next = _node_map[bucket];
    _node_map[bucket] = entry;
  }

  // ========================================================================
  // Node type conversion
  // ========================================================================
  C2NodeTypes::C2NodeType _get_node_type(Node* node) const;

  // ========================================================================
  // Node conversion methods
  // ========================================================================
  void* _convert_constant(Node* node);
  void* _convert_integer_alu(Node* node);
  void* _convert_long_alu(Node* node);
  void* _convert_float_alu(Node* node);
  void* _convert_double_alu(Node* node);
  void* _convert_load(Node* node);
  void* _convert_store(Node* node);
  void* _convert_convert(Node* node);
  void* _convert_compare(Node* node);
  void* _convert_branch(Node* node);
  void* _convert_simd(Node* node);
  void* _convert_gc_barrier(Node* node);
  void* _convert_memory(Node* node);
  void* _convert_control(Node* node);

public:
  // ========================================================================
  // Constructor
  // ========================================================================
  DAGBuilder(C2SelectionDAG* dag, PhaseCFG* cfg);

  // ========================================================================
  // Main conversion method
  // ========================================================================
  //
  // Converts the entire Ideal Node graph to SelectionDAG.
  // Traverses all nodes in the CFG and creates corresponding SDNodes.
  //
  void build();

  // ========================================================================
  // Single node conversion
  // ========================================================================
  //
  // Converts a single Ideal Node to SelectionDAG.
  // Returns the SDValue (opaque pointer) for the converted node.
  // Uses caching to avoid duplicate conversions.
  //
  void* convert(Node* node);

  // ========================================================================
  // Block conversion
  // ========================================================================
  //
  // Converts all nodes in a basic block to SelectionDAG.
  // Creates MachineBasicBlock and adds SDNodes to it.
  //
  void convert_block(Node* block);

  // ========================================================================
  // Get converted value
  // ========================================================================
  //
  // Returns the cached SDValue for an Ideal Node, or nullptr if not converted.
  //
  void* get_sd_value(Node* node) const {
    return _lookup(node);
  }
};

#endif // SHARE_OPTO_DAG_BUILDER_HPP
