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

#ifndef SHARE_GC_REDJET_API_REDJETTAILTRACES_HPP
#define SHARE_GC_REDJET_API_REDJETTAILTRACES_HPP

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
// TailTraces: Dead Reference Clasp Signaling to /heap/on_heap/rpss
// Controlled by -XX:+RedjetGCUseDeadReferenceSignals (PLAN.md Section 9.4)
// ============================================================================

struct alignas(64) RedjetClaspSignal {
  std::atomic<uint64_t> dead_oop_address;
  uint32_t              thread_id;
  uint32_t              region_id;
  uint64_t              timestamp_ns;
};

class RedjetTailTraces {
public:
  static const size_t RPSS_RING_CAPACITY = 4096;

private:
  RedjetClaspSignal _rpss_ring[RPSS_RING_CAPACITY];
  alignas(64) std::atomic<uint64_t> _write_cursor{0};
  alignas(64) std::atomic<uint64_t> _read_cursor{0};
  std::atomic<uint64_t> _total_signals_received{0};
  std::atomic<uint64_t> _total_signals_drained{0};

public:
  RedjetTailTraces();

  // Attaches a clasp to the current Java thread
  RJ_API void register_thread_clasp(uint32_t thread_id);

  // Emits dead reference signal when Liveness Dead End is reached (Zero STW)
  RJ_API void emit_dead_reference_signal(uint32_t thread_id, void* dead_obj, uint32_t region_id);

  // Drains pending clasp signals into Redjet GC mark bitmap / landfill
  RJ_API size_t drain_signals(void (*reclaim_callback)(void* obj, uint32_t region_id));

  RJ_API uint64_t total_signals() const { return _total_signals_received.load(std::memory_order_relaxed); }
  RJ_API uint64_t pending_signals() const;
};

extern RedjetTailTraces g_redjet_tail_traces;

#endif // SHARE_GC_REDJET_API_REDJETTAILTRACES_HPP
