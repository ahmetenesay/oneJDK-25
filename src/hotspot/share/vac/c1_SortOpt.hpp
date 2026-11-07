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

#ifndef SHARE_VAC_C1_SORTOPT_HPP
#define SHARE_VAC_C1_SORTOPT_HPP

#include "vac/c1_Instruction.hpp"
#include "vac/c1_IR.hpp"
#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/globalDefinitions.hpp"

// ============================================================================
// VAC (Sanal Soyut Derleme) — Virtual Abstract Compilation Canonical Types
// ============================================================================

enum vac_op_phase_e {
  VAC_PHASE_LOAD        = 0, // Memory loads, array fetches, constants, parameters
  VAC_PHASE_ARITHMETIC  = 1, // Computational ALU, FMA, vector arithmetic, blend
  VAC_PHASE_STORE       = 2, // Memory stores, array stores, field stores, return
  VAC_PHASE_CONTROL     = 3  // Flattened control gates, parkpoint / dockpoint markers
};

enum vac_op_kind_e {
  // Phase 0: Loads & Constants
  VAC_OP_CONST = 0,
  VAC_OP_LOAD_FIELD,
  VAC_OP_LOAD_INDEXED,
  VAC_OP_LOAD_PARAM,
  VAC_OP_ARRAY_LENGTH,

  // Phase 1: Arithmetic & Vector ALU & Fused nodes
  VAC_OP_ADD,
  VAC_OP_SUB,
  VAC_OP_MUL,
  VAC_OP_DIV,
  VAC_OP_REM,
  VAC_OP_NEG,
  VAC_OP_FMA,           // FMA_NODE: a * b + c or a * b - c fusion
  VAC_OP_DOT,           // Vector dot product
  VAC_OP_TRIG,          // Transcendental / trigonometric op (sin, cos, exp, log)
  VAC_OP_SQRT,
  VAC_OP_AND,
  VAC_OP_OR,
  VAC_OP_XOR,
  VAC_OP_SHIFT,
  VAC_OP_CMP,
  VAC_OP_BLEND,         // Branchless select / conditional move
  VAC_OP_CONVERT,

  // Phase 2: Stores & Output
  VAC_OP_STORE_FIELD,
  VAC_OP_STORE_INDEXED,
  VAC_OP_STORE_STREAM,  // Non-temporal streaming store hint
  VAC_OP_RETURN,

  // Phase 3: Control & 2-Points Coordination
  VAC_OP_PARKPOINT,
  VAC_OP_DOCKPOINT,
  VAC_OP_GATE
};

// Canonical Abstract IR Node for VAC Table Snapshot
struct vac_node_t {
  vac_op_phase_e phase;
  vac_op_kind_e  kind;
  BasicType      basic_type;
  int            id;
  int            src1_id;
  int            src2_id;
  int            src3_id;          // For FMA (c in a*b + c) or Blend mask
  int64_t        imm_val;
  int            offset;
  bool           is_vectorizable;
  int            vector_lanes;     // Suggested lane width: 4, 8, 16, 32
  bool           no_alias;         // NoAlias proof: true if proven disjoint memory

  vac_node_t()
    : phase(VAC_PHASE_LOAD), kind(VAC_OP_CONST), basic_type(T_ILLEGAL),
      id(-1), src1_id(-1), src2_id(-1), src3_id(-1), imm_val(0),
      offset(0), is_vectorizable(false), vector_lanes(1), no_alias(false) {}
};

// Loop description discovered during VAC flow flattening
struct vac_loop_desc_t {
  int  header_bci;
  int  backedge_bci;
  int  loop_depth;
  int  unroll_hint;        // Vectorization hint: 2x, 4x, 8x, 16x
  bool is_flattened;
  bool no_alias_verified;  // All memory streams inside loop proven disjoint
  int  load_count;
  int  arith_count;
  int  store_count;
  int  fma_count;

  vac_loop_desc_t()
    : header_bci(-1), backedge_bci(-1), loop_depth(0), unroll_hint(8),
      is_flattened(false), no_alias_verified(false), load_count(0),
      arith_count(0), store_count(0), fma_count(0) {}
};

// ============================================================================
// Canonical VAC_Table_Snapshot Data Structure
// ============================================================================

class VAC_Table_Snapshot : public ResourceObj {
private:
  GrowableArray<vac_node_t>       _nodes;
  GrowableArray<vac_loop_desc_t>  _loops;

  char         _method_name[128];
  int          _bytecode_size;
  bool         _is_strictfp;
  uint32_t     _function_id;
  uint64_t     _sig_hash;

  // 4 Critical VAC Attributes
  bool         _no_alias_proven;       // 1. NoAlias Tag
  double       _arithmetic_intensity;  // 2. Arithmetic Intensity (FLOP / Byte)
  int          _fma_fused_count;       // 3. FMA_NODE Fusion Count
  int          _vectorization_hint;    // 4. Vectorization Hint (e.g. 8x, 16x, 32x)

  // Complexity & Instruction Mix Metrics
  int          _load_count;
  int          _arith_count;
  int          _fma_ops;
  int          _transcendental_ops;
  int          _div_sqrt_ops;
  int          _blend_ops;
  int          _store_count;
  int          _total_flops;
  int          _total_memory_bytes;

public:
  VAC_Table_Snapshot();

  void init(const char* method_name, int bytecode_size, bool is_strictfp, uint32_t fn_id, uint64_t sig_hash);

  void add_node(const vac_node_t& node);
  void add_loop(const vac_loop_desc_t& loop);

  // Getters
  int node_count() const { return _nodes.length(); }
  const vac_node_t& node_at(int idx) const { return _nodes.at(idx); }
  vac_node_t& node_at(int idx) { return _nodes.at(idx); }

  int loop_count() const { return _loops.length(); }
  const vac_loop_desc_t& loop_at(int idx) const { return _loops.at(idx); }

  const char* method_name() const { return _method_name; }
  int bytecode_size() const { return _bytecode_size; }
  bool is_strictfp() const { return _is_strictfp; }
  uint32_t function_id() const { return _function_id; }
  uint64_t sig_hash() const { return _sig_hash; }

  // 4 Critical Attributes Accessors
  bool no_alias_proven() const { return _no_alias_proven; }
  void set_no_alias_proven(bool v) { _no_alias_proven = v; }

  double arithmetic_intensity() const { return _arithmetic_intensity; }
  void set_arithmetic_intensity(double v) { _arithmetic_intensity = v; }

  int fma_fused_count() const { return _fma_fused_count; }
  void set_fma_fused_count(int v) { _fma_fused_count = v; }

  int vectorization_hint() const { return _vectorization_hint; }
  void set_vectorization_hint(int v) { _vectorization_hint = v; }

  // Metrics
  int load_count() const { return _load_count; }
  int arith_count() const { return _arith_count; }
  int fma_ops() const { return _fma_ops; }
  int transcendental_ops() const { return _transcendental_ops; }
  int div_sqrt_ops() const { return _div_sqrt_ops; }
  int blend_ops() const { return _blend_ops; }
  int store_count() const { return _store_count; }
  int total_flops() const { return _total_flops; }
  int total_memory_bytes() const { return _total_memory_bytes; }

  // Topological sorting into: [LOADS] -> [ARITHMETIC / FMAs] -> [STORES]
  void sort_topological();

  // Full analysis: FLOP/byte ratio, FMA opportunities, unroll hints
  void compute_metrics_and_hints();

  void print_on(outputStream* st) const;
};

// ============================================================================
// SortOpt Pass: Loop Unrolling, Flow Flattening & Machine-Alphabetical Reordering
// ============================================================================

class SortOpt : public CompilationResourceObj {
private:
  IR*                 _ir;
  VAC_Table_Snapshot* _snapshot;

  void analyze_loops_and_flatten();
  void fuse_fma_nodes();
  void prove_no_alias();
  void reorder_machine_alphabetical();

public:
  SortOpt(IR* ir);

  void execute();
  VAC_Table_Snapshot* snapshot() const { return _snapshot; }
};

#endif // SHARE_VAC_C1_SORTOPT_HPP
