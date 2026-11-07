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

#ifndef SHARE_GC_REDJET_API_REDJETJAVAAPIBRIDGE_HPP
#define SHARE_GC_REDJET_API_REDJETJAVAAPIBRIDGE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "jni.h"
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/puzzler/redjetTubeFeeder.hpp"
#include "gc/redjet/puzzler/redjetAosSoaMorph.hpp"
#include "gc/redjet/engine/redjetBREE.hpp"
#include "gc/redjet/api/redjetExplicitGC.hpp"

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
// Redjet Java API Bridge: C++ Engine for jdk.gc.redjet.* (PLAN.md 8.7, 8.8, 10.1)
// - RedjetTube: RAII streaming hardware prefetch conduit for arrays
// - PrecisionScope: RAII IEEE-754 precision target guard
// - RedjetBatchExec: Superloop weaving and batch memory scope entry/exit
// - ThrowGCHint: Non-STW explicit cleaning hints
// - @RJGC Off-Heap Tracker: ImmediateEviction & ContinuousFlowWatch
// ============================================================================

enum class RedjetOffHeapMode : uint8_t {
  IMMEDIATE_EVICTION   = 0, // Freed immediately upon method exit
  CONTINUOUS_FLOW_WATCH = 1  // Tracked in background and freed when ref dropped
};

struct alignas(64) RedjetOffHeapEntry {
  uintptr_t            address{ 0 };
  size_t               size_bytes{ 0 };
  RedjetOffHeapMode    mode{ RedjetOffHeapMode::IMMEDIATE_EVICTION };
  uint64_t             registration_tsc{ 0 };
  std::atomic<bool>    is_active{ false };
};

class RedjetJavaAPIBridge : public CHeapObj<mtGC> {
public:
  static constexpr size_t MAX_OFFHEAP_TRACKED = 2048;

private:
  RedjetTubeFeeder       _tube_feeder;
  RedjetOffHeapEntry     _offheap_table[MAX_OFFHEAP_TRACKED];
  hs_ticket_lock_t       _bridge_lock;

  // Telemetry counters
  std::atomic<uint64_t>  _total_tube_feeds{ 0 };
  std::atomic<uint64_t>  _total_precision_scopes{ 0 };
  std::atomic<uint64_t>  _total_batch_scopes{ 0 };
  std::atomic<uint64_t>  _total_gc_hints_handled{ 0 };
  std::atomic<uint64_t>  _total_offheap_immediate_freed{ 0 };
  std::atomic<uint64_t>  _total_offheap_flow_watched{ 0 };

public:
  RedjetJavaAPIBridge();
  ~RedjetJavaAPIBridge();

  static RedjetJavaAPIBridge& instance();

  void init();
  void destroy();

  // =========================================================================
  // 1. RedjetTube Streaming API (jdk.gc.redjet.RedjetTube)
  // =========================================================================
  RJ_API uint32_t open_streaming_tube(const void* array_base, size_t element_count, size_t stride);
  RJ_API void     step_streaming_tube(uint32_t channel_id, size_t chunk_size);
  RJ_API void     close_streaming_tube(uint32_t channel_id);

  // =========================================================================
  // 2. PrecisionTarget Scope API (jdk.gc.redjet.PrecisionScope)
  // =========================================================================
  RJ_API void enter_precision_scope(bool exact_mode);
  RJ_API void exit_precision_scope(bool exact_mode);

  // =========================================================================
  // 3. BatchExec Scope API (jdk.gc.redjet.RedjetBatchExec)
  // =========================================================================
  RJ_API bool enter_batch_execution(uint32_t scope_id, const char* name, bool morph, uint32_t prefetch);
  RJ_API bool exit_batch_execution(uint32_t scope_id);

  // =========================================================================
  // 4. Explicit GC Hint API (jdk.gc.redjet.JDK.ThrowGCHint)
  // =========================================================================
  RJ_API void on_explicit_gc_hint(uint32_t hint_flags);

  // =========================================================================
  // 5. @RJGC Off-Heap Management (jdk.gc.annotation.RJGC)
  // =========================================================================
  RJ_API bool register_offheap_memory(void* ptr, size_t size_bytes, RedjetOffHeapMode mode);
  RJ_API bool release_offheap_memory(void* ptr);
  RJ_API size_t sweep_offheap_immediate_entries();

  // JNI Native Method Registration
  RJ_API static void register_natives(JNIEnv* env);

  // Telemetry accessors
  uint64_t total_tube_feeds() const noexcept { return _total_tube_feeds.load(std::memory_order_relaxed); }
  uint64_t total_precision_scopes() const noexcept { return _total_precision_scopes.load(std::memory_order_relaxed); }
  uint64_t total_batch_scopes() const noexcept { return _total_batch_scopes.load(std::memory_order_relaxed); }
  uint64_t total_gc_hints() const noexcept { return _total_gc_hints_handled.load(std::memory_order_relaxed); }
  uint64_t total_offheap_immediate_freed() const noexcept { return _total_offheap_immediate_freed.load(std::memory_order_relaxed); }
  uint64_t total_offheap_flow_watched() const noexcept { return _total_offheap_flow_watched.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

extern RedjetJavaAPIBridge g_redjet_java_api_bridge;

#endif // SHARE_GC_REDJET_API_REDJETJAVAAPIBRIDGE_HPP
