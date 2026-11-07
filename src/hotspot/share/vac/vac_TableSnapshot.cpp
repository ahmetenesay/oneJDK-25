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

#include "vac/vac_TableSnapshot.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_REDJETGC
#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "memory/universe.hpp"
#endif

#include <cstdlib>
#include <cstring>

namespace {
static constexpr uint32_t VAC_TAPE_MAGIC   = 0x56414354u; // "VACT"
static constexpr uint32_t VAC_TAPE_VERSION = 0x00010000u; // v1.0

struct VAC_Tape_Trailer {
  uint64_t digest_fnv64;
  uint64_t sealed_key_tag;
  uint32_t has_tbt_table;
  uint32_t trailer_magic; // 0x56414345 ("VACE")
};
static constexpr uint32_t VAC_TRAILER_MAGIC = 0x56414345u;
} // namespace

uint64_t VAC_Table_Snapshot_Compact::compute_cassette_digest(const void* payload, size_t bytes) noexcept {
  if (payload == nullptr || bytes == 0) return 0ULL;
  const uint8_t* p = static_cast<const uint8_t*>(payload);
  uint64_t hash = 0xCBF29CE484222325ULL;
  for (size_t i = 0; i < bytes; ++i) {
    hash ^= static_cast<uint64_t>(p[i]);
    hash *= 0x100000001B3ULL;
  }
  // Final Murmur64 avalanche mix
  hash ^= hash >> 33;
  hash *= 0xFF51AFD7ED558CCDULL;
  hash ^= hash >> 33;
  return hash;
}

const uint8_t* VAC_Table_Snapshot_Compact::seal_rawcode_cell(uint32_t method_id,
                                                             const uint8_t* raw_bytecode,
                                                             size_t bytecode_len,
                                                             uint64_t vac_access_key) {
  if (raw_bytecode == nullptr || bytecode_len == 0) {
    return nullptr;
  }

#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    if (vac_access_key != RedjetVFSTree::KEY_VAC_READ) {
      log_warning(jit, compilation)("VAC rawcode_cell seal rejected for method %u: invalid KEY_VAC_READ", method_id);
      return nullptr;
    }
    if (Universe::is_fully_initialized()) {
      RedjetHeap* heap = RedjetHeap::heap();
      if (!heap->vfs_tree()->check_wx_access("/heap/off_heap/code_boot/rawcode_cell", vac_access_key)) {
        return nullptr;
      }
      heap->vfs_tree()->record_path_alloc("/heap/off_heap/code_boot/rawcode_cell", bytecode_len);
    }
    void* cell = RedjetCompressedSpaces::instance().allocate_rawcode_cell(bytecode_len, vac_access_key);
    if (cell != nullptr) {
      std::memcpy(cell, raw_bytecode, bytecode_len);
      return static_cast<const uint8_t*>(cell);
    }
  }
#else
  (void)method_id;
  (void)vac_access_key;
#endif

  // Universal GC fallback (G1 / ZGC / Shenandoah / Parallel / Serial)
  uint8_t* fallback_buf = NEW_C_HEAP_ARRAY(uint8_t, bytecode_len, mtCompiler);
  std::memcpy(fallback_buf, raw_bytecode, bytecode_len);
  return fallback_buf;
}

bool VAC_Table_Snapshot_Compact::serialize_to_vfs_chunk(const VAC_Table_Snapshot_Compact* snap,
                                                        const Discrete_TBT_Table* tbt_opt,
                                                        uint64_t mirc_seal_key,
                                                        void** out_chunk_addr,
                                                        size_t* out_chunk_bytes) {
  if (snap == nullptr || out_chunk_addr == nullptr || out_chunk_bytes == nullptr) {
    return false;
  }
  *out_chunk_addr  = nullptr;
  *out_chunk_bytes = 0;

  const size_t inst_bytes = static_cast<size_t>(snap->instruction_count) * sizeof(VAC_Instruction_Entry);
  const size_t tbt_bytes  = (tbt_opt != nullptr) ? sizeof(Discrete_TBT_Table) : 0;
  const size_t body_bytes = sizeof(VAC_Tape_Header) + inst_bytes + tbt_bytes;
  const size_t total_bytes = body_bytes + sizeof(VAC_Tape_Trailer);

  void* dest = nullptr;

#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    if (mirc_seal_key != RedjetVFSTree::KEY_MIRC_READ) {
      log_warning(jit, compilation)("VAC chunk serialization rejected for method %u: invalid KEY_MIRC_READ", snap->method_id);
      return false;
    }
    if (Universe::is_fully_initialized()) {
      RedjetHeap* heap = RedjetHeap::heap();
      if (!heap->vfs_tree()->check_wx_access("/heap/off_heap/code_boot/vac_processed_code_chunk", mirc_seal_key)) {
        return false;
      }
      heap->vfs_tree()->record_path_alloc("/heap/off_heap/code_boot/vac_processed_code_chunk", total_bytes);
    }
    dest = RedjetCompressedSpaces::instance().allocate_vac_chunk(total_bytes, mirc_seal_key);
  }
