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

#ifndef SHARE_GC_REDJET_API_REDJETFASTFFM_HPP
#define SHARE_GC_REDJET_API_REDJETFASTFFM_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/api/redjetOffHeapReclaimer.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// RedjetFastFFM — Zero-Copy Off-Heap <=> Native FFM Engine (Redjet-Exclusive)
// ============================================================================
// Architectural Split:
//   - JNI = On-Heap <=> Native (RedjetFastJNI)
//   - FFM = Off-Heap <=> Native (this subsystem)
//
// Strictly guarded by `if (UseRedjetGC)` so standard OpenJDK ForeignArena /
// Unsafe.allocateMemory behavior on G1/ZGC/Shenandoah is 100% preserved.
//
// Core Capabilities under -XX:+UseRedjetGC:
// 1. VFS `JAVA_MEMORY` Off-Heap Domain Allocation:
//    Allocates 64B cache-line aligned MemorySegments accounted directly under
//    VFS `JAVA_MEMORY` (`on_heap = false`), with OOM spike fallback into
//    `/heap/off_heap/reserved_boot` emergency locked RAM headroom.
// 2. Cleaner-Free @RJGC Lifecycle (`ImmediateEviction` & `ContinuousFlowWatch`):
//    Eliminates `java.lang.ref.Cleaner` + `PhantomReference` queue latency by
//    binding every FFM segment to `RedjetOffHeapReclaimer` and `TailTraces`
//    last-use dead clasps.
// 3. Bidirectional Zero-Copy Pointer Synchronization:
//    When C/C++ native code calls `realloc()` on an off-heap segment,
//    `sync_native_realloc()` atomically updates registered `MemorySegment`
//    base addresses via `RedjetNativeTracker` without copying payload bytes.
// ============================================================================

struct RedjetFastFFMTelemetry {
  uint64_t total_segments_allocated{ 0 };
  uint64_t total_segments_freed_immediate{ 0 };
  uint64_t total_segments_freed_flow_watch{ 0 };
  uint64_t total_reserved_boot_borrows{ 0 };
  uint64_t total_zero_copy_downcalls{ 0 };
  uint64_t total_bidirectional_realloc_syncs{ 0 };
  uint64_t active_offheap_java_bytes{ 0 };
};

struct alignas(64) RedjetFFMSegmentHeader {
  static constexpr uint64_t SEGMENT_MAGIC = 0x524A46464D534547ULL; // "RJFFMSEG"

  uint64_t                magic{ SEGMENT_MAGIC };
  void*                   raw_allocation_base{ nullptr };
  size_t                  payload_bytes{ 0 };
  size_t                  total_reserved_bytes{ 0 };
  uint32_t                alignment{ 64 };
  RedjetOffHeapMarkPolicy mark_policy{ RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION };
  bool                    from_reserved_boot{ false };
  uint8_t                 reserved_pad[2]{ 0, 0 };
  RedjetOffHeapDescriptor* reclaimer_desc{ nullptr };
};

class RedjetFastFFM {
private:
  static std::atomic<uint64_t> _total_segments_allocated;
  static std::atomic<uint64_t> _total_segments_freed_immediate;
  static std::atomic<uint64_t> _total_segments_freed_flow_watch;
  static std::atomic<uint64_t> _total_reserved_boot_borrows;
  static std::atomic<uint64_t> _total_zero_copy_downcalls;
  static std::atomic<uint64_t> _total_bidirectional_realloc_syncs;
  static std::atomic<uint64_t> _active_offheap_java_bytes;

  static void release_raw_segment_storage(void* payload_ptr, size_t sz);

public:
  RJ_API static void initialize();
  RJ_API static void shutdown();

  RJ_API static bool is_enabled() noexcept;

  // 1. Allocate 64B-aligned zero-copy off-heap MemorySegment under @RJGC policy
  RJ_API static void* allocate_segment(size_t byte_size,
                                       size_t byte_alignment = 64,
                                       RedjetOffHeapMarkPolicy policy = RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION,
                                       bool zero_initialize = true,
                                       uint32_t owner_tid = 0) noexcept;

  // 2. Deterministic ImmediateEviction free (invoked by JLC/VAC TailTraces epilogue or Arena.close)
  RJ_API static bool free_segment_immediate(void* segment_payload_ptr) noexcept;

  // 3. TailTraces dead-reference clasp hook for @RJGC off-heap handles
  RJ_API static void on_tailtrace_segment_dead(void* segment_payload_ptr) noexcept;

  // 4. Bidirectional Native -> Java realloc synchronization
  RJ_API static void* sync_native_realloc(void* old_payload_ptr, size_t new_byte_size) noexcept;

  // 5. Record zero-copy FFM linker downcall execution
  RJ_API static void record_zero_copy_downcall() noexcept {
    _total_zero_copy_downcalls.fetch_add(1, std::memory_order_relaxed);
  }

  // 6. Telemetry snapshot for `RedjetFastInteropStats` & SA
  RJ_API static RedjetFastFFMTelemetry snapshot_telemetry() noexcept;
};

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void* hs_redjet_ffm_allocate(size_t bytes, size_t alignment, const char* rjgc_mark);
RJ_API bool  hs_redjet_ffm_free_immediate(void* segment_ptr);
RJ_API void* hs_redjet_ffm_realloc_sync(void* old_segment_ptr, size_t new_bytes);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_API_REDJETFASTFFM_HPP
