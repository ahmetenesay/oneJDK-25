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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETMULTIDIESTRIPING_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETMULTIDIESTRIPING_HPP

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
// Redjet Multi-Die Memory Striping Coordinator (PLAN.md Section 8.4, 8.8)
// - Coordinates the striped runway branch (/heap/on_heap/runway/striped)
// - Distributes 256KB page stripes across 12-channel DDR5 memory controllers
// - Maximizes sustained DRAM bus saturation (> 400 GB/s aggregate throughput)
// - Eliminates single-channel bottlenecks on high-core server architectures
// ============================================================================

enum class RedjetStripeGranularity : uint8_t {
  GRANULARITY_64KB  = 0,
  GRANULARITY_128KB = 1,
  GRANULARITY_256KB = 2,
  GRANULARITY_1MB   = 3
};

struct alignas(64) RedjetDramChannel {
  uint32_t            channel_id{ 0 };
  uint32_t            numa_node{ 0 };
  std::atomic<size_t> bytes_allocated{ 0 };
  std::atomic<size_t> active_stripes{ 0 };
  std::atomic<uint64_t> bytes_transferred{ 0 };
  double              measured_bandwidth_gbps{ 0.0 };
};

class RedjetMultiDieStriping {
public:
  static constexpr size_t DEFAULT_STRIPE_PAGE_SIZE = 262144; // 256 KB stripe unit
  static constexpr size_t MAX_CHANNELS             = 16;     // Up to 16 memory channels

private:
  static RedjetMultiDieStriping* _instance;
  static std::atomic<bool>       _initialized;

  RedjetDramChannel              _channels[MAX_CHANNELS];
  size_t                         _num_channels{ 12 }; // Default to 12 channels (EPYC/Xeon)
  std::atomic<uint32_t>          _next_channel_seq{ 0 };
  hs_ticket_lock_t               _striping_lock;
  RedjetStripeGranularity        _granularity{ RedjetStripeGranularity::GRANULARITY_256KB };

  // Telemetry & metrics
  std::atomic<uint64_t>          _total_stripes_allocated{ 0 };
  std::atomic<uint64_t>          _total_bytes_striped{ 0 };
  std::atomic<uint64_t>          _total_channel_rotations{ 0 };
  std::atomic<uint64_t>          _channel_imbalances_mitigated{ 0 };

public:
  RedjetMultiDieStriping();
  ~RedjetMultiDieStriping();

  static void initialize(size_t channels = 12);
  static void destroy();
  static RedjetMultiDieStriping* get() noexcept { return _instance; }

  // Configure number of active memory channels and NUMA topology
  void configure_topology(size_t active_channels, const uint32_t* numa_mapping = nullptr);

  // Allocate next stripe round-robin / least-loaded
  uint32_t select_next_channel() noexcept;

  // Record allocation on specific channel
  void record_stripe_allocation(uint32_t channel_id, size_t bytes) noexcept;

  // Record deallocation on specific channel
  void record_stripe_release(uint32_t channel_id, size_t bytes) noexcept;

  // Check channel balance across the multi-die bus
  bool is_channel_balanced() const noexcept;

  // Granularity configuration
  void set_granularity(RedjetStripeGranularity g) noexcept { _granularity = g; }
  RedjetStripeGranularity granularity() const noexcept { return _granularity; }
  size_t stripe_page_bytes() const noexcept;

  // Telemetry accessors
  size_t   num_channels()                  const noexcept { return _num_channels; }
  uint64_t total_stripes_allocated()       const noexcept { return _total_stripes_allocated.load(std::memory_order_relaxed); }
  uint64_t total_bytes_striped()           const noexcept { return _total_bytes_striped.load(std::memory_order_relaxed); }
  uint64_t total_channel_rotations()       const noexcept { return _total_channel_rotations.load(std::memory_order_relaxed); }
  uint64_t channel_imbalances_mitigated()  const noexcept { return _channel_imbalances_mitigated.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETMULTIDIESTRIPING_HPP
