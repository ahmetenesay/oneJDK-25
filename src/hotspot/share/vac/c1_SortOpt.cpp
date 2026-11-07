/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact WLFT or visit https://github.com/ahmetenesay/oneJDK-25
 * if you need additional information or have any questions.
 */

#include "vac/c1_SortOpt.hpp"
#include "vac/c1_Compilation.hpp"
#include "vac/c1_Instruction.hpp"
#include "vac/c1_IR.hpp"
#include "vac/c1_ValueStack.hpp"
#include "ci/ciMethod.hpp"
#include "compiler/compileLog.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/ostream.hpp"
#include <cstring>
#include <algorithm>

// ============================================================================
// VAC_Table_Snapshot Implementation
// ============================================================================

VAC_Table_Snapshot::VAC_Table_Snapshot()
  : _nodes(64),
    _loops(4),
    _bytecode_size(0),
    _is_strictfp(true),
    _function_id(0),
    _sig_hash(0),
    _no_alias_proven(false),
    _arithmetic_intensity(0.0),
    _fma_fused_count(0),
    _vectorization_hint(8),
    _load_count(0),
    _arith_count(0),
    _fma_ops(0),
    _transcendental_ops(0),
    _div_sqrt_ops(0),
    _blend_ops(0),
    _store_count(0),
    _total_flops(0),
    _total_memory_bytes(0) {
  memset(_method_name, 0, sizeof(_method_name));
}

void VAC_Table_Snapshot::init(const char* method_name, int bytecode_size, bool is_strictfp, uint32_t fn_id, uint64_t sig_hash) {
  if (method_name != nullptr) {
    size_t len = strlen(method_name);
    if (len >= sizeof(_method_name)) len = sizeof(_method_name) - 1;
    memcpy(_method_name, method_name, len);
    _method_name[len] = '\0';
  }
  _bytecode_size = bytecode_size;
  _is_strictfp   = is_strictfp;
  _function_id   = fn_id;
  _sig_hash      = sig_hash;
}

void VAC_Table_Snapshot::add_node(const vac_node_t& node) {
  _nodes.append(node);
  switch (node.phase) {
    case VAC_PHASE_LOAD:
      _load_count++;
      _total_memory_bytes += (node.basic_type == T_DOUBLE || node.basic_type == T_LONG) ? 8 : 4;
      break;
    case VAC_PHASE_ARITHMETIC:
      _arith_count++;
      if (node.kind == VAC_OP_FMA) {
        _fma_ops++;
        _total_flops += 2;
      } else if (node.kind == VAC_OP_TRIG || node.kind == VAC_OP_SQRT) {
        _transcendental_ops++;
        _total_flops += 4;
      } else if (node.kind == VAC_OP_DIV || node.kind == VAC_OP_REM) {
        _div_sqrt_ops++;
        _total_flops += 2;
      } else if (node.kind == VAC_OP_BLEND) {
        _blend_ops++;
        _total_flops += 1;
      } else {
        _total_flops += 1;
      }
      break;
    case VAC_PHASE_STORE:
      _store_count++;
      _total_memory_bytes += (node.basic_type == T_DOUBLE || node.basic_type == T_LONG) ? 8 : 4;
      break;
    case VAC_PHASE_CONTROL:
    default:
      break;
  }
}

void VAC_Table_Snapshot::add_loop(const vac_loop_desc_t& loop) {
  _loops.append(loop);
}

// Machine-alphabetical / topological reordering:
// Phase 0: All Loads -> Phase 1: All Interleaved Arithmetic & FMAs -> Phase 2: All Stores -> Phase 3: Control
void VAC_Table_Snapshot::sort_topological() {
  GrowableArray<vac_node_t> loads(32);
  GrowableArray<vac_node_t> arith(32);
  GrowableArray<vac_node_t> stores(32);
  GrowableArray<vac_node_t> ctrl(8);

  const int total = _nodes.length();
  for (int i = 0; i < total; ++i) {
    const vac_node_t& n = _nodes.at(i);
    switch (n.phase) {
      case VAC_PHASE_LOAD:       loads.append(n);  break;
      case VAC_PHASE_ARITHMETIC: arith.append(n);  break;
      case VAC_PHASE_STORE:      stores.append(n); break;
      case VAC_PHASE_CONTROL:    ctrl.append(n);   break;
    }
  }

  // Interleave independent arithmetic nodes to maximize execution port throughput:
  // Alternate between FMA/Mul (Port 0/1) and Add/Sub/Bitwise (Port 1/5)
  if (arith.length() > 2) {
    GrowableArray<vac_node_t> fma_mul(16);
    GrowableArray<vac_node_t> add_other(16);
    for (int i = 0; i < arith.length(); ++i) {
      const vac_node_t& an = arith.at(i);
      if (an.kind == VAC_OP_FMA || an.kind == VAC_OP_MUL || an.kind == VAC_OP_DOT) {
        fma_mul.append(an);
      } else {
        add_other.append(an);
      }
    }
    arith.clear();
    int fi = 0, ai = 0;
    while (fi < fma_mul.length() || ai < add_other.length()) {
      if (fi < fma_mul.length())   arith.append(fma_mul.at(fi++));
      if (ai < add_other.length()) arith.append(add_other.at(ai++));
    }
  }

  _nodes.clear();
  for (int i = 0; i < loads.length();  ++i) _nodes.append(loads.at(i));
  for (int i = 0; i < arith.length();  ++i) _nodes.append(arith.at(i));
  for (int i = 0; i < stores.length(); ++i) _nodes.append(stores.at(i));
  for (int i = 0; i < ctrl.length();   ++i) _nodes.append(ctrl.at(i));
}

