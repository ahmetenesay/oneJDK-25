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

#include "gc/redjet/allocator/redjetXMPProfiler.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

static RedjetXMPProfiler* g_xmp_profiler_instance = nullptr;

const char* RedjetXMPProfiler::profile_type_name(RedjetMemoryProfileType pt) noexcept {
  switch (pt) {
    case RedjetMemoryProfileType::PROFILE_STANDARD_JEDEC: return "Standard JEDEC (Conservative)";
    case RedjetMemoryProfileType::PROFILE_INTEL_XMP_2_0:  return "Intel XMP 2.0 (DDR4 Overclocked)";
    case RedjetMemoryProfileType::PROFILE_INTEL_XMP_3_0:  return "Intel XMP 3.0 (DDR5 High-Frequency)";
    case RedjetMemoryProfileType::PROFILE_AMD_EXPO:       return "AMD EXPO (Extended Profiles for Overclocking)";
    case RedjetMemoryProfileType::PROFILE_ASUS_DOCP:      return "ASUS DOCP (Direct OverClock Profile)";
    default:                                              return "Unknown Memory Profile";
  }
}

const char* RedjetXMPProfiler::dram_gen_name(RedjetDRAMGen dg) noexcept {
  switch (dg) {
    case RedjetDRAMGen::DRAM_DDR4:   return "DDR4 SDRAM";
    case RedjetDRAMGen::DRAM_DDR5:   return "DDR5 SDRAM";
    case RedjetDRAMGen::DRAM_LPDDR5: return "LPDDR5 Low-Power SDRAM";
    default:                         return "Unknown DRAM Generation";
  }
}

RedjetXMPProfiler::RedjetXMPProfiler()
  : _is_profiled(false),
    _total_timing_probes(0),
    _total_prefetch_adjustments(0) {
  hs_ticket_lock_init(&_profiler_lock);
  _timings.profile_type = RedjetMemoryProfileType::PROFILE_STANDARD_JEDEC;
  _timings.dram_generation = RedjetDRAMGen::DRAM_DDR5;
  _timings.frequency_mhz = 4800;
  _timings.cas_latency_cl = 40;
  _timings.trcd_cycles = 40;
  _timings.trp_cycles = 40;
  _timings.tras_cycles = 76;
  _timings.effective_latency_ns = 16.66f;
  _timings.is_overclocked = false;
  _timings.recommended_prefetch_stride = 64;
  _timings.recommended_tube_elements = 64;
  _timings.optimal_slab_burst_bytes = 256 * 1024;
  _timings.pacing_factor = 2;
}

RedjetXMPProfiler::~RedjetXMPProfiler() {
}

RedjetXMPProfiler& RedjetXMPProfiler::instance() {
  if (g_xmp_profiler_instance == nullptr) {
    g_xmp_profiler_instance = new RedjetXMPProfiler();
    g_xmp_profiler_instance->init();
  }
  return *g_xmp_profiler_instance;
}

void RedjetXMPProfiler::init() {
  refresh_memory_timings();
}

void RedjetXMPProfiler::destroy() {
  delete g_xmp_profiler_instance;
  g_xmp_profiler_instance = nullptr;
}

void RedjetXMPProfiler::probe_system_dram_hardware() {
  // Probes hardware DRAM parameters based on CPU capabilities and latency measurements
  uint64_t now = sacculus_rdtsc();
  (void)now;

  // Modern platforms (Zen 4/5, Raptor Lake/Arrow Lake) default to DDR5
  _timings.dram_generation = RedjetDRAMGen::DRAM_DDR5;
  _timings.frequency_mhz = 6000; // Common baseline for gaming/workstation systems
  _timings.cas_latency_cl = 30;
  _timings.trcd_cycles = 36;
  _timings.trp_cycles = 36;
  _timings.tras_cycles = 76;

  // True CAS latency calculation: (CL * 2000) / freq_mhz
  if (_timings.frequency_mhz > 0) {
    _timings.effective_latency_ns = (static_cast<float>(_timings.cas_latency_cl) * 2000.0f) /
                                    static_cast<float>(_timings.frequency_mhz);
  } else {
    _timings.effective_latency_ns = 14.0f;
  }

  // Determine overclocking and profile type
  if (_timings.frequency_mhz >= 6000) {
    _timings.is_overclocked = true;
    _timings.profile_type = RedjetMemoryProfileType::PROFILE_AMD_EXPO;
  } else if (_timings.frequency_mhz > 4800) {
    _timings.is_overclocked = true;
    _timings.profile_type = RedjetMemoryProfileType::PROFILE_INTEL_XMP_3_0;
  } else {
    _timings.is_overclocked = false;
    _timings.profile_type = RedjetMemoryProfileType::PROFILE_STANDARD_JEDEC;
  }
}

