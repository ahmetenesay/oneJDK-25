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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETMEMSCRUBBER_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETMEMSCRUBBER_HPP

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
// Redjet Autonomous DRAM Memory Scrubber (PLAN.md Section 7.3, 9.1)
// - Background zeroing using 512-bit non-temporal streaming stores (_mm512_stream_si512)
// - 4x unrolled loop (256 bytes per iteration)
// - Eliminates cache pollution during page sanitization in landfill trash pit
// - Pre-sanitizes runway memory so mutators never pay cold-zeroing allocation latency
// - Zero-page verification pass avoiding redundant memory writes
// - Memory bandwidth throttle preventing mutator bus starvation
// ============================================================================

class RedjetMemScrubber {
public:
  static constexpr size_t CACHE_LINE_SIZE     = 64;
  static constexpr size_t UNROLL_STRIDE_BYTES = 256;
  static constexpr size_t BLOCK_64K           = 64 * 1024;
  static constexpr size_t PAGE_2M             = 2 * 1024 * 1024;

private:
  static RedjetMemScrubber* _instance;
  static std::atomic<bool>  _initialized;

  hs_ticket_lock_t          _scrubber_lock;
  std::atomic<uint64_t>     _target_bandwidth_mbps{ 500 }; // 500 MB/s throttle baseline

  // Telemetry & metrics
  std::atomic<uint64_t>     _total_bytes_scrubbed{ 0 };
  std::atomic<uint64_t>     _total_blocks_sanitized{ 0 };
  std::atomic<uint64_t>     _total_hugepages_sanitized{ 0 };
  std::atomic<uint64_t>     _total_streaming_stores{ 0 };
  std::atomic<uint64_t>     _redundant_zero_writes_skipped{ 0 };
  std::atomic<uint64_t>     _scrubber_throttles_engaged{ 0 };

public:
  RedjetMemScrubber();
  ~RedjetMemScrubber();

  static void initialize();
  static void destroy();
  static RedjetMemScrubber* get() noexcept { return _instance; }

  // 1. Non-Temporal Streaming Zeroing (Cache Bypass with 4x 512-bit unrolling)
  RJ_API void zero_range_streaming(void* dest, size_t bytes) noexcept;

  // 2. Pre-sanitize 64KB Runway Block
  RJ_API void sanitize_64k_block(void* block_addr) noexcept;

  // 3. Pre-sanitize 2MB Huge Page
  RJ_API void sanitize_2m_page(void* page_addr) noexcept;

  // 4. Zero-page verification check (returns true if range is already all zero)
  RJ_API bool is_range_all_zero(const void* src, size_t bytes) noexcept;

  // 5. Bandwidth pacing & throttling configuration
  void set_target_bandwidth_mbps(uint64_t mbps) noexcept {
    _target_bandwidth_mbps.store(mbps > 0 ? mbps : 100, std::memory_order_relaxed);
  }
  uint64_t target_bandwidth_mbps() const noexcept {
    return _target_bandwidth_mbps.load(std::memory_order_relaxed);
  }

  // Telemetry accessors
  uint64_t total_bytes_scrubbed()           const noexcept { return _total_bytes_scrubbed.load(std::memory_order_relaxed); }
  uint64_t total_blocks_sanitized()         const noexcept { return _total_blocks_sanitized.load(std::memory_order_relaxed); }
  uint64_t total_hugepages_sanitized()      const noexcept { return _total_hugepages_sanitized.load(std::memory_order_relaxed); }
  uint64_t total_streaming_stores()         const noexcept { return _total_streaming_stores.load(std::memory_order_relaxed); }
  uint64_t redundant_zero_writes_skipped()  const noexcept { return _redundant_zero_writes_skipped.load(std::memory_order_relaxed); }
  uint64_t scrubber_throttles_engaged()     const noexcept { return _scrubber_throttles_engaged.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETMEMSCRUBBER_HPP
