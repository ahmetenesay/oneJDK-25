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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_RAWCODECELL_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_RAWCODECELL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/vfs/redjetVFSCrypto.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// Raw Code Cell Controller (/heap/off_heap/code_boot/rawcode_cell) (PLAN.md Section 9.1 & 9.2)
// - Cryptographically sealed bytecode storage for raw JVM classes
// - Sealed with KEY_VAC_READ before ingestion into VAC
// - Enforces immutable W^X read-only protection
// - Cryptographic integrity validation via 7-key derivation
// ============================================================================

struct alignas(64) RedjetRawCodeCellHeader {
  uint32_t method_id{ 0 };
  uint32_t class_id{ 0 };
  size_t   bytecode_size{ 0 };
  size_t   allocated_size{ 0 };
  uint64_t crypto_seal{ 0 };
  uint64_t timestamp_tsc{ 0 };
  bool     is_sealed{ false };
  uint8_t  padding[23]{ 0 };

  void* bytecode_data() noexcept {
    return reinterpret_cast<uint8_t*>(this) + sizeof(RedjetRawCodeCellHeader);
  }

  const void* bytecode_data() const noexcept {
    return reinterpret_cast<const uint8_t*>(this) + sizeof(RedjetRawCodeCellHeader);
  }
};

class RedjetVFSNode_RawCodeCell : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_CELL_QUOTA = 64ULL * 1024 * 1024; // 64 MB
  static constexpr size_t CELL_HEADER_SIZE   = sizeof(RedjetRawCodeCellHeader);
  static constexpr size_t MAX_CELLS_TRACKED   = 32768;

private:
  RedjetRawCodeCellHeader* _cells[MAX_CELLS_TRACKED]{ nullptr };
  size_t                   _cell_count{ 0 };
  hs_ticket_lock_t         _cell_lock;

  // Cryptographic master key for VAC sealing
  uint64_t                 _vac_read_key{ 0 };

  // Telemetry
  std::atomic<uint64_t>    _total_cells_allocated{ 0 };
  std::atomic<uint64_t>    _total_bytes_sealed{ 0 };
  std::atomic<uint64_t>    _total_seals_verified{ 0 };
  std::atomic<uint64_t>    _total_verification_failures{ 0 };
  std::atomic<uint64_t>    _total_cells_released{ 0 };

public:
  RedjetVFSNode_RawCodeCell(const char* name = "rawcode_cell",
                            const char* full_path = "/heap/off_heap/code_boot/rawcode_cell",
                            RedjetVFSNode* parent = nullptr,
                            size_t quota_bytes = DEFAULT_CELL_QUOTA);
  virtual ~RedjetVFSNode_RawCodeCell();

  void init(uint64_t vac_read_key = 0);
  void destroy();

  // 1. Raw Code Cell Allocation & Ingestion
  RJ_API RedjetRawCodeCellHeader* allocate_cell(uint32_t class_id, uint32_t method_id,
                                                const uint8_t* bytecode, size_t size);

  // 2. Cryptographic Sealing (transitions to W^X read-only with KEY_VAC_READ seal)
  RJ_API bool seal_cell(RedjetRawCodeCellHeader* cell, uint64_t vac_key);

  // 3. Cryptographic Verification
  RJ_API bool verify_cell_seal(const RedjetRawCodeCellHeader* cell, uint64_t vac_key) noexcept;

  // 4. Cell Release
  RJ_API bool release_cell(RedjetRawCodeCellHeader* cell);

  // Cell lookup by method ID
  RJ_API RedjetRawCodeCellHeader* find_cell_by_method(uint32_t method_id) noexcept;

  // Telemetry accessors
  size_t   tracked_cell_count() const noexcept { return _cell_count; }
  uint64_t cells_allocated()    const noexcept { return _total_cells_allocated.load(std::memory_order_relaxed); }
  uint64_t bytes_sealed()       const noexcept { return _total_bytes_sealed.load(std::memory_order_relaxed); }
  uint64_t seals_verified()     const noexcept { return _total_seals_verified.load(std::memory_order_relaxed); }
  uint64_t failures()           const noexcept { return _total_verification_failures.load(std::memory_order_relaxed); }
  uint64_t cells_released()     const noexcept { return _total_cells_released.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_RAWCODECELL_HPP
