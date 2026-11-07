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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_HEADERPANE_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_HEADERPANE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/vfs/redjetVFSNode.hpp"
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
// Header Pane Controller (/heap/on_heap/landfill/header_pane) (PLAN.md Section 9.1)
// - Isolated 8-byte metaspace compact object header storage
// - Prevents object metadata headers from polluting hot user-data cache lines
// - 32-bit Compact Header ID <-> 8-byte Header bi-directional indexing
// - SIMD batch mark & liveness scanning across contiguous headers
// ============================================================================

struct alignas(8) RedjetCompactHeader8 {
  uint32_t klass_id{ 0 };     // Compressed Klass pointer / ID
  uint16_t mark_bits{ 0 };    // GC mark state, lock bits, age bits
  uint8_t  vfs_origin_tier{ 0 }; // Origin VFS tier (egg/fledgling/hotfield)
  uint8_t  flags{ 0 };        // Pinned, morph, dead-clasp flags

  inline bool is_marked() const noexcept {
    return (mark_bits & 0x01) != 0;
  }

  inline void set_marked(bool marked) noexcept {
    if (marked) mark_bits |= 0x01;
    else        mark_bits &= ~0x01;
  }
};

class RedjetVFSNode_HeaderPane : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_HEADER_CAPACITY = 1024 * 1024; // 1M headers (8 MB)
  static constexpr size_t DEFAULT_QUOTA = 128ULL * 1024 * 1024; // 128 MB quota

private:
  RedjetCompactHeader8* _headers{ nullptr };
  size_t                _capacity{ 0 };
  std::atomic<size_t>   _allocated_count{ 0 };
  hs_ticket_lock_t      _header_lock;

  // Global telemetry
  std::atomic<uint64_t> _total_headers_registered{ 0 };
  std::atomic<uint64_t> _total_headers_released{ 0 };
  std::atomic<uint64_t> _total_batch_scans{ 0 };
  std::atomic<uint64_t> _total_headers_marked{ 0 };

public:
  RedjetVFSNode_HeaderPane(const char* name = "header_pane",
                           const char* full_path = "/heap/on_heap/landfill/header_pane",
                           RedjetVFSNode* parent = nullptr,
                           size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_HeaderPane();

  void init(void* storage, size_t header_capacity);
  void destroy();

  // 1. Header Registration & ID Mapping
  RJ_API uint32_t register_header(uint32_t klass_id, uint8_t origin_tier, uint8_t flags = 0);
  RJ_API void     release_header(uint32_t header_id);

  // Direct 8-byte header lookup
  RJ_API RedjetCompactHeader8* get_header(uint32_t header_id) noexcept;
  RJ_API const RedjetCompactHeader8* get_header(uint32_t header_id) const noexcept;

  // 2. SIMD Batch Header Mark Scanning
  // Scans contiguous 8-byte headers using 512-bit SIMD registers (8 headers per vector)
  RJ_API size_t scan_and_mark_batch(uint32_t start_header_id, size_t count);

  // Clears mark bits across entire header pane
  RJ_API void reset_all_marks();

  // Telemetry accessors
  size_t   capacity() const noexcept { return _capacity; }
  size_t   allocated_headers() const noexcept { return _allocated_count.load(std::memory_order_relaxed); }
  uint64_t total_registered() const noexcept { return _total_headers_registered.load(std::memory_order_relaxed); }
  uint64_t total_released() const noexcept { return _total_headers_released.load(std::memory_order_relaxed); }
  uint64_t total_scans() const noexcept { return _total_batch_scans.load(std::memory_order_relaxed); }
  uint64_t total_marked() const noexcept { return _total_headers_marked.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_HEADERPANE_HPP