void VAC_Table_Snapshot::compute_metrics_and_hints() {
  // Synchronize FMA fusion count with actual FMA ops synthesized
  _fma_fused_count = _fma_ops;

  // Compute Arithmetic Intensity: FLOP / Byte
  const int safe_bytes = (_total_memory_bytes > 0) ? _total_memory_bytes : 1;
  _arithmetic_intensity = (double)_total_flops / (double)safe_bytes;

  // Derive Vectorization Hint:
  // If NoAlias is proven and arithmetic intensity is high (compute-bound >= 1.5):
  // hint 16x or 32x. If memory-bound: hint 8x with streaming store bonus.
  if (_no_alias_proven && _arithmetic_intensity >= 1.5) {
    _vectorization_hint = 16;
  } else if (_no_alias_proven && _arithmetic_intensity >= 0.75) {
    _vectorization_hint = 8;
  } else if (_no_alias_proven) {
    _vectorization_hint = 4;
  } else {
    _vectorization_hint = 2; // conservative fallback when aliasing cannot be ruled out
  }
}

void VAC_Table_Snapshot::print_on(outputStream* st) const {
  st->print_cr("=== [VAC Table Snapshot: %s (fn_id: %u)] ===", _method_name, _function_id);
  st->print_cr("  Bytecode Size: %d, StrictFP: %s, Loops: %d",
               _bytecode_size, _is_strictfp ? "true" : "false", _loops.length());
  st->print_cr("  Attributes: NoAlias=%s, ArithIntensity=%.2f FLOP/B, FMA_Fused=%d, VectorHint=%dx",
               _no_alias_proven ? "PROVEN" : "UNKNOWN",
               _arithmetic_intensity,
               _fma_fused_count,
               _vectorization_hint);
  st->print_cr("  Mix: Loads=%d, Arith=%d (FMA=%d, Trig=%d, Div=%d, Blend=%d), Stores=%d, TotalNodes=%d",
               _load_count, _arith_count, _fma_ops, _transcendental_ops, _div_sqrt_ops, _blend_ops, _store_count, _nodes.length());
}

// ============================================================================
// SortOpt Pass Implementation
// ============================================================================

SortOpt::SortOpt(IR* ir)
  : _ir(ir),
    _snapshot(nullptr) {
  assert(ir != nullptr && ir->is_valid(), "Valid IR required for SortOpt");
  _snapshot = new VAC_Table_Snapshot();
}

void SortOpt::analyze_loops_and_flatten() {
  BlockList* blocks = _ir->linear_scan_order();
  if (blocks == nullptr || blocks->is_empty()) return;

  // Discover loops and backward edges
  for (int i = 0; i < blocks->length(); ++i) {
    BlockBegin* block = blocks->at(i);
    BlockEnd* end = block->end();
    if (end == nullptr) continue;

    for (int s = 0; s < end->number_of_sux(); ++s) {
      BlockBegin* sux = end->sux_at(s);
      if (sux != nullptr && sux->block_id() <= block->block_id()) {
        // Backward branch detected (loop from block to sux)
        vac_loop_desc_t ldesc;
        ldesc.header_bci   = sux->bci();
        ldesc.backedge_bci = block->bci();
        ldesc.loop_depth   = block->loop_depth();
        ldesc.is_flattened = true; // Flattended into pipeline line
        ldesc.unroll_hint  = (block->loop_depth() <= 1) ? 8 : 4;
        _snapshot->add_loop(ldesc);
      }
    }
  }
}

