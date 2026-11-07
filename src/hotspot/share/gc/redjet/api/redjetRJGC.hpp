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

#ifndef SHARE_GC_REDJET_API_REDJETRJGC_HPP
#define SHARE_GC_REDJET_API_REDJETRJGC_HPP

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
// @RJGC: Smart Off-Heap Lifecycle Manager (PLAN.md Section 10.1)
// ============================================================================

enum class RJGCMode : uint8_t {
  IMMEDIATE_EVICTION    = 0, // Instant return to OS upon method scope exit
  CONTINUOUS_FLOW_WATCH = 1  // Silent E-Core background monitoring until ref_count == 0
};

struct alignas(64) RJGCTrackedHandle {
  void*                off_heap_ptr;
  size_t               size_bytes;
  RJGCMode             mode;
  std::atomic<int32_t> ref_count;
  void (*custom_destructor)(void* ptr, size_t sz);
  RJGCTrackedHandle*   next;
  std::atomic<bool>    is_evicted;
  bool                 in_pool;

  RJGCTrackedHandle() : off_heap_ptr(nullptr), size_bytes(0), mode(RJGCMode::IMMEDIATE_EVICTION),
                        ref_count(0), custom_destructor(nullptr), next(nullptr),
                        is_evicted(false), in_pool(false) {}
};

class RedjetRJGC {
public:
  static const size_t MAX_TRACKED_HANDLES = 8192;

private:
  static RJGCTrackedHandle               _handle_pool[MAX_TRACKED_HANDLES];
  static std::atomic<RJGCTrackedHandle*> _free_head;
  static std::atomic<bool>               _pool_initialized;
  static std::atomic<RJGCTrackedHandle*> _head_handle;
  static std::atomic<size_t>             _total_registered;
  static std::atomic<size_t>             _total_evicted_bytes;

  static void ensure_pool_initialized();
  static RJGCTrackedHandle* acquire_handle();
  static void recycle_handle(RJGCTrackedHandle* handle);

public:
  // Registers an off-heap native memory allocation under @RJGC lifecycle management
  RJ_API static RJGCTrackedHandle* register_off_heap(void* ptr, size_t size, RJGCMode mode, void (*dtor)(void*, size_t) = nullptr);

  // Instantly returns off-heap memory to OS / allocator upon method exit
  RJ_API static void evict_immediate(RJGCTrackedHandle* handle);

  // Retains or releases reference on ContinuousFlowWatch handles
  RJ_API static void retain_handle(RJGCTrackedHandle* handle);
  RJ_API static void release_handle(RJGCTrackedHandle* handle);

  // Background E-Core worker sweep: evicts zero-ref ContinuousFlowWatch allocations
  RJ_API static size_t poll_and_evict_continuous();

  RJ_API static size_t total_evicted_bytes() { return _total_evicted_bytes.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_API_REDJETRJGC_HPP
