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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETCOMPRESSEDSPACES_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETCOMPRESSEDSPACES_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#else
class outputStream;
#endif

// ============================================================================
// RedjetCompressedSpaces
//
// Architectural Isolation & Compressed Pointer / Klass Encoding for RedjetGC:
// 1. Strict Virtual Address Segregation:
//    Ensures Redjet VFS `/heap/off_heap/code_boot` (`rawcode_cell`,
//    `vac_processed_code_chunk`, `mirc_execution_ready_page/lvl1`, `lvl2`)
//    and VFS `/heap/on_heap/landfill/header_pane` NEVER collide or alias with
//    legacy HotSpot `CodeCache`/`CodeHeap` or `CompressedClassSpace` (which
//    remain 100% intact for G1/ZGC/Shenandoah/Parallel/Serial).
//
// 2. Redjet `HeaderPane` NarrowKlass Encoding (32-bit `narrowKlass`):
//    Encodes `Klass*` and veteran/retired class headers directly inside
//    `/heap/on_heap/landfill/header_pane` (`hotspot_memory` domain) with:
//      - Mode 0 (`ZERO_BASE_ZERO_SHIFT`): `< 4 GB` low VA (0-cycle decode)
//      - Mode 1 (`BASE_ZERO_SHIFT`):      `<= 4 GB` span at `header_pane_base`
//      - Mode 2 (`BASE_SHIFT_3`):         `<= 32 GB` span (8-byte alignment)
//      - Mode 3 (`BASE_SHIFT_6_CL`):      `<= 256 GB` span (64-byte cacheline!)
//
// 3. Redjet Unified On-Heap NarrowOop Encoding (32-bit `narrowOop`):
//    Supports 0-shift (`<= 4 GB`), 3-shift (`<= 32 GB`), and Redjet-exclusive
//    6-shift 64-byte cacheline-aligned slabs (`<= 256 GB`), seamlessly falling
//    back to 64-bit unscaled oops on > 256 GB heaps.
// ============================================================================

enum class RedjetCompressedMode : uint8_t {
  UNCOMPRESSED_64BIT   = 0,
  ZERO_BASE_ZERO_SHIFT = 1, // ptr = (uintptr_t)narrow
  ZERO_BASE_SHIFT_3    = 2, // ptr = (uintptr_t)narrow << 3
  BASE_ZERO_SHIFT      = 3, // ptr = base + (uintptr_t)narrow
  BASE_SHIFT_3         = 4, // ptr = base + ((uintptr_t)narrow << 3)
  BASE_SHIFT_6_CL      = 5  // ptr = base + ((uintptr_t)narrow << 6) [64B Cacheline!]
};

struct RedjetCodeBootPartition {
  uintptr_t arena_base;
  size_t    arena_reserved_bytes;
  uintptr_t rawcode_cell_base;
  size_t    rawcode_cell_bytes;
  uintptr_t vac_chunk_base;
  size_t    vac_chunk_bytes;
  uintptr_t mirc_lvl1_base;
  size_t    mirc_lvl1_bytes;
  uintptr_t mirc_lvl2_base;
  size_t    mirc_lvl2_bytes;
  bool      isolated_from_legacy_codecache;
  bool      isolated_from_legacy_ccs;
};

class RedjetCompressedSpaces {
public:
  static constexpr size_t DEFAULT_HEADER_PANE_BYTES = 1ULL * 1024 * 1024 * 1024; // 1 GB
  static constexpr size_t DEFAULT_CODE_BOOT_BYTES   = 256ULL * 1024 * 1024;      // 256 MB

private:
  // On-Heap Compressed Oops state
  uintptr_t             _oop_base{ 0 };
  int                   _oop_shift{ 0 };
  size_t                _oop_span_bytes{ 0 };
  RedjetCompressedMode  _oop_mode{ RedjetCompressedMode::UNCOMPRESSED_64BIT };

  // HeaderPane Compressed Klass state (/heap/on_heap/landfill/header_pane)
  uintptr_t             _klass_base{ 0 };
  int                   _klass_shift{ 0 };
  size_t                _klass_reserved_bytes{ 0 };
  std::atomic<size_t>   _klass_used_bytes{ 0 };
  RedjetCompressedMode  _klass_mode{ RedjetCompressedMode::BASE_SHIFT_3 };
  bool                  _owns_header_pane_arena{ false };

  // Isolated VFS /heap/off_heap/code_boot partition
  RedjetCodeBootPartition _code_boot{};
  std::atomic<size_t>     _rawcode_used{ 0 };
  std::atomic<size_t>     _vac_chunk_used{ 0 };
  std::atomic<size_t>     _mirc_lvl1_used{ 0 };
  std::atomic<size_t>     _mirc_lvl2_used{ 0 };
  bool                    _owns_code_boot_arena{ false };
  std::atomic<bool>       _initialized{ false };

  // Legacy HotSpot spaces recorded solely to guarantee 100% disjointness
  uintptr_t             _legacy_codecache_low{ 0 };
  uintptr_t             _legacy_codecache_high{ 0 };
  uintptr_t             _legacy_ccs_low{ 0 };
  uintptr_t             _legacy_ccs_high{ 0 };

