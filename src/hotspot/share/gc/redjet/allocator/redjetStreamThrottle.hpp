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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETSTREAMTHROTTLE_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETSTREAMTHROTTLE_HPP

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
// Redjet Hardware Memory Stream Throttle (PLAN.md Section 4.4, 5.4)
// - Regulates hardware prefetch streams to protect mutator L1/L2 cache lines
// - Cooperates with MLTC stream_paused bus congestion backoff
// - Dynamically limits background GC thread stream concurrency per CCD
// - Token bucket bandwidth limiter and spin-pause damping
// ============================================================================

enum class RedjetStreamPriority : uint8_t {
  STREAM_MUTATOR_CRITICAL = 0, // Unthrottled highest priority
  STREAM_GC_FOREGROUND    = 1, // Paced during heavy congestion
  STREAM_GC_BACKGROUND    = 2  // Aggressively throttled background scrubber
};

struct alignas(64) RedjetStreamDescriptor {
  uint32_t             stream_id{ 0 };
  uint32_t             ccd_id{ 0 };
  RedjetStreamPriority priority{ RedjetStreamPriority::STREAM_GC_BACKGROUND };
  bool                 is_active{ false };
  uint64_t             bytes_streamed{ 0 };
  uint64_t             pauses_injected{ 0 };
  uint64_t             last_transfer_tsc{ 0 };
  size_t               bandwidth_token_bucket{ 1024 * 1024 }; // Tokens for rate limiting
};

class RedjetStreamThrottle {
public:
  static constexpr size_t MAX_CONCURRENT_STREAMS = 32;

private:
  static RedjetStreamThrottle* _instance;
  static std::atomic<bool>     _initialized;

  RedjetStreamDescriptor       _streams[MAX_CONCURRENT_STREAMS];
  std::atomic<uint32_t>        _active_streams_count{ 0 };
  std::atomic<bool>            _throttle_engaged{ false };
  std::atomic<uint32_t>        _global_pacing_factor{ 1 };
  hs_ticket_lock_t             _throttle_lock;

  // Telemetry
  std::atomic<uint64_t>        _total_streams_opened{ 0 };
  std::atomic<uint64_t>        _total_streams_closed{ 0 };
  std::atomic<uint64_t>        _total_pauses_injected{ 0 };
  std::atomic<uint64_t>        _total_bytes_streamed{ 0 };
  std::atomic<uint64_t>        _total_throttle_cycles_damped{ 0 };

public:
  RedjetStreamThrottle();
  ~RedjetStreamThrottle();

  static void initialize();
  static void destroy();
  static RedjetStreamThrottle* get() noexcept { return _instance; }

  // 1. Open / Close Stream Channel
  RJ_API uint32_t open_stream(uint32_t ccd_id, RedjetStreamPriority priority);
  RJ_API void     close_stream(uint32_t stream_id);

  // 2. Pace Stream Step (injects pause if bus is congested)
  RJ_API void pace_stream(uint32_t stream_id, size_t bytes_transferred);

  // 3. Set Global Congestion Flag (called by MLTC / Sensors)
  RJ_API void set_global_congestion_throttle(bool engage, uint32_t pacing_multiplier = 2) noexcept;

  // Query state
  bool is_throttling_engaged() const noexcept { return _throttle_engaged.load(std::memory_order_relaxed); }
  uint32_t active_streams_count() const noexcept { return _active_streams_count.load(std::memory_order_relaxed); }

  // Telemetry
  uint64_t streams_opened()   const noexcept { return _total_streams_opened.load(std::memory_order_relaxed); }
  uint64_t streams_closed()   const noexcept { return _total_streams_closed.load(std::memory_order_relaxed); }
  uint64_t pauses_injected()  const noexcept { return _total_pauses_injected.load(std::memory_order_relaxed); }
  uint64_t bytes_streamed()   const noexcept { return _total_bytes_streamed.load(std::memory_order_relaxed); }
  uint64_t cycles_damped()    const noexcept { return _total_throttle_cycles_damped.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETSTREAMTHROTTLE_HPP
