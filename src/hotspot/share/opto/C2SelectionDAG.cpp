/*
 * Copyright (c) 2025, oneJDK-25 project. All rights reserved.
 *
 * C2SelectionDAG.cpp — C2 wrapper for LLVM SelectionDAG
 *
 * Implementation of the C2-specific SelectionDAG wrapper. Handles:
 * - GC barriers (G1, ZGC, Shenandoah)
 * - Safepoint polling
 * - Deoptimization
 * - C2 register class mapping
 * - CPU feature tracking
 */

#include "opto/C2SelectionDAG.hpp"
#include "opto/node.hpp"
#include "opto/addnode.hpp"
#include "opto/subnode.hpp"
#include "opto/mulnode.hpp"
#include "opto/divnode.hpp"
#include "opto/remnode.hpp"
#include "opto/andnode.hpp"
#include "opto/ornode.hpp"
#include "opto/xornode.hpp"
#include "opto/shlnode.hpp"
#include "opto/shrnode.hpp"
#include "opto/loadnode.hpp"
#include "opto/storenode.hpp"
#include "opto/phi.hpp"
#include "opto/region.hpp"
#include "opto/ifnode.hpp"
#include "opto/projectnode.hpp"
#include "opto/cmpnode.hpp"
#include "opto/connode.hpp"
#include "opto/memnode.hpp"
#include "opto/callnode.hpp"
#include "opto/returnnode.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

// ============================================================================
// C2SDNode Implementation
// ============================================================================

C2SDNode::C2SDNode(C2SDNodeClass cls, uint num_ops, uint num_res)
  : _cls(cls),
    _num_ops(num_ops),
    _num_res(num_res),
    _operands(num_ops > 0 ? NEW_C_HEAP_ARRAY(C2SDNode*, num_ops, mtCompiler) : nullptr),
    _ideal_node(nullptr),
    _reg_class(0) {
  // Initialize operands to nullptr
  for (uint i = 0; i < num_ops; i++) {
    _operands[i] = nullptr;
  }
}

// ============================================================================
// C2SelectionDAG Implementation
// ============================================================================

C2SelectionDAG::C2SelectionDAG()
  : _nodes(nullptr),
    _num_nodes(0),
    _nodes_capacity(0),
    _roots(nullptr),
    _num_roots(0),
    _roots_capacity(0),
    _needs_safepoint_poll(false),
    _cpu_features(nullptr),
    _num_cpu_features(0),
    _cpu_features_capacity(0),
    _num_gc_barriers(0),
    _num_safepoint_polls(0) {
}

C2SelectionDAG::~C2SelectionDAG() {
  // Free all nodes
  for (uint i = 0; i < _num_nodes; i++) {
    if (_nodes[i] != nullptr) {
      C2SDNode* node = _nodes[i];
      if (node->_operands != nullptr) {
        FREE_C_HEAP_ARRAY(C2SDNode*, node->_operands);
      }
      delete node;
    }
  }
  if (_nodes != nullptr) {
    FREE_C_HEAP_ARRAY(C2SDNode*, _nodes);
  }
  if (_roots != nullptr) {
    FREE_C_HEAP_ARRAY(C2SDNode*, _roots);
  }
  if (_cpu_features != nullptr) {
    FREE_C_HEAP_ARRAY(const char*, _cpu_features);
  }
}

// ------------------------------------------------------------------------
// DAG Construction
// ------------------------------------------------------------------------

C2SDNode* C2SelectionDAG::add_node(C2SDNodeClass cls, uint num_ops, uint num_res) {
  // Grow the node array if needed
  if (_num_nodes >= _nodes_capacity) {
    grow_nodes();
  }

  C2SDNode* node = new C2SDNode(cls, num_ops, num_res);
  _nodes[_num_nodes++] = node;

  // Track GC barriers and safepoint polls
  if (cls == SDNC_GC_Barrier) {
    _num_gc_barriers++;
  } else if (cls == SDNC_Safepoint_Poll) {
    _num_safepoint_polls++;
    _needs_safepoint_poll = true;
  }

  return node;
}

void C2SelectionDAG::add_root(C2SDNode* node) {
  if (_num_roots >= _roots_capacity) {
    uint new_capacity = _roots_capacity == 0 ? 16 : _roots_capacity * 2;
    _roots = REALLOC_C_HEAP_ARRAY(C2SDNode*, _roots, new_capacity, mtCompiler);
    _roots_capacity = new_capacity;
  }
  _roots[_num_roots++] = node;
}

// ------------------------------------------------------------------------
// GC Barrier Integration
// ------------------------------------------------------------------------

void C2SelectionDAG::insert_gc_barrier(C2SDNode* store_node, int barrier_type) {
  // Create a GC barrier node with the store as an operand
  C2SDNode* barrier = add_node(SDNC_GC_Barrier, 1, 0);
  barrier->set_operand(0, store_node);
  barrier->set_ideal_node(store_node->ideal_node());
  // barrier_type is stored in reg_class field for now
  barrier->set_reg_class(barrier_type);
}

bool C2SelectionDAG::has_gc_barrier(C2SDNode* node) const {
  return node->is_gc_barrier();
}

int C2SelectionDAG::gc_barrier_type(C2SDNode* node) const {
  if (node->is_gc_barrier()) {
    return (int)node->reg_class();
  }
  return 0;
}

// ------------------------------------------------------------------------
// Safepoint Polling
// ------------------------------------------------------------------------

void C2SelectionDAG::insert_safepoint_poll(C2SDNode* loop_node) {
  C2SDNode* poll = add_node(SDNC_Safepoint_Poll, 1, 0);
  poll->set_operand(0, loop_node);
  poll->set_ideal_node(loop_node->ideal_node());
  _needs_safepoint_poll = true;
}

// ------------------------------------------------------------------------
// Deoptimization
// ------------------------------------------------------------------------

void C2SelectionDAG::mark_deoptimize(C2SDNode* node, int deopt_action) {
  // Store deopt action in reg_class field
  node->set_reg_class(deopt_action);
}

int C2SelectionDAG::deopt_action(C2SDNode* node) const {
  if (node->is_deoptimize()) {
    return (int)node->reg_class();
  }
  return 0;
}

// ------------------------------------------------------------------------
// Register Class Mapping
// ------------------------------------------------------------------------

uint C2SelectionDAG::map_to_llvm_reg_class(uint c2_reg_class) const {
  // Map C2 OptoReg::Name to LLVM register class
  // This is a simplified mapping — full mapping requires LLVM register info
  switch (c2_reg_class) {
    case 0:  // OptoReg::Stack0
      return 0;
    case 1:  // OptoReg::Flags
      return 1;  // CC register class
    default:
      // GPR registers (RAX, RCX, etc.) map to GR64
      if (c2_reg_class >= 16 && c2_reg_class < 128) {
        return 2;  // GR64 register class
      }
      // XMM registers map to XMM0_reg
      if (c2_reg_class >= 128 && c2_reg_class < 256) {
        return 3;  // XMM0 register class
      }
      return c2_reg_class;  // Pass through for unknown
  }
}

uint C2SelectionDAG::map_to_c2_reg_class(uint llvm_reg_class) const {
  // Map LLVM register class to C2 OptoReg::Name
  switch (llvm_reg_class) {
    case 0:
      return 0;  // Stack0
    case 1:
      return 1;  // Flags
    case 2:
      return 16; // First GPR (RAX)
    case 3:
      return 128;// First XMM
    default:
      return llvm_reg_class;
  }
}

// ------------------------------------------------------------------------
// CPU Feature Tracking
// ------------------------------------------------------------------------

void C2SelectionDAG::record_cpu_feature(const char* feature_name) {
  // Check if already recorded
  for (uint i = 0; i < _num_cpu_features; i++) {
    if (strcmp(_cpu_features[i], feature_name) == 0) {
      return;  // Already recorded
    }
  }

  // Grow the feature array if needed
  if (_num_cpu_features >= _cpu_features_capacity) {
    uint new_capacity = _cpu_features_capacity == 0 ? 16 : _cpu_features_capacity * 2;
    _cpu_features = REALLOC_C_HEAP_ARRAY(const char*, _cpu_features, new_capacity, mtCompiler);
    _cpu_features_capacity = new_capacity;
  }

  // Store the feature name
  _cpu_features[_num_cpu_features++] = feature_name;
}

