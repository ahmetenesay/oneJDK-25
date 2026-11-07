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

#ifndef SHARE_VAC_VAC_TABLESNAPSHOT_HPP
#define SHARE_VAC_VAC_TABLESNAPSHOT_HPP

#include <cstdint>
#include <cstring>
#include "utilities/globalDefinitions.hpp"
#include "vac/c1_SortOpt.hpp"

// ============================================================================
// VAC (Pre-MIRC Sanal Soyut Derleyici) — Data Structures & Tape Snapshot
// ============================================================================

enum VAC_DataType {
  VAC_TYPE_INT32     = 0,
  VAC_TYPE_INT64     = 1,
  VAC_TYPE_FLOAT32   = 2,
  VAC_TYPE_FLOAT64   = 3,
  VAC_TYPE_OBJECT    = 4,
  VAC_TYPE_VECTOR128 = 5,
  VAC_TYPE_VECTOR256 = 6,
  VAC_TYPE_VECTOR512 = 7
};

enum VAC_AffinityHint {
  VAC_AFFINITY_ALU    = 0,
  VAC_AFFINITY_FMA    = 1,
  VAC_AFFINITY_MEM    = 2,
  VAC_AFFINITY_BRANCH = 3
};

struct VAC_Instruction_Entry {
  uint16_t opcode;            // Soyut islem kodu (VAC_OP_LOAD, VAC_OP_FMA, vb.)
  uint8_t  datatype;          // Float32, Float64, Int32, Int64, Vector512
  uint16_t src1, src2, src3;  // SSA Girdi Degerleri
  uint16_t dst;               // SSA Cikti Degeri
  uint8_t  group_phase;       // 0 = LOAD, 1 = ALU/FMA, 2 = STORE
  uint8_t  fma_fusion_ready;  // 1 = FMA3 / FMA4 aday
};

struct VAC_Requirements {
  uint16_t virt_gpr_pressure;
  uint16_t virt_vec_pressure;
  uint32_t sram_footprint_bytes;
  uint8_t  branch_pressure;
  uint8_t  mem_bandwidth_class;
  uint8_t  affinity_hint;     // ALU/FMA/MEM/BRANCH
};

// Faz 4: Discrete TBT Slice & Table
struct Discrete_TBT_Slice {
  uint32_t epoch_window;
  uint8_t  dominant_branch_dir;
  uint8_t  chaos_level;
  uint16_t recommended_slot;
};

struct Discrete_TBT_Table {
  uint32_t slice_count;
  Discrete_TBT_Slice slices[16];
};

struct VAC_Tape_Header {
  uint32_t magic;              // 0x56414354 ("VACT")
  uint32_t version;            // 0x00010000 (v1.0)
  uint32_t method_id;
  uint32_t instruction_count;
  VAC_Requirements req;
  uint8_t  has_no_alias;
  uint8_t  arithmetic_intensity;
  uint8_t  unroll_recommendation;
  uint8_t  vectorization_hint;
  uint16_t tbt_branch_count;
  uint16_t reserved;
};

struct VAC_Table_Snapshot_Compact {
  uint32_t method_id;
  uint32_t instruction_count;
  VAC_Instruction_Entry* instructions; // Makine-alfabetik sirali liste
  VAC_Requirements req;
  uint8_t  has_no_alias;
  uint8_t  arithmetic_intensity;
  uint8_t  unroll_recommendation;
  uint8_t  vectorization_hint;
  uint16_t tbt_branch_count;
  void*    tbt_rollback_deltas;

  static VAC_Table_Snapshot_Compact* create(uint32_t method_id, uint32_t inst_count) {
    VAC_Table_Snapshot_Compact* snap = new VAC_Table_Snapshot_Compact();
    snap->method_id = method_id;
    snap->instruction_count = inst_count;
    snap->instructions = inst_count > 0 ? new VAC_Instruction_Entry[inst_count] : nullptr;
    memset(&snap->req, 0, sizeof(snap->req));
    snap->has_no_alias = 0;
    snap->arithmetic_intensity = 0;
    snap->unroll_recommendation = 0;
    snap->vectorization_hint = 0;
    snap->tbt_branch_count = 0;
    snap->tbt_rollback_deltas = nullptr;
    return snap;
  }

