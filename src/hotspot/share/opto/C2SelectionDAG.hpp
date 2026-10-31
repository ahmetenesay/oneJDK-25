/*
 * Copyright (c) 2025, oneJDK-25 project. All rights reserved.
 *
 * C2SelectionDAG.hpp — C2 wrapper for LLVM SelectionDAG
 *
 * This file provides the C2-specific wrapper around LLVM's SelectionDAG
 * infrastructure. It handles C2-specific concerns that LLVM SelectionDAG
 * doesn't know about:
 *
 * 1. GC barriers (G1, ZGC, Shenandoah) — C2 memory barriers for load/store
 * 2. Safepoint polling — JVM safepoint check injection
 * 3. Ideal Node → SelectionDAG node mapping — C2 IR to LLVM IR conversion
 * 4. C2 register class mapping — OptoReg to LLVM register classes
 * 5. C2 deoptimization — JIT to interpreter fallback
 *
 * Architecture:
 * Bytecode → Ideal Node graph (C2, unchanged)
 *     ↓
 * dag_builder.cpp: Ideal Node → SelectionDAG nodes
 *     ↓
 * C2SelectionDAG.cpp: SelectionDAG wrapper (GC barrier, safepoint polling)
 *     ↓
 * X86DAGISel.cpp: tablegen pattern matching → X86 instructions
 *     ↓
 * C2ScheduleDAG.cpp: instruction scheduling
 *     ↓
 * C2RegAlloc.cpp: register allocation
 *     ↓
 * C2CodeEmitter.cpp: MachineInstr → machine code (MacroAssembler)
 *     ↓
 * Machine code
 */

#ifndef SHARE_OPTO_C2SELECTIONDAG_HPP
#define SHARE_OPTO_C2SELECTIONDAG_HPP

#include "opto/node.hpp"
#include "opto/cmpnode.hpp"
#include "opto/connode.hpp"
#include "opto/memnode.hpp"
#include "runtime/cpuFeatures.hpp"

// Forward declarations
class C2SelectionDAG;
class C2DAGBuilder;
class PhaseRegAlloc;
class C2_MacroAssembler;

// ============================================================================
// C2 SelectionDAG Node Types
// ============================================================================
//
// These are C2-specific SelectionDAG node types that extend LLVM's basic
// node types with JVM-specific semantics.
//

enum C2SDNodeClass {
  SDNC_Invalid = 0,

  // Basic operations
  SDNC_Copy,
  SDNC_Load,
  SDNC_Store,
  SDNC_BinOp,
  SDNC_UnOp,
  SDNC_Select,

  // JVM-specific operations
  SDNC_GC_Barrier,          // GC barrier (G1, ZGC, Shenandoah)
  SDNC_Safepoint_Poll,      // Safepoint polling
  SDNC_Deoptimize,          // Deoptimization trigger
  SDNC_Monitor_Enter,       // synchronized enter
  SDNC_Monitor_Exit,        // synchronized exit
  SDNC_New_Object,          // object allocation
  SDNC_Check_Cast,          // type check
  SDNC_Array_Bounds,        // array bounds check
  SDNC_Invoke,              // method invocation
  SDNC_Return,              // method return

  // Vector/SIMD operations
  SDNC_Vector_Shuffle,
  SDNC_Vector_Reduce,
  SDNC_Vector_Math,

  // Number of node classes
  SDNC_NumClasses
};

// ============================================================================
// C2 SelectionDAG Node
// ============================================================================
//
// A single node in the C2 SelectionDAG. Each node represents an operation
// (load, store, arithmetic, GC barrier, etc.) with operands and results.
//

class C2SDNode {
public:
  C2SDNode(C2SDNodeClass cls, uint num_ops = 0, uint num_res = 0);

  C2SDNodeClass class() const { return _cls; }
  uint num_operands() const { return _num_ops; }
  uint num_results() const { return _num_res; }

  C2SDNode* operand(uint i) const { return _operands[i]; }
  void set_operand(uint i, C2SDNode* node) { _operands[i] = node; }

  // C2 Ideal Node that produced this SelectionDAG node
  Node* ideal_node() const { return _ideal_node; }
  void set_ideal_node(Node* n) { _ideal_node = n; }

  // C2 register class for results
  uint reg_class() const { return _reg_class; }
  void set_reg_class(uint rc) { _reg_class = rc; }

  // Flags
  bool is_gc_barrier() const { return _cls == SDNC_GC_Barrier; }
  bool is_safepoint_poll() const { return _cls == SDNC_Safepoint_Poll; }
  bool is_deoptimize() const { return _cls == SDNC_Deoptimize; }

private:
  C2SDNodeClass _cls;
  uint _num_ops;
  uint _num_res;
  C2SDNode** _operands;
  Node* _ideal_node;       // Back-reference to C2 Ideal Node
  uint _reg_class;         // C2 register class (OptoReg::Name)
};

