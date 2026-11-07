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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETXMPPROFILER_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETXMPPROFILER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "utilities/globalDefinitions.hpp"
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
// XMP / EXPO / DOCP Memory Timing Profiler (PLAN.md Section 8.4)
// - Probes DRAM frequency, CAS Latency (CL), tRCD, tRP, tRAS
// - Distinguishes between Standard JEDEC vs Overclocked XMP / EXPO profiles
// - Dynamically adjusts Slab prefetch distances and Hardware Tube Feeder strides
// ============================================================================

enum class RedjetMemoryProfileType : uint8_t {
  PROFILE_STANDARD_JEDEC = 0,
  PROFILE_INTEL_XMP_2_0  = 1,
  PROFILE_INTEL_XMP_3_0  = 2,
  PROFILE_AMD_EXPO       = 3,
  PROFILE_ASUS_DOCP      = 4
};

enum class RedjetDRAMGen : uint8_t {
  DRAM_UNKNOWN = 0,
  DRAM_DDR4    = 4,
  DRAM_DDR5    = 5,
  DRAM_LPDDR5  = 6
};

struct alignas(64) RedjetMemoryTimings {
  RedjetMemoryProfileType profile_type{ RedjetMemoryProfileType::PROFILE_STANDARD_JEDEC };
  RedjetDRAMGen           dram_generation{ RedjetDRAMGen::DRAM_DDR5 };
  uint32_t                frequency_mhz{ 4800 };   // e.g. 6000 MHz
  uint32_t                cas_latency_cl{ 40 };    // e.g. 30 CL
  uint32_t                trcd_cycles{ 40 };
  uint32_t                trp_cycles{ 40 };
  uint32_t                tras_cycles{ 76 };
  float                   effective_latency_ns{ 13.33f }; // (CL * 2000) / freq_mhz
  bool                    is_overclocked{ false };
  uint32_t                recommended_prefetch_stride{ 64 };
  uint32_t                recommended_tube_elements{ 64 };
  size_t                  optimal_slab_burst_bytes{ 256 * 1024 }; // 256 KB
  uint32_t                pacing_factor{ 1 };
};

class RedjetXMPProfiler : public CHeapObj<mtGC> {
private:
  RedjetMemoryTimings   _timings;
  hs_ticket_lock_t      _profiler_lock;
  std::atomic<bool>     _is_profiled{ false };

  // Telemetry counters
  std::atomic<uint64_t> _total_timing_probes{ 0 };
  std::atomic<uint64_t> _total_prefetch_adjustments{ 0 };

  void probe_system_dram_hardware();
  void calibrate_prefetch_parameters();

public:
  RedjetXMPProfiler();
  ~RedjetXMPProfiler();

  static RedjetXMPProfiler& instance();

  void init();
  void destroy();

  // =========================================================================
  // 1. Hardware DRAM Probing & Timing Calculation
  // =========================================================================
  RJ_API void refresh_memory_timings();

  // Getters
  RedjetMemoryProfileType profile_type() const noexcept { return _timings.profile_type; }
  RedjetDRAMGen           dram_gen()     const noexcept { return _timings.dram_generation; }
  uint32_t                frequency_mhz() const noexcept { return _timings.frequency_mhz; }
  uint32_t                cas_latency()   const noexcept { return _timings.cas_latency_cl; }
  float                   latency_ns()    const noexcept { return _timings.effective_latency_ns; }
  bool                    is_overclocked() const noexcept { return _timings.is_overclocked; }
  uint32_t                pacing_factor() const noexcept { return _timings.pacing_factor; }

  // =========================================================================
  // 2. Dynamic GC & Prefetch Calibration Accessors
  // =========================================================================
  RJ_API uint32_t recommended_prefetch_stride() const noexcept;
  RJ_API uint32_t recommended_tube_elements() const noexcept;
  RJ_API size_t   optimal_slab_burst_bytes() const noexcept;

  // Profile names
  static const char* profile_type_name(RedjetMemoryProfileType pt) noexcept;
  static const char* dram_gen_name(RedjetDRAMGen dg) noexcept;

  // Telemetry
  uint64_t total_timing_probes() const noexcept { return _total_timing_probes.load(std::memory_order_relaxed); }
  uint64_t prefetch_adjustments() const noexcept { return _total_prefetch_adjustments.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETXMPPROFILER_HPP