  static void destroy(VAC_Table_Snapshot_Compact* snap) {
    if (snap != nullptr) {
      if (snap->instructions != nullptr) {
        delete[] snap->instructions;
        snap->instructions = nullptr;
      }
      delete snap;
    }
  }

  // Convert from C1 SortOpt VAC_Table_Snapshot resource object
  static VAC_Table_Snapshot_Compact* from_resource_snapshot(const VAC_Table_Snapshot* src) {
    if (src == nullptr) return nullptr;
    uint32_t count = (uint32_t)src->node_count();
    VAC_Table_Snapshot_Compact* snap = create(src->function_id(), count);
    snap->has_no_alias = src->no_alias_proven() ? 1 : 0;
    snap->arithmetic_intensity = (uint8_t)(src->arithmetic_intensity() > 255.0 ? 255 : (uint8_t)src->arithmetic_intensity());
    snap->unroll_recommendation = (uint8_t)VACUnrollFactor;
    snap->vectorization_hint = (uint8_t)src->vectorization_hint();

    // Virtual topology requirements estimation (ARRT)
    snap->req.virt_gpr_pressure = (uint16_t)(src->load_count() + src->store_count());
    snap->req.virt_vec_pressure = (uint16_t)(src->fma_ops() + src->div_sqrt_ops());
    snap->req.sram_footprint_bytes = (uint32_t)src->total_memory_bytes();
    snap->req.branch_pressure = (uint8_t)(src->blend_ops() > 255 ? 255 : src->blend_ops());
    snap->req.mem_bandwidth_class = (snap->req.sram_footprint_bytes > 32768) ? 2 : 1;
    snap->req.affinity_hint = (snap->req.virt_vec_pressure > 16) ? VAC_AFFINITY_FMA : VAC_AFFINITY_ALU;

    for (uint32_t i = 0; i < count; i++) {
      const vac_node_t& node = src->node_at((int)i);
      VAC_Instruction_Entry& entry = snap->instructions[i];
      entry.opcode = (uint16_t)node.kind;
      switch (node.basic_type) {
        case T_FLOAT:  entry.datatype = VAC_TYPE_FLOAT32; break;
        case T_DOUBLE: entry.datatype = VAC_TYPE_FLOAT64; break;
        case T_LONG:   entry.datatype = VAC_TYPE_INT64; break;
        case T_OBJECT:
        case T_ARRAY:  entry.datatype = VAC_TYPE_OBJECT; break;
        default:       entry.datatype = VAC_TYPE_INT32; break;
      }
      entry.src1 = (uint16_t)(node.src1_id >= 0 ? node.src1_id : 0);
      entry.src2 = (uint16_t)(node.src2_id >= 0 ? node.src2_id : 0);
      entry.src3 = (uint16_t)(node.src3_id >= 0 ? node.src3_id : 0);
      entry.dst  = (uint16_t)(node.id >= 0 ? node.id : 0);
      entry.group_phase = (uint8_t)node.phase;
      entry.fma_fusion_ready = (node.kind == VAC_OP_FMA) ? 1 : 0;
    }

    return snap;
  }

  // =========================================================================
  // VFS /heap/off_heap/code_boot Cassette Serialization & W^X Key Enforcement
  //   - rawcode_cell: sealed read-only, unlocked exclusively via KEY_VAC_READ
  //   - vac_processed_code_chunk: W^X protected, unlocked exclusively via KEY_MIRC_READ
  //   - Universal GC compatible: falls back to C-heap cassette when !UseRedjetGC
  // =========================================================================
  static const uint8_t* seal_rawcode_cell(uint32_t method_id,
                                          const uint8_t* raw_bytecode,
                                          size_t bytecode_len,
                                          uint64_t vac_access_key);

  static bool serialize_to_vfs_chunk(const VAC_Table_Snapshot_Compact* snap,
                                     const Discrete_TBT_Table* tbt_opt,
                                     uint64_t mirc_seal_key,
                                     void** out_chunk_addr,
                                     size_t* out_chunk_bytes);

  static VAC_Table_Snapshot_Compact* deserialize_from_vfs_chunk(const void* chunk_addr,
                                                                size_t chunk_bytes,
                                                                uint64_t mirc_read_key,
                                                                Discrete_TBT_Table* out_tbt_opt = nullptr);

  static uint64_t compute_cassette_digest(const void* payload, size_t bytes) noexcept;
};

#endif // SHARE_VAC_VAC_TABLESNAPSHOT_HPP