bool C2SelectionDAG::uses_cpu_feature(const char* feature_name) const {
  for (uint i = 0; i < _num_cpu_features; i++) {
    if (strcmp(_cpu_features[i], feature_name) == 0) {
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------------
// DAG Statistics
// ------------------------------------------------------------------------

void C2SelectionDAG::dump() const {
  tty->print_cr("=== C2 SelectionDAG Dump ===");
  tty->print_cr("Nodes: %d", _num_nodes);
  tty->print_cr("Roots: %d", _num_roots);
  tty->print_cr("GC barriers: %d", _num_gc_barriers);
  tty->print_cr("Safepoint polls: %d", _num_safepoint_polls);
  tty->print_cr("CPU features: %d", _num_cpu_features);

  for (uint i = 0; i < _num_cpu_features; i++) {
    tty->print_cr("  CPU feature: %s", _cpu_features[i]);
  }

  tty->print_cr("=== End Dump ===");
}

// ------------------------------------------------------------------------
// Internal Helpers
// ------------------------------------------------------------------------

void C2SelectionDAG::grow_nodes() {
  uint new_capacity = _nodes_capacity == 0 ? 256 : _nodes_capacity * 2;
  _nodes = REALLOC_C_HEAP_ARRAY(C2SDNode*, _nodes, new_capacity, mtCompiler);
  _nodes_capacity = new_capacity;
}

// ============================================================================
// C2DAGBuilder Implementation
// ============================================================================

C2DAGBuilder::C2DAGBuilder(C2SelectionDAG* dag)
  : _dag(dag) {
}

void C2DAGBuilder::build(Node* root) {
  if (root == nullptr) return;

  // Convert the root node
  C2SDNode* sd_root = convert_node(root);
  if (sd_root != nullptr) {
    _dag->add_root(sd_root);
  }
}

C2SDNode* C2DAGBuilder::convert_node(Node* ideal_node) {
  if (ideal_node == nullptr) return nullptr;

  uint opcode = ideal_node->Opcode();
  return convert_by_opcode(ideal_node, opcode);
}

C2SDNode* C2DAGBuilder::convert_mem_node(Node* mem_node) {
  if (mem_node == nullptr) return nullptr;

  uint opcode = mem_node->Opcode();
  if (opcode == Op_LoadB || opcode == Op_LoadUB ||
      opcode == Op_LoadS || opcode == Op_LoadUS ||
      opcode == Op_LoadI || opcode == Op_LoadL ||
      opcode == Op_LoadF || opcode == Op_LoadD ||
      opcode == Op_LoadP) {
    C2SDNode* load = _dag->add_node(SDNC_Load, mem_node->req(), 1);
    load->set_ideal_node(mem_node);
    for (uint i = 1; i < mem_node->req(); i++) {
      load->set_operand(i - 1, convert_node(mem_node->in(i)));
    }
    return load;
  } else if (opcode == Op_StoreB || opcode == Op_StoreI ||
             opcode == Op_StoreL || opcode == Op_StoreF ||
             opcode == Op_StoreD || opcode == Op_StoreP) {
    C2SDNode* store = _dag->add_node(SDNC_Store, mem_node->req(), 0);
    store->set_ideal_node(mem_node);
    for (uint i = 1; i < mem_node->req(); i++) {
      store->set_operand(i - 1, convert_node(mem_node->in(i)));
    }
    return store;
  }

  return convert_by_opcode(mem_node, opcode);
}

C2SDNode* C2DAGBuilder::convert_phi_node(Node* phi_node) {
  if (phi_node == nullptr) return nullptr;

  C2SDNode* phi = _dag->add_node(SDNC_Copy, phi_node->req(), 1);
  phi->set_ideal_node(phi_node);
  for (uint i = 1; i < phi_node->req(); i++) {
    phi->set_operand(i - 1, convert_node(phi_node->in(i)));
  }
  return phi;
}

C2SDNode* C2DAGBuilder::convert_region_node(Node* region_node) {
  if (region_node == nullptr) return nullptr;

  C2SDNode* region = _dag->add_node(SDNC_Select, region_node->req(), 1);
  region->set_ideal_node(region_node);
  for (uint i = 1; i < region_node->req(); i++) {
    region->set_operand(i - 1, convert_node(region_node->in(i)));
  }
  return region;
}

C2SDNode* C2DAGBuilder::convert_by_opcode(Node* n, uint opcode) {
  // Binary operations
  if (opcode == Op_AddI || opcode == Op_AddL) {
    C2SDNode* binop = _dag->add_node(SDNC_BinOp, n->req(), 1);
    binop->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      binop->set_operand(i - 1, convert_node(n->in(i)));
    }
    return binop;
  }

  if (opcode == Op_SubI || opcode == Op_SubL ||
      opcode == Op_MulI || opcode == Op_MulL ||
      opcode == Op_AndI || opcode == Op_AndL ||
      opcode == Op_OrI || opcode == Op_OrL ||
      opcode == Op_XorI || opcode == Op_XorL) {
    C2SDNode* binop = _dag->add_node(SDNC_BinOp, n->req(), 1);
    binop->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      binop->set_operand(i - 1, convert_node(n->in(i)));
    }
    return binop;
  }

  // Unary operations
  if (opcode == Op_NegI || opcode == Op_NegL ||
      opcode == Op_NotI || opcode == Op_NotL) {
    C2SDNode* unop = _dag->add_node(SDNC_UnOp, n->req(), 1);
    unop->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      unop->set_operand(i - 1, convert_node(n->in(i)));
    }
    return unop;
  }

  // Shift operations
  if (opcode == Op_LShiftI || opcode == Op_LShiftL ||
      opcode == Op_RShiftI || opcode == Op_RShiftL ||
      opcode == Op_URShiftI || opcode == Op_URShiftL) {
    C2SDNode* binop = _dag->add_node(SDNC_BinOp, n->req(), 1);
    binop->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      binop->set_operand(i - 1, convert_node(n->in(i)));
    }
    return binop;
  }

  // Float/double operations
  if (opcode == Op_AddF || opcode == Op_AddD ||
      opcode == Op_SubF || opcode == Op_SubD ||
      opcode == Op_MulF || opcode == Op_MulD ||
      opcode == Op_DivF || opcode == Op_DivD) {
    C2SDNode* binop = _dag->add_node(SDNC_BinOp, n->req(), 1);
    binop->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      binop->set_operand(i - 1, convert_node(n->in(i)));
    }
    return binop;
  }

  // Memory operations
  if (opcode >= Op_LoadB && opcode <= Op_LoadP) {
    return convert_mem_node(n);
  }
  if (opcode >= Op_StoreB && opcode <= Op_StoreP) {
    return convert_mem_node(n);
  }

  // Phi nodes
  if (opcode == Op_Phi) {
    return convert_phi_node(n->as_Phi());
  }

  // Region nodes
  if (opcode == Op_Region) {
    return convert_region_node(n->as_Region());
  }

  // If nodes (branches)
  if (opcode == Op_If) {
    C2SDNode* sel = _dag->add_node(SDNC_Select, n->req(), 2);
    sel->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      sel->set_operand(i - 1, convert_node(n->in(i)));
    }
    return sel;
  }

  // Return nodes
  if (opcode == Op_Return || opcode == Op_ReturnN || opcode == Op_ReturnAddress) {
    C2SDNode* ret = _dag->add_node(SDNC_Return, n->req(), 0);
    ret->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      ret->set_operand(i - 1, convert_node(n->in(i)));
    }
    return ret;
  }

  // Call nodes
  if (opcode == Op_CallStaticJava || opcode == Op_CallVirtualJava ||
      opcode == Op_CallSuperJava || opcode == Op_CallInterfaceJava) {
    C2SDNode* call = _dag->add_node(SDNC_Invoke, n->req(), 1);
    call->set_ideal_node(n);
    for (uint i = 1; i < n->req(); i++) {
      call->set_operand(i - 1, convert_node(n->in(i)));
    }
    return call;
  }

  // Default: copy node
  C2SDNode* copy = _dag->add_node(SDNC_Copy, n->req(), 1);
  copy->set_ideal_node(n);
  for (uint i = 1; i < n->req(); i++) {
    copy->set_operand(i - 1, convert_node(n->in(i)));
  }
  return copy;
}