#endif

  if (dest == nullptr) {
    dest = NEW_C_HEAP_ARRAY(uint8_t, total_bytes, mtCompiler);
  }

  uint8_t* cursor = static_cast<uint8_t*>(dest);

  VAC_Tape_Header hdr{};
  hdr.magic                 = VAC_TAPE_MAGIC;
  hdr.version               = VAC_TAPE_VERSION;
  hdr.method_id             = snap->method_id;
  hdr.instruction_count     = snap->instruction_count;
  hdr.req                   = snap->req;
  hdr.has_no_alias          = snap->has_no_alias;
  hdr.arithmetic_intensity  = snap->arithmetic_intensity;
  hdr.unroll_recommendation = snap->unroll_recommendation;
  hdr.vectorization_hint    = snap->vectorization_hint;
  hdr.tbt_branch_count      = snap->tbt_branch_count;
  hdr.reserved              = 0;

  std::memcpy(cursor, &hdr, sizeof(hdr));
  cursor += sizeof(hdr);

  if (inst_bytes > 0 && snap->instructions != nullptr) {
    std::memcpy(cursor, snap->instructions, inst_bytes);
    cursor += inst_bytes;
  } else if (inst_bytes > 0) {
    std::memset(cursor, 0, inst_bytes);
    cursor += inst_bytes;
  }

  if (tbt_opt != nullptr) {
    std::memcpy(cursor, tbt_opt, sizeof(Discrete_TBT_Table));
    cursor += sizeof(Discrete_TBT_Table);
  }

  VAC_Tape_Trailer trailer{};
  trailer.digest_fnv64   = compute_cassette_digest(dest, body_bytes);
  trailer.sealed_key_tag = mirc_seal_key ^ 0xA5A5F00D56414354ULL;
  trailer.has_tbt_table  = (tbt_opt != nullptr) ? 1u : 0u;
  trailer.trailer_magic  = VAC_TRAILER_MAGIC;
  std::memcpy(cursor, &trailer, sizeof(trailer));

  *out_chunk_addr  = dest;
  *out_chunk_bytes = total_bytes;
  return true;
}

VAC_Table_Snapshot_Compact* VAC_Table_Snapshot_Compact::deserialize_from_vfs_chunk(const void* chunk_addr,
                                                                                   size_t chunk_bytes,
                                                                                   uint64_t mirc_read_key,
                                                                                   Discrete_TBT_Table* out_tbt_opt) {
  if (chunk_addr == nullptr || chunk_bytes < sizeof(VAC_Tape_Header) + sizeof(VAC_Tape_Trailer)) {
    return nullptr;
  }

#if INCLUDE_REDJETGC
  if (UseRedjetGC && mirc_read_key != RedjetVFSTree::KEY_MIRC_READ) {
    log_warning(jit, compilation)("MIRC attempted to read vac_processed_code_chunk without valid KEY_MIRC_READ");
    return nullptr;
  }
#endif

  const uint8_t* base = static_cast<const uint8_t*>(chunk_addr);
  VAC_Tape_Header hdr{};
  std::memcpy(&hdr, base, sizeof(hdr));
  if (hdr.magic != VAC_TAPE_MAGIC || hdr.version != VAC_TAPE_VERSION) {
    return nullptr;
  }

  const size_t inst_bytes = static_cast<size_t>(hdr.instruction_count) * sizeof(VAC_Instruction_Entry);
  if (chunk_bytes < sizeof(VAC_Tape_Header) + inst_bytes + sizeof(VAC_Tape_Trailer)) {
    return nullptr;
  }

  const size_t body_bytes = chunk_bytes - sizeof(VAC_Tape_Trailer);
  VAC_Tape_Trailer trailer{};
  std::memcpy(&trailer, base + body_bytes, sizeof(trailer));
  if (trailer.trailer_magic != VAC_TRAILER_MAGIC) {
    return nullptr;
  }
  if (trailer.sealed_key_tag != (mirc_read_key ^ 0xA5A5F00D56414354ULL)) {
    return nullptr;
  }
  if (compute_cassette_digest(base, body_bytes) != trailer.digest_fnv64) {
    return nullptr;
  }

  VAC_Table_Snapshot_Compact* snap = create(hdr.method_id, hdr.instruction_count);
  snap->req                   = hdr.req;
  snap->has_no_alias          = hdr.has_no_alias;
  snap->arithmetic_intensity  = hdr.arithmetic_intensity;
  snap->unroll_recommendation = hdr.unroll_recommendation;
  snap->vectorization_hint    = hdr.vectorization_hint;
  snap->tbt_branch_count      = hdr.tbt_branch_count;

  if (inst_bytes > 0 && snap->instructions != nullptr) {
    std::memcpy(snap->instructions, base + sizeof(VAC_Tape_Header), inst_bytes);
  }

  if (trailer.has_tbt_table != 0 && out_tbt_opt != nullptr) {
    const size_t tbt_offset = sizeof(VAC_Tape_Header) + inst_bytes;
    if (tbt_offset + sizeof(Discrete_TBT_Table) <= body_bytes) {
      std::memcpy(out_tbt_opt, base + tbt_offset, sizeof(Discrete_TBT_Table));
    }
  }

  return snap;
}
