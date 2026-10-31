/*
 * Copyright (c) oneJDK-25 contributors. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "precompiled.hpp"
#include "opto/dag_builder.hpp"
#include "opto/C2SelectionDAG.hpp"
#include "opto/node.hpp"
#include "opto/opcodes.hpp"
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
#include "opto/cmpnode.hpp"
#include "opto/branchnode.hpp"
#include "opto/convnode.hpp"
#include "opto/phinode.hpp"
#include "opto/regionnode.hpp"
#include "opto/connode.hpp"
#include "opto/memnode.hpp"
#include "opto/projectnode.hpp"
#include "opto/ifnode.hpp"
#include "opto/stopnode.hpp"
#include "opto/catchnode.hpp"
#include "opto/loopnode.hpp"
#include "opto/multnode.hpp"
#include "opto/idealstack.hpp"
#include "opto/compile.hpp"
#include "opto/phaseCFG.hpp"
#include "opto/graph_its.hpp"

// ============================================================================
// DAGBuilder — Constructor
// ============================================================================

DAGBuilder::DAGBuilder(C2SelectionDAG* dag, PhaseCFG* cfg)
  : _dag(dag),
    _cfg(cfg),
    _current_block(nullptr)
{
  // Initialize hash table
  for (uint i = 0; i < 256; i++) {
    _node_map[i] = nullptr;
  }
}

// ============================================================================
// _get_node_type — Map C2 opcode to C2NodeType
// ============================================================================

C2NodeTypes::C2NodeType DAGBuilder::_get_node_type(Node* node) const {
  uint opcode = node->Opcode();

  // --- Control flow ---
  switch (opcode) {
  case Op_BlockEnd:     return C2NodeTypes::C2_BLOCK_END;
  case Op_If:           return C2NodeTypes::C2_IF;
  case Op_Stop:         return C2NodeTypes::C2_STOP;
  case Op_CatchEnd:     return C2NodeTypes::C2_CATCH_END;

  // --- Integer operations ---
  case Op_AddI:         return C2NodeTypes::C2_ADD_I;
  case Op_SubI:         return C2NodeTypes::C2_SUB_I;
  case Op_MulI:         return C2NodeTypes::C2_MUL_I;
  case Op_DivI:         return C2NodeTypes::C2_DIV_I;
  case Op_UDivI:        return C2NodeTypes::C2_UDIV_I;
  case Op_RemI:         return C2NodeTypes::C2_REM_I;
  case Op_URemI:        return C2NodeTypes::C2_UREM_I;
  case Op_AndI:         return C2NodeTypes::C2_AND_I;
  case Op_OrI:          return C2NodeTypes::C2_OR_I;
  case Op_XorI:         return C2NodeTypes::C2_XOR_I;
  case Op_ShlI:         return C2NodeTypes::C2_LSHIFT_I;
  case Op_ShrI:         return C2NodeTypes::C2_RSHIFT_I;
  case Op_UShrI:        return C2NodeTypes::C2.URSHIFT_I;

  // --- Long operations ---
  case Op_AddL:         return C2NodeTypes::C2_ADD_L;
  case Op_SubL:         return C2NodeTypes::C2_SUB_L;
  case Op_MulL:         return C2NodeTypes::C2_MUL_L;
  case Op_DivL:         return C2NodeTypes::C2_DIV_L;
  case Op_UDivL:        return C2NodeTypes::C2_UDIV_L;
  case Op_RemL:         return C2NodeTypes::C2_REM_L;
  case Op_URemL:        return C2NodeTypes::C2_UREM_L;
  case Op_AndL:         return C2NodeTypes::C2_AND_L;
  case Op_OrL:          return C2NodeTypes::C2_OR_L;
  case Op_XorL:         return C2NodeTypes::C2_XOR_L;
  case Op_ShlL:         return C2NodeTypes::C2_LSHIFT_L;
  case Op_ShrL:         return C2NodeTypes::C2_RSHIFT_L;
  case Op_UShrL:        return C2NodeTypes::C2.URSHIFT_L;

  // --- Float operations ---
  case Op_AddF:         return C2NodeTypes::C2_ADD_F;
  case Op_SubF:         return C2NodeTypes::C2_SUB_F;
  case Op_MulF:         return C2NodeTypes::C2_MUL_F;
  case Op_DivF:         return C2NodeTypes::C2_DIV_F;
  case Op_RemF:         return C2NodeTypes::C2_REM_F;
  case Op_NegF:         return C2NodeTypes::C2_NEG_F;
  case Op_SqrtF:        return C2NodeTypes::C2_SQRT_F;

  // --- Double operations ---
  case Op_AddD:         return C2NodeTypes::C2_ADD_D;
  case Op_SubD:         return C2NodeTypes::C2_SUB_D;
  case Op_MulD:         return C2NodeTypes::C2_MUL_D;
  case Op_DivD:         return C2NodeTypes::C2_DIV_D;
  case Op_RemD:         return C2NodeTypes::C2_REM_D;
  case Op_NegD:         return C2NodeTypes::C2_NEG_D;
  case Op_SqrtD:        return C2NodeTypes::C2_SQRT_D;

  // --- Load operations ---
  case Op_LoadB:        return C2NodeTypes::C2_LOAD_B;
  case Op_LoadUB:       return C2NodeTypes::C2_LOAD_UB;
  case Op_LoadS:        return C2NodeTypes::C2_LOAD_S;
  case Op_LoadUS:       return C2NodeTypes::C2_LOAD_US;
  case Op_LoadI:        return C2NodeTypes::C2_LOAD_I;
  case Op_LoadL:        return C2NodeTypes::C2_LOAD_L;
  case Op_LoadF:        return C2NodeTypes::C2_LOAD_F;
  case Op_LoadD:        return C2NodeTypes::C2_LOAD_D;
  case Op_LoadP:        return C2NodeTypes::C2_LOAD_P;

  // --- Store operations ---
  case Op_StoreB:       return C2NodeTypes::C2_STORE_B;
  case Op_StoreS:       return C2NodeTypes::C2_STORE_S;
  case Op_StoreI:       return C2NodeTypes::C2_STORE_I;
  case Op_StoreL:       return C2NodeTypes::C2_STORE_L;
  case Op_StoreF:       return C2NodeTypes::C2_STORE_F;
  case Op_StoreD:       return C2NodeTypes::C2_STORE_D;
  case Op_StoreP:       return C2NodeTypes::C2_STORE_P;

  // --- Convert operations ---
  case Op_I2L:          return C2NodeTypes::C2_I2L;
  case Op_I2F:          return C2NodeTypes::C2_I2F;
  case Op_I2D:          return C2NodeTypes::C2_I2D;
  case Op_L2I:          return C2NodeTypes::C2_L2I;
  case Op_L2F:          return C2NodeTypes::C2_L2F;
  case Op_L2D:          return C2NodeTypes::C2_L2D;
  case Op_F2I:          return C2NodeTypes::C2_F2I;
  case Op_F2L:          return C2NodeTypes::C2_F2L;
  case Op_F2D:          return C2NodeTypes::C2_F2D;
  case Op_D2I:          return C2NodeTypes::C2_D2I;
  case Op_D2L:          return C2NodeTypes::C2_D2L;
  case Op_D2F:          return C2NodeTypes::C2_D2F;

  // --- Compare operations ---
  case Op_CmpI:         return C2NodeTypes::C2_CMP_I;
  case Op_CmpL:         return C2NodeTypes::C2_CMP_L;
  case Op_CmpF:         return C2NodeTypes::C2_CMP_F;
  case Op_CmpD:         return C2NodeTypes::C2_CMP_D;
  case Op_CmpP:         return C2NodeTypes::C2_CMP_P;

  // --- SIMD operations ---
  case Op_AddFVec:      return C2NodeTypes::C2_ADD_FVEC;
  case Op_AddDVec:      return C2NodeTypes::C2_ADD_DVEC;
  case Op_MulFVec:      return C2NodeTypes::C2_MUL_FVEC;
  case Op_MulDVec:      return C2NodeTypes::C2_MUL_DVEC;
  case Op_SubFVec:      return C2NodeTypes::C2_SUB_FVEC;
  case Op_SubDVec:      return C2NodeTypes::C2_SUB_DVEC;
  case Op_DivFVec:      return C2NodeTypes::C2_DIV_FVEC;
  case Op_DivDVec:      return C2NodeTypes::C2_DIV_DVEC;

  // --- GC barrier operations ---
  case Op_G1LoadP:      return C2NodeTypes::C2_GC_LOAD_P;
  case Op_G1StoreP:     return C2NodeTypes::C2_GC_STORE_P;

  // --- Special operations ---
  case Op_Con:
  case Op_ConX:
  case Op_ConL:
  case Op_ConP:
  case Op_ConF:
  case Op_ConD:
    return C2NodeTypes::C2_CONSTANT;

  case Op_Mem:
  case Op_MemBar:
  case Op_MemBarCPUOrder:
  case Op_MemBarPlatformLoadLoad:
  case Op_MemBarPlatformStoreStore:
  case Op_MemBarPlatformLoadStore:
  case Op_MemBarVolatile:
  case Op_MemBarAcquire:
  case Op_MemBarRelease:
  case Op_MemBarSync:
    return C2NodeTypes::C2_MEMORY;

  case Op_Proj:         return C2NodeTypes::C2_PROJ;
  case Op_Region:       return C2NodeTypes::C2_REGION;

  default:
    return C2NodeTypes::C2_UNKNOWN;
  }
}

// ============================================================================
// _convert_constant — Convert constant nodes
// ============================================================================

void* DAGBuilder::_convert_constant(Node* node) {
  uint opcode = node->Opcode();

  switch (opcode) {
  case Op_Con: {
    // int constant
    ConNode* con = node->as_Con();
    jint value = con->_con.i();
    return _dag->get_int32_value(value);
  }
  case Op_ConL: {
    // long constant
    ConNode* con = node->as_Con();
    jlong value = con->_con.x();
    return _dag->get_int64_value(value);
  }
  case Op_ConF: {
    // float constant
    ConNode* con = node->as_Con();
    jfloat value = con->_con.f();
    return _dag->get_float_value(value);
  }
  case Op_ConD: {
    // double constant
    ConNode* con = node->as_Con();
    jdouble value = con->_con.d();
    return _dag->get_double_value(value);
  }
  case Op_ConP: {
    // pointer constant
    ConNode* con = node->as_Con();
    jobject value = con->_con.j();
    return _dag->get_ptr_value(value);
  }
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_integer_alu — Convert integer ALU operations
// ============================================================================

void* DAGBuilder::_convert_integer_alu(Node* node) {
  uint opcode = node->Opcode();

  // Get operands
  void* lhs = convert(node->in(1));
  void* rhs = convert(node->in(2));

  switch (opcode) {
  case Op_AddI:   return _dag->get_add32_node(lhs, rhs);
  case Op_SubI:   return _dag->get_sub32_node(lhs, rhs);
  case Op_MulI:   return _dag->get_mul32_node(lhs, rhs);
  case Op_DivI:   return _dag->get_sdiv32_node(lhs, rhs);
  case Op_UDivI:  return _dag->get_udiv32_node(lhs, rhs);
  case Op_RemI:   return _dag->get_srem32_node(lhs, rhs);
  case Op_URemI:  return _dag->get_urem32_node(lhs, rhs);
  case Op_AndI:   return _dag->get_and32_node(lhs, rhs);
  case Op_OrI:    return _dag->get_or32_node(lhs, rhs);
  case Op_XorI:   return _dag->get_xor32_node(lhs, rhs);
  case Op_ShlI:   return _dag->get_shl32_node(lhs, rhs);
  case Op_ShrI:   return _dag->get_sar32_node(lhs, rhs);
  case Op_UShrI:  return _dag->get_shr32_node(lhs, rhs);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_long_alu — Convert long ALU operations
// ============================================================================

void* DAGBuilder::_convert_long_alu(Node* node) {
  uint opcode = node->Opcode();

  // Get operands
  void* lhs = convert(node->in(1));
  void* rhs = convert(node->in(2));

  switch (opcode) {
  case Op_AddL:   return _dag->get_add64_node(lhs, rhs);
  case Op_SubL:   return _dag->get_sub64_node(lhs, rhs);
  case Op_MulL:   return _dag->get_mul64_node(lhs, rhs);
  case Op_DivL:   return _dag->get_sdiv64_node(lhs, rhs);
  case Op_UDivL:  return _dag->get_udiv64_node(lhs, rhs);
  case Op_RemL:   return _dag->get_srem64_node(lhs, rhs);
  case Op_URemL:  return _dag->get_urem64_node(lhs, rhs);
  case Op_AndL:   return _dag->get_and64_node(lhs, rhs);
  case Op_OrL:    return _dag->get_or64_node(lhs, rhs);
  case Op_XorL:   return _dag->get_xor64_node(lhs, rhs);
  case Op_ShlL:   return _dag->get_shl64_node(lhs, rhs);
  case Op_ShrL:   return _dag->get_sar64_node(lhs, rhs);
  case Op_UShrL:  return _dag->get_shr64_node(lhs, rhs);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_float_alu — Convert float ALU operations
// ============================================================================

void* DAGBuilder::_convert_float_alu(Node* node) {
  uint opcode = node->Opcode();

  if (opcode == Op_NegF || opcode == Op_SqrtF) {
    // Unary operations
    void* operand = convert(node->in(1));
    switch (opcode) {
    case Op_NegF:   return _dag->get_fneg32_node(operand);
    case Op_SqrtF:  return _dag->get_fsqrt32_node(operand);
    default:        ShouldNotReachHere(); return nullptr;
    }
  } else {
    // Binary operations
    void* lhs = convert(node->in(1));
    void* rhs = convert(node->in(2));
    switch (opcode) {
    case Op_AddF:   return _dag->get_fadd32_node(lhs, rhs);
    case Op_SubF:   return _dag->get_fsub32_node(lhs, rhs);
    case Op_MulF:   return _dag->get_fmul32_node(lhs, rhs);
    case Op_DivF:   return _dag->get_fdiv32_node(lhs, rhs);
    case Op_RemF:   return _dag->get_frem32_node(lhs, rhs);
    default:        ShouldNotReachHere(); return nullptr;
    }
  }
}

// ============================================================================
// _convert_double_alu — Convert double ALU operations
// ============================================================================

void* DAGBuilder::_convert_double_alu(Node* node) {
  uint opcode = node->Opcode();

  if (opcode == Op_NegD || opcode == Op_SqrtD) {
    // Unary operations
    void* operand = convert(node->in(1));
    switch (opcode) {
    case Op_NegD:   return _dag->get_fneg64_node(operand);
    case Op_SqrtD:  return _dag->get_fsqrt64_node(operand);
    default:        ShouldNotReachHere(); return nullptr;
    }
  } else {
    // Binary operations
    void* lhs = convert(node->in(1));
    void* rhs = convert(node->in(2));
    switch (opcode) {
    case Op_AddD:   return _dag->get_fadd64_node(lhs, rhs);
    case Op_SubD:   return _dag->get_fsub64_node(lhs, rhs);
    case Op_MulD:   return _dag->get_fmul64_node(lhs, rhs);
    case Op_DivD:   return _dag->get_fdiv64_node(lhs, rhs);
    case Op_RemD:   return _dag->get_frem64_node(lhs, rhs);
    default:        ShouldNotReachHere(); return nullptr;
    }
  }
}

// ============================================================================
// _convert_load — Convert load operations
// ============================================================================

void* DAGBuilder::_convert_load(Node* node) {
  uint opcode = node->Opcode();

  // Load nodes have: in(1)=control, in(2)=memory, in(3)=address, in(4)=IME
  void* addr = convert(node->in(3));

  switch (opcode) {
  case Op_LoadB:  return _dag->get_load8s_node(addr);
  case Op_LoadUB: return _dag->get_load8_node(addr);
  case Op_LoadS:  return _dag->get_load16s_node(addr);
  case Op_LoadUS: return _dag->get_load16_node(addr);
  case Op_LoadI:  return _dag->get_load32_node(addr);
  case Op_LoadL:  return _dag->get_load64_node(addr);
  case Op_LoadF:  return _dag->get_load_f32_node(addr);
  case Op_LoadD:  return _dag->get_load_f64_node(addr);
  case Op_LoadP:  return _dag->get_load_ptr_node(addr);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_store — Convert store operations
// ============================================================================

void* DAGBuilder::_convert_store(Node* node) {
  uint opcode = node->Opcode();

  // Store nodes have: in(1)=control, in(2)=mem, in(3)=addr, in(4)=value, in(5)=IME
  void* addr = convert(node->in(3));
  void* value = convert(node->in(4));

  switch (opcode) {
  case Op_StoreB: return _dag->get_store8_node(addr, value);
  case Op_StoreS: return _dag->get_store16_node(addr, value);
  case Op_StoreI: return _dag->get_store32_node(addr, value);
  case Op_StoreL: return _dag->get_store64_node(addr, value);
  case Op_StoreF: return _dag->get_store_f32_node(addr, value);
  case Op_StoreD: return _dag->get_store_f64_node(addr, value);
  case Op_StoreP: return _dag->get_store_ptr_node(addr, value);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_convert — Convert type conversion operations
// ============================================================================

void* DAGBuilder::_convert_convert(Node* node) {
  uint opcode = node->Opcode();

  void* operand = convert(node->in(1));

  switch (opcode) {
  case Op_I2L:  return _dag->get_zext32_64_node(operand);
  case Op_I2F:  return _dag->get_sitofp32_32_node(operand);
  case Op_I2D:  return _dag->get_sitofp32_64_node(operand);
  case Op_L2I:  return _dag->get_trunc64_32_node(operand);
  case Op_L2F:  return _dag->get_sitofp64_32_node(operand);
  case Op_L2D:  return _dag->get_sitofp64_64_node(operand);
  case Op_F2I:  return _dag->get_fptosi32_32_node(operand);
  case Op_F2L:  return _dag->get_fptosi32_64_node(operand);
  case Op_F2D:  return _dag->get_ext32_64_node(operand);
  case Op_D2I:  return _dag->get_fptosi64_32_node(operand);
  case Op_D2L:  return _dag->get_fptosi64_64_node(operand);
  case Op_D2F:  return _dag->get_trunc64_32_node(operand);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_compare — Convert compare operations
// ============================================================================

void* DAGBuilder::_convert_compare(Node* node) {
  uint opcode = node->Opcode();

  void* lhs = convert(node->in(1));
  void* rhs = convert(node->in(2));

  switch (opcode) {
  case Op_CmpI:   return _dag->get_set32_node(lhs, rhs);
  case Op_CmpL:   return _dag->get_set64_node(lhs, rhs);
  case Op_CmpF:   return _dag->get_set_f32_node(lhs, rhs);
  case Op_CmpD:   return _dag->get_set_f64_node(lhs, rhs);
  case Op_CmpP:   return _dag->get_set_ptr_node(lhs, rhs);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_branch — Convert branch operations
// ============================================================================

void* DAGBuilder::_convert_branch(Node* node) {
  uint opcode = node->Opcode();

  switch (opcode) {
  case Op_If: {
    // If node: in(1)=control, in(2)=test, in(3)=prob, in(4)=freq
    void* test = convert(node->in(2));
    return _dag->get_brcond_node(test);
  }
  case Op_Stop: {
    // Stop node: return
    StopNode* stop = node->as_Stop();
    if (stop->stop_type() == Stop::Return) {
      void* value = (node->req() > 1) ? convert(node->in(1)) : nullptr;
      return _dag->get_ret_node(value);
    }
    return _dag->get_unreachable_node();
  }
  case Op_CatchEnd: {
    // Exception catch
    return _dag->get_landing_pad_node();
  }
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_simd — Convert SIMD operations
// ============================================================================

void* DAGBuilder::_convert_simd(Node* node) {
  uint opcode = node->Opcode();

  void* lhs = convert(node->in(1));
  void* rhs = convert(node->in(2));

  switch (opcode) {
  case Op_AddFVec:  return _dag->get_vadd_f32_node(lhs, rhs);
  case Op_AddDVec:  return _dag->get_vadd_f64_node(lhs, rhs);
  case Op_MulFVec:  return _dag->get_vmul_f32_node(lhs, rhs);
  case Op_MulDVec:  return _dag->get_vmul_f64_node(lhs, rhs);
  case Op_SubFVec:  return _dag->get_vsub_f32_node(lhs, rhs);
  case Op_SubDVec:  return _dag->get_vsub_f64_node(lhs, rhs);
  case Op_DivFVec:  return _dag->get_vdiv_f32_node(lhs, rhs);
  case Op_DivDVec:  return _dag->get_vdiv_f64_node(lhs, rhs);
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_gc_barrier — Convert GC barrier operations
// ============================================================================

void* DAGBuilder::_convert_gc_barrier(Node* node) {
  uint opcode = node->Opcode();

  switch (opcode) {
  case Op_G1LoadP: {
    // G1 load barrier
    void* addr = convert(node->in(3));
    return _dag->get_g1_load_barrier_node(addr);
  }
  case Op_G1StoreP: {
    // G1 store barrier
    void* addr = convert(node->in(3));
    void* value = convert(node->in(4));
    return _dag->get_g1_store_barrier_node(addr, value);
  }
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// _convert_memory — Convert memory barrier operations
// ============================================================================

void* DAGBuilder::_convert_memory(Node* node) {
  uint opcode = node->Opcode();

  switch (opcode) {
  case Op_MemBarCPUOrder:
    return _dag->get_fence_node(C2SelectionDAG::FenceCPUOrder);
  case Op_MemBarVolatile:
    return _dag->get_fence_node(C2SelectionDAG::FenceVolatile);
  case Op_MemBarAcquire:
    return _dag->get_fence_node(C2SelectionDAG::FenceAcquire);
  case Op_MemBarRelease:
    return _dag->get_fence_node(C2SelectionDAG::FenceRelease);
  case Op_MemBarSync:
    return _dag->get_fence_node(C2SelectionDAG::FenceSync);
  default:
    // Other memory barriers — treat as full fence
    return _dag->get_fence_node(C2SelectionDAG::FenceSync);
  }
}

// ============================================================================
// _convert_control — Convert control flow operations
// ============================================================================

void* DAGBuilder::_convert_control(Node* node) {
  uint opcode = node->Opcode();

  switch (opcode) {
  case Op_Region: {
    // Region node: merge point from multiple predecessors
    uint num_values = node->req() - 1; // Exclude control input
    void** values = new void*[num_values];
    for (uint i = 0; i < num_values; i++) {
      values[i] = convert(node->in(i + 1));
    }
    return _dag->get_phi_node(values, num_values);
  }
  case Op_Proj: {
    // Projection: select one output from a multi-result node
    Node* parent = node->in(1);
    uint idx = node->outcnt();
    void* parent_value = convert(parent);
    return _dag->get_select_node(parent_value, idx);
  }
  case Op_BlockEnd: {
    // Block end: creates a new basic block
    return _dag->get_block_end_node();
  }
  default:
    ShouldNotReachHere();
    return nullptr;
  }
}

// ============================================================================
// convert — Convert a single Ideal Node to SelectionDAG
// ============================================================================

void* DAGBuilder::convert(Node* node) {
  if (node == nullptr) return nullptr;

  // Check cache first
  void* cached = _lookup(node);
  if (cached) return cached;

  // Get node type
  C2NodeTypes::C2NodeType type = _get_node_type(node);

  // Convert based on type
  void* result = nullptr;
  switch (type) {
  case C2NodeTypes::C2_CONSTANT:       result = _convert_constant(node); break;
  case C2NodeTypes::C2_ADD_I:
  case C2NodeTypes::C2_SUB_I:
  case C2NodeTypes::C2_MUL_I:
  case C2NodeTypes::C2_DIV_I:
  case C2NodeTypes::C2_UDIV_I:
  case C2NodeTypes::C2_REM_I:
  case C2NodeTypes::C2_UREM_I:
  case C2NodeTypes::C2_AND_I:
  case C2NodeTypes::C2_OR_I:
  case C2NodeTypes::C2_XOR_I:
  case C2NodeTypes::C2_LSHIFT_I:
  case C2NodeTypes::C2_RSHIFT_I:
  case C2NodeTypes::C2.URSHIFT_I:
    result = _convert_integer_alu(node); break;

  case C2NodeTypes::C2_ADD_L:
  case C2NodeTypes::C2_SUB_L:
  case C2NodeTypes::C2_MUL_L:
  case C2NodeTypes::C2_DIV_L:
  case C2NodeTypes::C2_UDIV_L:
  case C2NodeTypes::C2_REM_L:
  case C2NodeTypes::C2_UREM_L:
  case C2NodeTypes::C2_AND_L:
  case C2NodeTypes::C2_OR_L:
  case C2NodeTypes::C2_XOR_L:
  case C2NodeTypes::C2_LSHIFT_L:
  case C2NodeTypes::C2_RSHIFT_L:
  case C2NodeTypes::C2.URSHIFT_L:
    result = _convert_long_alu(node); break;

  case C2NodeTypes::C2_ADD_F:
  case C2NodeTypes::C2_SUB_F:
  case C2NodeTypes::C2_MUL_F:
  case C2NodeTypes::C2_DIV_F:
  case C2NodeTypes::C2_REM_F:
  case C2NodeTypes::C2_NEG_F:
  case C2NodeTypes::C2_SQRT_F:
    result = _convert_float_alu(node); break;

  case C2NodeTypes::C2_ADD_D:
  case C2NodeTypes::C2_SUB_D:
  case C2NodeTypes::C2_MUL_D:
  case C2NodeTypes::C2_DIV_D:
  case C2NodeTypes::C2_REM_D:
  case C2NodeTypes::C2_NEG_D:
  case C2NodeTypes::C2_SQRT_D:
    result = _convert_double_alu(node); break;

  case C2NodeTypes::C2_LOAD_B:
  case C2NodeTypes::C2_LOAD_UB:
  case C2NodeTypes::C2_LOAD_S:
  case C2NodeTypes::C2_LOAD_US:
  case C2NodeTypes::C2_LOAD_I:
  case C2NodeTypes::C2_LOAD_L:
  case C2NodeTypes::C2_LOAD_F:
  case C2NodeTypes::C2_LOAD_D:
  case C2NodeTypes::C2_LOAD_P:
    result = _convert_load(node); break;

  case C2NodeTypes::C2_STORE_B:
  case C2NodeTypes::C2_STORE_S:
  case C2NodeTypes::C2_STORE_I:
  case C2NodeTypes::C2_STORE_L:
  case C2NodeTypes::C2_STORE_F:
  case C2NodeTypes::C2_STORE_D:
  case C2NodeTypes::C2_STORE_P:
    result = _convert_store(node); break;

  case C2NodeTypes::C2_I2L:
  case C2NodeTypes::C2_I2F:
  case C2NodeTypes::C2_I2D:
  case C2NodeTypes::C2_L2I:
  case C2NodeTypes::C2_L2F:
  case C2NodeTypes::C2_L2D:
  case C2NodeTypes::C2_F2I:
  case C2NodeTypes::C2_F2L:
  case C2NodeTypes::C2_F2D:
  case C2NodeTypes::C2_D2I:
  case C2NodeTypes::C2_D2L:
  case C2NodeTypes::C2_D2F:
    result = _convert_convert(node); break;

  case C2NodeTypes::C2_CMP_I:
  case C2NodeTypes::C2_CMP_L:
  case C2NodeTypes::C2_CMP_F:
  case C2NodeTypes::C2_CMP_D:
  case C2NodeTypes::C2_CMP_P:
    result = _convert_compare(node); break;

  case C2NodeTypes::C2_IF:
  case C2NodeTypes::C2_STOP:
  case C2NodeTypes::C2_CATCH_END:
    result = _convert_branch(node); break;

  case C2NodeTypes::C2_ADD_FVEC:
  case C2NodeTypes::C2_ADD_DVEC:
  case C2NodeTypes::C2_MUL_FVEC:
  case C2NodeTypes::C2_MUL_DVEC:
  case C2NodeTypes::C2_SUB_FVEC:
  case C2NodeTypes::C2_SUB_DVEC:
  case C2NodeTypes::C2_DIV_FVEC:
  case C2NodeTypes::C2_DIV_DVEC:
    result = _convert_simd(node); break;

  case C2NodeTypes::C2_GC_LOAD_P:
  case C2NodeTypes::C2_GC_STORE_P:
    result = _convert_gc_barrier(node); break;

  case C2NodeTypes::C2_MEMORY:
    result = _convert_memory(node); break;

  case C2NodeTypes::C2_REGION:
  case C2NodeTypes::C2_PROJ:
  case C2NodeTypes::C2_BLOCK_END:
    result = _convert_control(node); break;

  default:
    // Unknown node type — skip
    result = nullptr;
    break;
  }

  // Cache the result
  if (result) {
    _insert(node, result);
  }

  return result;
}

// ============================================================================
// convert_block — Convert all nodes in a basic block
// ============================================================================

void DAGBuilder::convert_block(Node* block) {
  if (block == nullptr) return;

  // Create a new basic block in SelectionDAG
  _current_block = _dag->create_basic_block();

  // Iterate over all nodes in the block
  // In C2, blocks are represented as Region nodes with incoming edges
  for (DUIterator_Fast imx, im = block->Outs(imx); im; ++imx, ++im) {
    Node* use = block->Out(im);
    convert(use);
  }
}

// ============================================================================
// build — Convert entire Ideal Node graph to SelectionDAG
// ============================================================================

void DAGBuilder::build() {
  // Get the root node from C2
  Node* root = _cfg->get_root();
  if (root == nullptr) return;

  // Create entry basic block
  _current_block = _dag->create_basic_block();

  // Traverse all nodes in the CFG
  // Use a worklist to process nodes in topological order
  IdealStack worklist(Compile::current());

  // Start from root
  worklist.push(root);

  while (!worklist.is_empty()) {
    Node* node = worklist.pop();
    if (node == nullptr) continue;

    // Convert this node
    convert(node);

    // Add inputs to worklist (process dependencies first)
    for (DUIterator_Fast imx, im = node->Outs(imx); im; ++imx, ++im) {
      Node* use = node->Out(im);
      if (use && !_lookup(use)) {
        worklist.push(use);
      }
    }
  }

  // Finalize the DAG
  _dag->finalize();
}
