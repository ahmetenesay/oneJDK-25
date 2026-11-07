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

#ifndef SHARE_MIRC_FCS_ARCHIVE_HPP
#define SHARE_MIRC_FCS_ARCHIVE_HPP

#include <cstdint>
#include <cstddef>
#include "utilities/globalDefinitions.hpp"
#include "vac/vac_TableSnapshot.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#define FCSA_MAGIC   0x46435341 // "FCSA"
#define FCSA_VERSION 0x00010000 // v1.0

struct alignas(64) FCSA_Header {
  uint32_t magic;
  uint32_t version;
  uint64_t jar_fingerprint_hash;
  uint64_t cpu_fingerprint_hash;
  uint32_t total_methods_count;
  uint32_t hot_methods_count;
  uint64_t layer1_vac_offset;
  uint64_t layer1_vac_size;
  uint64_t layer2_arrt_offset;
  uint64_t layer2_arrt_size;
  uint64_t layer3_tbt_offset;
  uint64_t layer3_tbt_size;
  uint64_t layer4_code_offset;
  uint64_t layer4_code_size;
  uint32_t crc32_checksum;
  uint32_t reserved;
};

struct FCSA_Method_Entry {
  uint32_t method_id;
  char     method_name[128];
  uint32_t hotness_rank;
  uint32_t instruction_count;
  uint32_t code_size_bytes;
  uint64_t vac_tape_offset;
  uint64_t code_buffer_offset;
  Discrete_TBT_Table tbt_table;
};

// ============================================================================
// FCS Archive Engine: Fast Ahead-Of-Warmup (.fcsa) Serialization & mmap Loader
// ============================================================================

class FCSArchive {
public:
  RJ_API static uint64_t compute_jar_hash(const char* jar_path);
  RJ_API static uint64_t compute_cpu_fingerprint();

  RJ_API static bool save_archive(const char* filepath,
                                  const FCSA_Header& header,
                                  const FCSA_Method_Entry* methods,
                                  const void* vac_data, size_t vac_size,
                                  const void* code_data, size_t code_size);

  RJ_API static bool load_archive_mmap(const char* filepath,
                                       uint64_t expected_jar_hash,
                                       void** out_mapped_base,
                                       size_t* out_mapped_size,
                                       const FCSA_Header** out_header);
};

#endif // SHARE_MIRC_FCS_ARCHIVE_HPP
