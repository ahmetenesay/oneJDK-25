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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETTUBEFEEDER_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETTUBEFEEDER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"

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
// Hardware Tube Feeder: Streaming Vector Prefetch Conduit (PLAN.md Section 8.8)
// - Hardware prefetch ahead by 64 elements via _mm_prefetch(..., _MM_HINT_T0)
// - MLTC bus throttling (stream_paused) when bus contention is detected
// - Multi-channel streaming tube support
// - RAII RedjetTubeScope for automatic prefetch conduit lifecycle
// ============================================================================

static constexpr size_t REDJET_TUBE_PREFETCH_DISTANCE = 64; // Ahead by 64 elements
static constexpr size_t REDJET_MAX_TUBE_CHANNELS = 8;

struct alignas(64) RedjetTubeChannel {
  const uint8_t*        stream_base{ nullptr };
  size_t                total_elements{ 0 };
  size_t                element_stride{ 0 };
  std::atomic<size_t>   consumer_index{ 0 };
  std::atomic<size_t>   prefetch_index{ 0 };
  std::atomic<bool>     stream_paused{ false };
  std::atomic<bool>     is_active{ false };
  uint32_t              channel_id{ 0 };

  void reset() noexcept {
    stream_base = nullptr;
    total_elements = 0;
    element_stride = 0;
    consumer_index.store(0, std::memory_order_relaxed);
    prefetch_index.store(0, std::memory_order_relaxed);
    stream_paused.store(false, std::memory_order_relaxed);
    is_active.store(false, std::memory_order_relaxed);
    channel_id = 0;
  }
};

class RedjetTubeFeeder : public CHeapObj<mtGC> {
private:
  RedjetTubeChannel    _channels[REDJET_MAX_TUBE_CHANNELS];
  hs_ticket_lock_t     _feeder_lock;

  // MLTC coupling
  std::atomic<bool>    _global_stream_throttle{ false };

  // Telemetry counters
  std::atomic<uint64_t> _total_elements_fed{ 0 };
  std::atomic<uint64_t> _total_prefetches_issued{ 0 };
  std::atomic<uint64_t> _total_throttle_pauses{ 0 };
  std::atomic<uint64_t> _total_channels_opened{ 0 };

public:
  RedjetTubeFeeder();
  ~RedjetTubeFeeder();

  void init();
  void destroy();

  // =========================================================================
  // 1. Channel Lifecycle
  // =========================================================================
  RJ_API uint32_t open_channel(const void* base_ptr, size_t element_count, size_t element_stride);
  RJ_API void     close_channel(uint32_t channel_id);

  // =========================================================================
  // 2. Hardware Prefetch Stepping (Ahead by 64 Elements)
  // Issues _mm_prefetch(..., _MM_HINT_T0) ahead into L1-D cache
  // =========================================================================
  RJ_API void advance_prefetch(uint32_t channel_id, size_t consumer_pos);

  // Consumes N elements and advances the tube conduit
  RJ_API const void* consume_elements(uint32_t channel_id, size_t count);

  // =========================================================================
  // 3. MLTC Bus Throttling & Congestion Coupling
  // Pauses prefetch stream when bus saturation is detected by MLTC
  // =========================================================================
  RJ_API void update_mltc_congestion_state(RedjetBusCongestionLevel congestion_level);
  RJ_API bool is_channel_paused(uint32_t channel_id) const noexcept;

  // Telemetry
  uint64_t total_elements_fed() const noexcept { return _total_elements_fed.load(std::memory_order_relaxed); }
  uint64_t total_prefetches_issued() const noexcept { return _total_prefetches_issued.load(std::memory_order_relaxed); }
  uint64_t total_throttle_pauses() const noexcept { return _total_throttle_pauses.load(std::memory_order_relaxed); }
  uint64_t total_channels_opened() const noexcept { return _total_channels_opened.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

// ============================================================================
// RAII RedjetTubeScope: Manages lifecycle and autonomous prefetch stepping
// ============================================================================
class RedjetTubeScope {
private:
  RedjetTubeFeeder* _feeder;
  uint32_t          _channel_id{ UINT32_MAX };
  size_t            _current_pos{ 0 };
  size_t            _total_count{ 0 };

public:
  RedjetTubeScope(RedjetTubeFeeder* feeder, const void* base_ptr, size_t count, size_t stride)
    : _feeder(feeder), _total_count(count), _current_pos(0) {
    if (_feeder != nullptr) {
      _channel_id = _feeder->open_channel(base_ptr, count, stride);
      // Pre-warm the tube by prefetching the first 64 elements
      _feeder->advance_prefetch(_channel_id, 0);
    }
  }

  ~RedjetTubeScope() {
    if (_feeder != nullptr && _channel_id != UINT32_MAX) {
      _feeder->close_channel(_channel_id);
    }
  }

  inline bool has_next() const noexcept {
    return _current_pos < _total_count;
  }

  inline void step(size_t chunk_size = 1) noexcept {
    _current_pos += chunk_size;
    if (_feeder != nullptr && _channel_id != UINT32_MAX) {
      _feeder->advance_prefetch(_channel_id, _current_pos);
    }
  }

  inline uint32_t channel_id() const noexcept { return _channel_id; }
  inline size_t   current_position() const noexcept { return _current_pos; }
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETTUBEFEEDER_HPP