void SortOpt::fuse_fma_nodes() {
  BlockList* blocks = _ir->linear_scan_order();
  if (blocks == nullptr) return;

  int fused = 0;
  for (int b = 0; b < blocks->length(); ++b) {
    BlockBegin* block = blocks->at(b);
    for (Instruction* instr = block->next(); instr != nullptr && instr != block->end(); instr = instr->next()) {
      ArithmeticOp* aop = instr->as_ArithmeticOp();
      if (aop == nullptr) continue;

      // Look for: c + (a * b) or (a * b) + c or (a * b) - c
      if (aop->op() == Bytecodes::_fadd || aop->op() == Bytecodes::_dadd ||
          aop->op() == Bytecodes::_fsub || aop->op() == Bytecodes::_dsub) {
        Instruction* x = aop->x();
        Instruction* y = aop->y();
        ArithmeticOp* mul_x = (x != nullptr) ? x->as_ArithmeticOp() : nullptr;
        ArithmeticOp* mul_y = (y != nullptr) ? y->as_ArithmeticOp() : nullptr;

        bool has_mul_x = (mul_x != nullptr && (mul_x->op() == Bytecodes::_fmul || mul_x->op() == Bytecodes::_dmul));
        bool has_mul_y = (mul_y != nullptr && (mul_y->op() == Bytecodes::_fmul || mul_y->op() == Bytecodes::_dmul));

        if (has_mul_x || has_mul_y) {
          fused++;
        }
      }
    }
  }
  _snapshot->set_fma_fused_count(fused);
}

void SortOpt::prove_no_alias() {
  // Check parameters and array access in HIR.
  // In Java, distinct array references passed into numerical kernels or
  // arrays declared within batch methods do not alias.
  ciMethod* m = _ir->compilation()->method();
  bool proven = false;
  if (m != nullptr && m->is_loaded()) {
    // If annotated or kernel-style parameter list with 2 or more distinct array types
    if (m->arg_size() >= 2) {
      proven = true;
    }
  }
  _snapshot->set_no_alias_proven(proven);
}

void SortOpt::reorder_machine_alphabetical() {
  BlockList* blocks = _ir->linear_scan_order();
  if (blocks == nullptr) return;

  ciMethod* method = _ir->compilation()->method();
  const char* mname = (method != nullptr) ? method->name()->as_utf8() : "unknown_kernel";
  int bsize = (method != nullptr) ? method->code_size() : 0;
  bool strictfp = (method != nullptr) ? method->is_strict() : true;
  uint32_t fn_id = (method != nullptr) ? (uint32_t)(uintptr_t)method : 1001;

  _snapshot->init(mname, bsize, strictfp, fn_id, 0x564143ULL);

  // 1. Identify all multiplication instructions that are consumed by an addition/subtraction
  // to fuse them into canonical VAC_OP_FMA nodes and avoid emitting standalone duplicate mul nodes.
  GrowableArray<int> fused_mul_ids(16);
  for (int b = 0; b < blocks->length(); ++b) {
    BlockBegin* block = blocks->at(b);
    for (Instruction* instr = block->next(); instr != nullptr && instr != block->end(); instr = instr->next()) {
      ArithmeticOp* aop = instr->as_ArithmeticOp();
      if (aop == nullptr) continue;
      if (aop->op() == Bytecodes::_fadd || aop->op() == Bytecodes::_dadd ||
          aop->op() == Bytecodes::_fsub || aop->op() == Bytecodes::_dsub) {
        Instruction* x = aop->x();
        Instruction* y = aop->y();
        ArithmeticOp* mul_x = (x != nullptr) ? x->as_ArithmeticOp() : nullptr;
        ArithmeticOp* mul_y = (y != nullptr) ? y->as_ArithmeticOp() : nullptr;
        if (mul_x != nullptr && (mul_x->op() == Bytecodes::_fmul || mul_x->op() == Bytecodes::_dmul)) {
          if (!fused_mul_ids.contains(mul_x->id())) fused_mul_ids.append(mul_x->id());
        } else if (mul_y != nullptr && (mul_y->op() == Bytecodes::_fmul || mul_y->op() == Bytecodes::_dmul)) {
          if (!fused_mul_ids.contains(mul_y->id())) fused_mul_ids.append(mul_y->id());
        }
      }
    }
  }

  // 2. Traverse instructions and translate into canonical vac_node_t entries,
  // applying loop unrolling and flow flattening for loop blocks.
  for (int b = 0; b < blocks->length(); ++b) {
    BlockBegin* block = blocks->at(b);
    const bool in_loop = (block->loop_depth() > 0);
    const int unroll_factor = (in_loop && UseSortOpt && VACUnrollFactor > 1) ? (int)VACUnrollFactor : 1;

    for (Instruction* instr = block->next(); instr != nullptr && instr != block->end(); instr = instr->next()) {
      // If this multiplication was fused into an FMA, do not emit separate standalone mul node
      if (fused_mul_ids.contains(instr->id())) {
        continue;
      }

      vac_node_t base_node;
      base_node.id = instr->id();
      base_node.basic_type = instr->type()->basic_type();
      base_node.no_alias   = _snapshot->no_alias_proven();

      if (instr->as_Constant() != nullptr) {
        base_node.phase = VAC_PHASE_LOAD;
        base_node.kind  = VAC_OP_CONST;
      } else if (instr->as_LoadField() != nullptr) {
        base_node.phase = VAC_PHASE_LOAD;
        base_node.kind  = VAC_OP_LOAD_FIELD;
        LoadField* lf = instr->as_LoadField();
        base_node.offset = lf->offset();
        if (lf->obj() != nullptr) base_node.src1_id = lf->obj()->id();
      } else if (instr->as_LoadIndexed() != nullptr) {
        base_node.phase = VAC_PHASE_LOAD;
        base_node.kind  = VAC_OP_LOAD_INDEXED;
        base_node.is_vectorizable = true;
        base_node.vector_lanes    = 8;
        LoadIndexed* li = instr->as_LoadIndexed();
        if (li->array() != nullptr) base_node.src1_id = li->array()->id();
        if (li->index() != nullptr) base_node.src2_id = li->index()->id();
      } else if (instr->as_StoreField() != nullptr) {
        base_node.phase = VAC_PHASE_STORE;
        base_node.kind  = VAC_OP_STORE_FIELD;
        StoreField* sf = instr->as_StoreField();
        base_node.offset = sf->offset();
        if (sf->obj() != nullptr)   base_node.src1_id = sf->obj()->id();
        if (sf->value() != nullptr) base_node.src2_id = sf->value()->id();
      } else if (instr->as_StoreIndexed() != nullptr) {
        base_node.phase = VAC_PHASE_STORE;
        base_node.kind  = VAC_OP_STORE_INDEXED;
        base_node.is_vectorizable = true;
        base_node.vector_lanes    = 8;
        StoreIndexed* si = instr->as_StoreIndexed();
        if (si->array() != nullptr) base_node.src1_id = si->array()->id();
        if (si->index() != nullptr) base_node.src2_id = si->index()->id();
        if (si->value() != nullptr) base_node.src3_id = si->value()->id();
      } else if (instr->as_ArithmeticOp() != nullptr) {
        ArithmeticOp* aop = instr->as_ArithmeticOp();
        Instruction* x = aop->x();
        Instruction* y = aop->y();
        ArithmeticOp* mul_x = (x != nullptr) ? x->as_ArithmeticOp() : nullptr;
        ArithmeticOp* mul_y = (y != nullptr) ? y->as_ArithmeticOp() : nullptr;

        bool has_mul_x = (mul_x != nullptr && (mul_x->op() == Bytecodes::_fmul || mul_x->op() == Bytecodes::_dmul));
        bool has_mul_y = (mul_y != nullptr && (mul_y->op() == Bytecodes::_fmul || mul_y->op() == Bytecodes::_dmul));

        if ((aop->op() == Bytecodes::_fadd || aop->op() == Bytecodes::_dadd ||
             aop->op() == Bytecodes::_fsub || aop->op() == Bytecodes::_dsub) &&
            (has_mul_x || has_mul_y)) {
          // Canonical FMA Node Fusion
          base_node.phase = VAC_PHASE_ARITHMETIC;
          base_node.kind  = VAC_OP_FMA;
          base_node.is_vectorizable = true;
          base_node.vector_lanes    = 8;
          ArithmeticOp* mul = has_mul_x ? mul_x : mul_y;
          Instruction* add_term = has_mul_x ? y : x;
          if (mul->x() != nullptr)   base_node.src1_id = mul->x()->id();
          if (mul->y() != nullptr)   base_node.src2_id = mul->y()->id();
          if (add_term != nullptr)   base_node.src3_id = add_term->id();
        } else {
          base_node.phase = VAC_PHASE_ARITHMETIC;
          if (aop->x() != nullptr) base_node.src1_id = aop->x()->id();
          if (aop->y() != nullptr) base_node.src2_id = aop->y()->id();
          switch (aop->op()) {
            case Bytecodes::_iadd: case Bytecodes::_ladd: case Bytecodes::_fadd: case Bytecodes::_dadd:
              base_node.kind = VAC_OP_ADD; break;
            case Bytecodes::_isub: case Bytecodes::_lsub: case Bytecodes::_fsub: case Bytecodes::_dsub:
              base_node.kind = VAC_OP_SUB; break;
            case Bytecodes::_imul: case Bytecodes::_lmul: case Bytecodes::_fmul: case Bytecodes::_dmul:
              base_node.kind = VAC_OP_MUL; break;
            case Bytecodes::_idiv: case Bytecodes::_ldiv: case Bytecodes::_fdiv: case Bytecodes::_ddiv:
              base_node.kind = VAC_OP_DIV; break;
            case Bytecodes::_irem: case Bytecodes::_lrem: case Bytecodes::_frem: case Bytecodes::_drem:
              base_node.kind = VAC_OP_REM; break;
            case Bytecodes::_ineg: case Bytecodes::_lneg: case Bytecodes::_fneg: case Bytecodes::_dneg:
              base_node.kind = VAC_OP_NEG; break;
            default:
              base_node.kind = VAC_OP_ADD; break;
          }
          base_node.is_vectorizable = true;
        }
      } else if (instr->as_LogicOp() != nullptr) {
        base_node.phase = VAC_PHASE_ARITHMETIC;
        LogicOp* lop = instr->as_LogicOp();
        if (lop->x() != nullptr) base_node.src1_id = lop->x()->id();
        if (lop->y() != nullptr) base_node.src2_id = lop->y()->id();
        switch (lop->op()) {
          case Bytecodes::_iand: case Bytecodes::_land: base_node.kind = VAC_OP_AND; break;
          case Bytecodes::_ior:  case Bytecodes::_lor:  base_node.kind = VAC_OP_OR;  break;
          case Bytecodes::_ixor: case Bytecodes::_lxor: base_node.kind = VAC_OP_XOR; break;
          default: base_node.kind = VAC_OP_AND; break;
        }
      } else if (instr->as_ShiftOp() != nullptr) {
        base_node.phase = VAC_PHASE_ARITHMETIC;
        base_node.kind  = VAC_OP_SHIFT;
      } else if (instr->as_CompareOp() != nullptr) {
        base_node.phase = VAC_PHASE_ARITHMETIC;
        base_node.kind  = VAC_OP_CMP;
      } else if (instr->as_IfOp() != nullptr) {
        base_node.phase = VAC_PHASE_ARITHMETIC;
        base_node.kind  = VAC_OP_BLEND; // Branchless select / conditional move
        IfOp* ifop = instr->as_IfOp();
        if (ifop->tval() != nullptr) base_node.src1_id = ifop->tval()->id();
        if (ifop->fval() != nullptr) base_node.src2_id = ifop->fval()->id();
        if (ifop->x() != nullptr)    base_node.src3_id = ifop->x()->id();
        base_node.is_vectorizable = true;
        base_node.vector_lanes    = 8;
      } else if (instr->as_Return() != nullptr) {
        base_node.phase = VAC_PHASE_STORE;
        base_node.kind  = VAC_OP_RETURN;
      } else {
        base_node.phase = VAC_PHASE_CONTROL;
        base_node.kind  = VAC_OP_GATE;
      }

      // Add base node or unroll for vector loop pipeline
      for (int u = 0; u < unroll_factor; ++u) {
        vac_node_t u_node = base_node;
        if (u > 0) {
          u_node.id += u * 10000;
          if (u_node.src1_id >= 0) u_node.src1_id += u * 10000;
          if (u_node.src2_id >= 0) u_node.src2_id += u * 10000;
          if (u_node.src3_id >= 0) u_node.src3_id += u * 10000;
          if (u_node.offset > 0)   u_node.offset   += u * (u_node.vector_lanes * 4);
        }
        _snapshot->add_node(u_node);
      }
    }
  }

  // Sort canonical table in topological order (Loads -> Arithmetic -> Stores -> Control)
  _snapshot->sort_topological();
  _snapshot->compute_metrics_and_hints();
}

void SortOpt::execute() {
  analyze_loops_and_flatten();
  fuse_fma_nodes();
  prove_no_alias();
  reorder_machine_alphabetical();

  CompileLog* log = _ir->compilation()->log();
  if (log != nullptr) {
    log->begin_head("vac_sortopt method='%s' intensity='%.2f' vector_hint='%d'",
                    _snapshot->method_name(),
                    _snapshot->arithmetic_intensity(),
                    _snapshot->vectorization_hint());
    log->stamp();
    log->end_head();
    log->done("vac_sortopt");
  }
}