// ============================================================================
// C2 SelectionDAG
// ============================================================================
//
// The C2 SelectionDAG is a wrapper around LLVM's SelectionDAG that adds
// C2-specific functionality: GC barriers, safepoint polling, deoptimization,
// and C2 register class mapping.
//
// The DAG is built from C2 Ideal Nodes by the C2DAGBuilder, then passed
// to the X86DAGISel for instruction selection.
//

class C2SelectionDAG {
public:
  C2SelectionDAG();
  ~C2SelectionDAG();

  // ------------------------------------------------------------------------
  // DAG Construction
  // ------------------------------------------------------------------------

  // Add a node to the DAG
  C2SDNode* add_node(C2SDNodeClass cls, uint num_ops, uint num_res);

  // Add a root node (entry point for instruction selection)
  void add_root(C2SDNode* node);

  // Get all root nodes
  uint num_roots() const { return _num_roots; }
  C2SDNode* root(uint i) const { return _roots[i]; }

  // ------------------------------------------------------------------------
  // GC Barrier Integration
  // ------------------------------------------------------------------------

  // Insert a GC barrier before a store operation
  // barrier_type: G1, ZGC, Shenandoah
  void insert_gc_barrier(C2SDNode* store_node, int barrier_type);

  // Check if a node has a GC barrier
  bool has_gc_barrier(C2SDNode* node) const;

  // Get the GC barrier type for a node
  int gc_barrier_type(C2SDNode* node) const;

  // ------------------------------------------------------------------------
  // Safepoint Polling
  // ------------------------------------------------------------------------

  // Insert a safepoint poll at a loop back-edge
  void insert_safepoint_poll(C2SDNode* loop_node);

  // Check if safepoint polling is needed
  bool needs_safepoint_poll() const { return _needs_safepoint_poll; }

  // ------------------------------------------------------------------------
  // Deoptimization
  // ------------------------------------------------------------------------

  // Mark a node for deoptimization
  void mark_deoptimize(C2SDNode* node, int deopt_action);

  // Get deoptimization action for a node
  int deopt_action(C2SDNode* node) const;

  // ------------------------------------------------------------------------
  // Register Class Mapping
  // ------------------------------------------------------------------------

  // Map C2 OptoReg class to LLVM register class
  uint map_to_llvm_reg_class(uint c2_reg_class) const;

  // Map LLVM register class to C2 OptoReg class
  uint map_to_c2_reg_class(uint llvm_reg_class) const;

  // ------------------------------------------------------------------------
  // CPU Feature Tracking
  // ========================================================================
  //
  // Track which CPU features are used in the DAG. This is used by the
  // code emitter to generate feature-specific instructions (AVX2, AVX512,
  // AVX10, AMX, etc.).
  //

  // Record that a CPU feature is used
  void record_cpu_feature(const char* feature_name);

  // Check if a CPU feature is used
  bool uses_cpu_feature(const char* feature_name) const;

  // Get all used CPU features
  uint num_cpu_features() const { return _num_cpu_features; }
  const char* cpu_feature(uint i) const { return _cpu_features[i]; }

  // ------------------------------------------------------------------------
  // DAG Statistics
  // ------------------------------------------------------------------------

  uint num_nodes() const { return _num_nodes; }
  uint num_gc_barriers() const { return _num_gc_barriers; }
  uint num_safepoint_polls() const { return _num_safepoint_polls; }

  // Dump the DAG for debugging
  void dump() const;

private:
  // DAG nodes
  C2SDNode** _nodes;
  uint _num_nodes;
  uint _nodes_capacity;

  // Root nodes
  C2SDNode** _roots;
  uint _num_roots;
  uint _roots_capacity;

  // GC barrier tracking
  bool _needs_safepoint_poll;

  // CPU feature tracking
  const char** _cpu_features;
  uint _num_cpu_features;
  uint _cpu_features_capacity;

  // GC barrier count
  uint _num_gc_barriers;
  uint _num_safepoint_polls;

  // Grow the node array
  void grow_nodes();
};

// ============================================================================
// C2 DAG Builder
// ============================================================================
//
// Converts C2 Ideal Nodes to C2 SelectionDAG nodes. This is the bridge
// between C2's Ideal Graph representation and LLVM's SelectionDAG.
//

class C2DAGBuilder {
public:
  C2DAGBuilder(C2SelectionDAG* dag);

  // Build the SelectionDAG from a C2 Ideal Node graph
  void build(Node* root);

  // Convert a single Ideal Node to a SelectionDAG node
  C2SDNode* convert_node(Node* ideal_node);

  // Convert a memory node (load/store) to a SelectionDAG node
  C2SDNode* convert_mem_node(Node* mem_node);

  // Convert a phi node to a SelectionDAG node
  C2SDNode* convert_phi_node(Node* phi_node);

  // Convert a region node (branch) to a SelectionDAG node
  C2SDNode* convert_region_node(Node* region_node);

private:
  C2SelectionDAG* _dag;

  // Convert based on Ideal Node opcode
  C2SDNode* convert_by_opcode(Node* n, uint opcode);
};

#endif // SHARE_OPTO_C2SELECTIONDAG_HPP