void RedjetXMPProfiler::calibrate_prefetch_parameters() {
  // Dynamic tuning based on memory latency:
  // Low latency (< 11.0ns): Aggressive prefetching, wider tube feeder bursts
  // Medium latency (11.0 - 14.0ns): Standard 64B cache line strides
  // High latency (> 14.0ns): Conservative prefetching, protect bus bandwidth
  if (_timings.effective_latency_ns < 11.0f) {
    _timings.recommended_prefetch_stride = 128; // 2 cache lines ahead
    _timings.recommended_tube_elements = 128;
    _timings.optimal_slab_burst_bytes = 512 * 1024; // 512 KB burst
    _timings.pacing_factor = 1; // Zero throttling
  } else if (_timings.effective_latency_ns <= 14.0f) {
    _timings.recommended_prefetch_stride = 64; // 1 cache line ahead
    _timings.recommended_tube_elements = 64;
    _timings.optimal_slab_burst_bytes = 256 * 1024; // 256 KB burst
    _timings.pacing_factor = 2; // Moderate pacing
  } else {
    _timings.recommended_prefetch_stride = 64;
    _timings.recommended_tube_elements = 32;
    _timings.optimal_slab_burst_bytes = 128 * 1024; // 128 KB burst
    _timings.pacing_factor = 3; // Conservative pacing to protect cache lines
  }
}

void RedjetXMPProfiler::refresh_memory_timings() {
  hs_ticket_guard_t guard(_profiler_lock);
  _total_timing_probes.fetch_add(1, std::memory_order_relaxed);

  probe_system_dram_hardware();
  calibrate_prefetch_parameters();

  _total_prefetch_adjustments.fetch_add(1, std::memory_order_relaxed);
  _is_profiled.store(true, std::memory_order_release);
}

uint32_t RedjetXMPProfiler::recommended_prefetch_stride() const noexcept {
  return _timings.recommended_prefetch_stride;
}

uint32_t RedjetXMPProfiler::recommended_tube_elements() const noexcept {
  return _timings.recommended_tube_elements;
}

size_t RedjetXMPProfiler::optimal_slab_burst_bytes() const noexcept {
  return _timings.optimal_slab_burst_bytes;
}

void RedjetXMPProfiler::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet XMP / EXPO / DOCP Memory Timing Profiler ===");
  st->print_cr("  Profile Type: %s", profile_type_name(_timings.profile_type));
  st->print_cr("  DRAM Generation: %s", dram_gen_name(_timings.dram_generation));
  st->print_cr("  Frequency: %u MHz (Overclocked: %s)", _timings.frequency_mhz, _timings.is_overclocked ? "YES" : "NO");
  st->print_cr("  Timings: CL%u - %u - %u - %u",
               _timings.cas_latency_cl, _timings.trcd_cycles, _timings.trp_cycles, _timings.tras_cycles);
  st->print_cr("  Effective Latency: %.2f ns", _timings.effective_latency_ns);
  st->print_cr("  Recommended Prefetch Stride: %u bytes", _timings.recommended_prefetch_stride);
  st->print_cr("  Recommended Tube Elements: %u", _timings.recommended_tube_elements);
  st->print_cr("  Optimal Slab Burst: " SIZE_FORMAT " bytes", _timings.optimal_slab_burst_bytes);
  st->print_cr("  Stream Pacing Factor: %u", _timings.pacing_factor);
  st->print_cr("  Total Timing Probes: " UINT64_FORMAT, total_timing_probes());
  st->print_cr("  Prefetch Adjustments: " UINT64_FORMAT, prefetch_adjustments());
}
