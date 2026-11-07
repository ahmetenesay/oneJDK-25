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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETSLABPOOL_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETSLABPOOL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/allocator/redjetSlabMesh.hpp"

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
// Redjet High-Speed Tagged Freelist Slab Pool (PLAN.md Section 4.2)
// - ABA-mitigated lock-free freelist using 64-bit tagged pointers
// - Pre-carved 64KB slab blocks for all 10 OOH size classes
// - Sub-cycle allocation path for hotfield and fledgling
// ============================================================================

struct RedjetSlabSlot {
  RedjetSlabSlot* next{ nullptr };
};

struct alignas(64) RedjetSlabBlock {
  void*            block_memory{ nullptr };
  size_t           block_size{ 65536 }; // 64 KB block
  size_t           slot_size{ 64 };
  size_t           total_slots{ 1024 };
  std::atomic<size_t> allocated_slots{ 0 };
  RedjetSlabSlot*  freelist_head{ nullptr };
  RedjetSlabBlock* next{ nullptr };
};

class RedjetSlabPool {
public:
  static constexpr size_t NUM_CLASSES = static_cast<size_t>(RedjetSlabClass::CLASS_COUNT);
  static constexpr size_t DEFAULT_POOL_CAPACITY_BYTES = 64ULL * 1024 * 1024; // 64 MB

private:
  static RedjetSlabPool*   _instance;
  static std::atomic<bool> _initialized;

  RedjetSlabBlock*         _class_blocks[NUM_CLASSES]{ nullptr };
  hs_ticket_lock_t         _pool_locks[NUM_CLASSES];

  void*                    _pool_backing_memory{ nullptr };
  size_t                   _pool_capacity{ 0 };
  std::atomic<size_t>      _pool_carved_offset{ 0 };

  // Telemetry
  std::atomic<uint64_t>    _total_allocations{ 0 };
  std::atomic<uint64_t>    _total_deallocations{ 0 };
  std::atomic<uint64_t>    _total_blocks_carved{ 0 };

  static size_t class_to_slot_size(RedjetSlabClass sclass) noexcept;

public:
  RedjetSlabPool();
  ~RedjetSlabPool();

  static void initialize(size_t capacity_bytes = DEFAULT_POOL_CAPACITY_BYTES);
  static void destroy();
  static RedjetSlabPool* get() noexcept { return _instance; }

  // 1. Allocate Slot from appropriate OOH Class
  RJ_API void* allocate_slot(RedjetSlabClass sclass);

  // 2. Free Slot back to OOH Class Freelist
  RJ_API void  free_slot(void* ptr, RedjetSlabClass sclass);

  // Telemetry
  uint64_t allocations()   const noexcept { return _total_allocations.load(std::memory_order_relaxed); }
  uint64_t deallocations() const noexcept { return _total_deallocations.load(std::memory_order_relaxed); }
  uint64_t blocks_carved() const noexcept { return _total_blocks_carved.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETSLABPOOL_HPP