  static inline bool ranges_disjoint(uintptr_t a_lo, uintptr_t a_hi,
                                     uintptr_t b_lo, uintptr_t b_hi) noexcept {
    if (a_lo == a_hi || b_lo == b_hi) return true;
    return (a_hi <= b_lo) || (b_hi <= a_lo);
  }

public:
  RedjetCompressedSpaces() = default;
  ~RedjetCompressedSpaces() { shutdown(); }

  static RedjetCompressedSpaces& instance() noexcept;

  RJ_API bool initialize(uintptr_t on_heap_low,
                         size_t on_heap_reserved_bytes,
                         size_t requested_code_boot_bytes = DEFAULT_CODE_BOOT_BYTES,
                         size_t requested_header_pane_bytes = DEFAULT_HEADER_PANE_BYTES);

  RJ_API void shutdown() noexcept;
  bool is_initialized() const noexcept { return _initialized.load(std::memory_order_acquire); }

  // =========================================================================
  // 1. O(1) NarrowKlass Encode / Decode (/heap/on_heap/landfill/header_pane)
  // =========================================================================
  inline uint32_t encode_klass_not_null(const void* klass_ptr) const noexcept {
    uintptr_t addr = reinterpret_cast<uintptr_t>(klass_ptr);
    return static_cast<uint32_t>((addr - _klass_base) >> _klass_shift);
  }

  inline uint32_t encode_klass(const void* klass_ptr) const noexcept {
    return (klass_ptr == nullptr) ? 0u : encode_klass_not_null(klass_ptr);
  }

  inline void* decode_klass_not_null(uint32_t narrow_klass) const noexcept {
    return reinterpret_cast<void*>(_klass_base + (static_cast<uintptr_t>(narrow_klass) << _klass_shift));
  }

  inline void* decode_klass(uint32_t narrow_klass) const noexcept {
    return (narrow_klass == 0u) ? nullptr : decode_klass_not_null(narrow_klass);
  }

  // Bump-allocate metadata / veteran header in HeaderPane (hotspot_memory domain)
  RJ_API void* allocate_in_header_pane(size_t bytes, size_t alignment = 64) noexcept;

  // =========================================================================
  // 2. O(1) NarrowOop Encode / Decode (Unified Redjet On-Heap)
  // =========================================================================
  inline bool use_compressed_oops() const noexcept {
    return _oop_mode != RedjetCompressedMode::UNCOMPRESSED_64BIT;
  }

  inline uint32_t encode_oop_not_null(const void* oop_ptr) const noexcept {
    uintptr_t addr = reinterpret_cast<uintptr_t>(oop_ptr);
    return static_cast<uint32_t>((addr - _oop_base) >> _oop_shift);
  }

  inline uint32_t encode_oop(const void* oop_ptr) const noexcept {
    return (oop_ptr == nullptr) ? 0u : encode_oop_not_null(oop_ptr);
  }

  inline void* decode_oop_not_null(uint32_t narrow_oop) const noexcept {
    return reinterpret_cast<void*>(_oop_base + (static_cast<uintptr_t>(narrow_oop) << _oop_shift));
  }

  inline void* decode_oop(uint32_t narrow_oop) const noexcept {
    return (narrow_oop == 0u) ? nullptr : decode_oop_not_null(narrow_oop);
  }

  // =========================================================================
  // 3. Isolated VFS /heap/off_heap/code_boot Sub-Arena Allocation
  // =========================================================================
  RJ_API void* allocate_rawcode_cell(size_t bytes, uint64_t vac_key) noexcept;
  RJ_API void* allocate_vac_chunk(size_t bytes, uint64_t mirc_key) noexcept;
  RJ_API void* allocate_mirc_ready_page(size_t bytes, bool is_lvl2) noexcept;

  // Verify zero overlap with legacy HotSpot CodeCache & CompressedClassSpace
  RJ_API bool verify_isolation_invariants() const noexcept;

  // Accessors for SA, JMX, and Assembler codegen
  uintptr_t            oop_base() const noexcept               { return _oop_base; }
  int                  oop_shift() const noexcept              { return _oop_shift; }
  RedjetCompressedMode oop_mode() const noexcept               { return _oop_mode; }
  uintptr_t            klass_base() const noexcept             { return _klass_base; }
  int                  klass_shift() const noexcept            { return _klass_shift; }
  size_t               klass_reserved_bytes() const noexcept   { return _klass_reserved_bytes; }
  size_t               klass_used_bytes() const noexcept       { return _klass_used_bytes.load(std::memory_order_relaxed); }
  RedjetCompressedMode klass_mode() const noexcept             { return _klass_mode; }
  const RedjetCodeBootPartition& code_boot() const noexcept    { return _code_boot; }
  size_t               code_boot_used_bytes() const noexcept {
    return _rawcode_used.load(std::memory_order_relaxed) +
           _vac_chunk_used.load(std::memory_order_relaxed) +
           _mirc_lvl1_used.load(std::memory_order_relaxed) +
           _mirc_lvl2_used.load(std::memory_order_relaxed);
  }

  static const char* mode_name(RedjetCompressedMode m) noexcept;

#ifndef REDJET_STANDALONE_BUILD
  void print_on(outputStream* st) const;
#endif
};

extern RedjetCompressedSpaces g_redjet_compressed_spaces;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API int HS_hotspot_code_heap_query(size_t* out_reserved_bytes,
                                      size_t* out_used_bytes,
                                      uintptr_t* out_base_addr);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETCOMPRESSEDSPACES_HPP
