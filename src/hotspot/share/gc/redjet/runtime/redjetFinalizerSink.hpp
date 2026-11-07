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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETFINALIZERSINK_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETFINALIZERSINK_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
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
// Redjet Lock-Free Finalizer & Resurrection Sink (PLAN.md Section 9.1)
// - Handles Trash Pit Rehabilitation (/heap/on_heap/landfill/trash_pit/rehab)
// - Lock-free multi-producer single-consumer (MPSC) resurrection queue
// - Weak, Soft, Phantom, and Cleaner reference notification pipeline
// - Zero mutator stalling during finalizer invocation
// ============================================================================

enum class RedjetRefKind : uint8_t {
  REF_SOFT      = 0,
  REF_WEAK      = 1,
  REF_FINALIZER = 2,
  REF_PHANTOM   = 3,
  REF_CLEANER   = 4
};

struct alignas(32) RedjetRehabNode {
  void*               object_address{ nullptr };
  RedjetRefKind       kind{ RedjetRefKind::REF_FINALIZER };
  uint32_t            klass_id{ 0 };
  uint64_t            enqueue_tsc{ 0 };
  RedjetRehabNode*    next{ nullptr };
};

class RedjetFinalizerSink {
public:
  static constexpr size_t MAX_DRAIN_BATCH = 1024;

private:
  static RedjetFinalizerSink*       _instance;
  static std::atomic<bool>          _initialized;

  std::atomic<RedjetRehabNode*>     _queue_head{ nullptr };
  std::atomic<size_t>               _queue_depth{ 0 };
  hs_ticket_lock_t                  _sink_lock;

  // Telemetry
  std::atomic<uint64_t>             _total_references_enqueued{ 0 };
  std::atomic<uint64_t>             _total_objects_resurrected{ 0 };
  std::atomic<uint64_t>             _total_cleaners_dispatched{ 0 };
  std::atomic<uint64_t>             _total_drains_executed{ 0 };

public:
  RedjetFinalizerSink();
  ~RedjetFinalizerSink();

  static void initialize();
  static void destroy();
  static RedjetFinalizerSink* get() noexcept { return _instance; }

  // 1. Lock-free Enqueue Object into Rehab Queue
  RJ_API void enqueue_reference(void* obj_addr, RedjetRefKind kind, uint32_t klass_id);

  // 2. Drain Batch from Rehab Queue (executed by background GC worker)
  RJ_API size_t drain_rehab_batch(RedjetRehabNode** out_batch, size_t max_items);

  // 3. Mark object as Resurrected (cancels uncommit and promotes to fledgling)
  RJ_API bool resurrect_object(void* obj_addr);

  // Status queries
  size_t queue_depth() const noexcept { return _queue_depth.load(std::memory_order_relaxed); }

  // Telemetry
  uint64_t references_enqueued() const noexcept { return _total_references_enqueued.load(std::memory_order_relaxed); }
  uint64_t objects_resurrected() const noexcept { return _total_objects_resurrected.load(std::memory_order_relaxed); }
  uint64_t cleaners_dispatched() const noexcept { return _total_cleaners_dispatched.load(std::memory_order_relaxed); }
  uint64_t drains_executed()     const noexcept { return _total_drains_executed.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETFINALIZERSINK_HPP
