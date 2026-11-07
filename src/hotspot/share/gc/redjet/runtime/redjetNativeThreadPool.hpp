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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETNATIVETHREADPOOL_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETNATIVETHREADPOOL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/globalDefinitions.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Bare-Metal Native FCS Task Slot & Ring-Buffer Thread Pool (PLAN.md Section 7.4)
// ============================================================================

struct alignas(64) NativeFCS_TaskSlot {
  std::atomic<void*> task_func;
  std::atomic<void*> task_data;
  uint32_t           core_affinity_mask;
  uint32_t           _pad;

  NativeFCS_TaskSlot() : task_func(nullptr), task_data(nullptr), core_affinity_mask(0), _pad(0) {}
};

class NativeFCS_ThreadPool {
public:
  static const size_t RING_SIZE = 1024;

private:
  NativeFCS_TaskSlot ring_buffer[RING_SIZE];
  alignas(64) std::atomic<uint64_t> head_idx{0};
  alignas(64) std::atomic<uint64_t> tail_idx{0};

public:
  NativeFCS_ThreadPool();

  RJ_API void push_task(void (*func)(void*), void* data, uint32_t affinity);
  RJ_API bool steal_task_for_core(uint32_t core_id, void** out_func, void** out_data);
  RJ_API size_t pending_tasks() const;
};

// ============================================================================
// Native Redjet Slab Allocator: 64KB Slices Atomic Bump Pointer Allocator
// ============================================================================

class NativeRedjet_SlabAllocator {
public:
  static const size_t SLAB_SIZE = 64 * 1024; // 64 KB

private:
  struct SlabHeader {
    std::atomic<size_t> bump_offset;
    SlabHeader*         next_slab;
    uint8_t             data[SLAB_SIZE - sizeof(std::atomic<size_t>) - sizeof(void*)];
  };

  std::atomic<SlabHeader*> _current_slab;
  std::atomic<size_t>      _total_allocated_bytes;

  SlabHeader* allocate_new_slab();

public:
  NativeRedjet_SlabAllocator();
  ~NativeRedjet_SlabAllocator();

  RJ_API void* allocate(size_t size_bytes, size_t alignment = 8);
  RJ_API void reset();
  RJ_API size_t total_allocated_bytes() const { return _total_allocated_bytes.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETNATIVETHREADPOOL_HPP
