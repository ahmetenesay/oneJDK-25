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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETHANDSHAKEGOVERNOR_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETHANDSHAKEGOVERNOR_HPP

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
// Redjet Deterministic Handshake Governor (PLAN.md Section 7.1)
// - Coordinates Thread-Local Direct Handshakes without Global STW Halts
// - 99% of mutator threads run uninterrupted while targeted thread executes closure
// - High-speed nanosecond acknowledgment ring
// ============================================================================

typedef void (*RedjetHandshakeClosure)(void* context, uint32_t target_thread_id);

enum class RedjetHandshakeState : uint8_t {
  HANDSHAKE_IDLE       = 0,
  HANDSHAKE_REQUESTED  = 1,
  HANDSHAKE_PROCESSING = 2,
  HANDSHAKE_COMPLETED  = 3
};

struct alignas(64) RedjetHandshakeSlot {
  uint32_t                target_thread_id{ 0 };
  RedjetHandshakeClosure  closure{ nullptr };
  void*                   context{ nullptr };
  RedjetHandshakeState    state{ RedjetHandshakeState::HANDSHAKE_IDLE };
  uint64_t                request_tsc{ 0 };
  uint64_t                complete_tsc{ 0 };
};

class RedjetHandshakeGovernor {
public:
  static constexpr size_t MAX_THREADS_HANDSHAKE = 256;

private:
  static RedjetHandshakeGovernor* _instance;
  static std::atomic<bool>        _initialized;

  RedjetHandshakeSlot             _slots[MAX_THREADS_HANDSHAKE];
  hs_ticket_lock_t                _handshake_lock;

  // Telemetry
  std::atomic<uint64_t>           _total_handshakes_requested{ 0 };
  std::atomic<uint64_t>           _total_handshakes_completed{ 0 };
  std::atomic<uint64_t>           _total_handshake_latency_cycles{ 0 };

public:
  RedjetHandshakeGovernor();
  ~RedjetHandshakeGovernor();

  static void initialize();
  static void destroy();
  static RedjetHandshakeGovernor* get() noexcept { return _instance; }

  // 1. Request Direct Handshake on Target Thread
  RJ_API bool request_handshake(uint32_t target_thread_id, RedjetHandshakeClosure closure, void* ctx);

  // 2. Poll & Execute Handshake (called by target thread at dock point)
  RJ_API bool check_and_execute_handshake(uint32_t target_thread_id);

  // 3. Await Handshake Completion
  RJ_API bool wait_for_handshake(uint32_t target_thread_id, uint32_t timeout_micros);

  // Telemetry
  uint64_t handshakes_requested() const noexcept { return _total_handshakes_requested.load(std::memory_order_relaxed); }
  uint64_t handshakes_completed() const noexcept { return _total_handshakes_completed.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETHANDSHAKEGOVERNOR_HPP
