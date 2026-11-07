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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETPRECISIONTIMER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETPRECISIONTIMER_HPP

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
// Redjet Invariant TSC High-Precision Timer & Calibrator (PLAN.md Section 2.1, 5.1)
// - Sub-nanosecond resolution without expensive kernel syscalls (clock_gettime/QPC)
// - Invariant TSC CPUID detection & hardware invariant validation
// - Fast fixed-point math: (cycles * multiplier) >> shift for 0.2 ns cycle-to-ns
// - Multi-socket / Multi-CCD TSC phase delta alignment & drift monitoring
// - Sub-microsecond pause spinning loops with CPU hardware backoff (_mm_pause)
// ============================================================================

struct RedjetTimerCalibration {
  uint64_t tsc_frequency_hz{ 0 };
  uint64_t base_tsc_epoch{ 0 };
  uint64_t base_epoch_nanos{ 0 };
  uint32_t mult_cycles_to_ns{ 0 };
  uint32_t shift_cycles_to_ns{ 0 };
  uint32_t mult_ns_to_cycles{ 0 };
  uint32_t shift_ns_to_cycles{ 0 };
  bool     is_invariant{ false };
  double   measured_ghz{ 0.0 };
};

class RedjetPrecisionTimer {
public:
  static constexpr size_t MAX_NUMA_NODES = 16;
  static constexpr uint32_t DEFAULT_SHIFT = 32;

private:
  static RedjetPrecisionTimer* _instance;
  static std::atomic<bool>     _initialized;

  RedjetTimerCalibration       _calib;
  hs_ticket_lock_t             _timer_lock;

  // Multi-node sync tracking
  int64_t                      _numa_phase_deltas[MAX_NUMA_NODES];
  size_t                       _detected_nodes{ 1 };

  // Telemetry counters
  std::atomic<uint64_t>        _total_calibrations{ 0 };
  std::atomic<uint64_t>        _total_phase_checks{ 0 };
  std::atomic<uint64_t>        _total_drift_warnings{ 0 };

  void calibrate_internal();
  void calculate_fixed_point_factors(uint64_t freq_hz);
  static bool detect_invariant_tsc_cpuid() noexcept;

public:
  RedjetPrecisionTimer();
  ~RedjetPrecisionTimer();

  static void initialize();
  static void destroy();
  static RedjetPrecisionTimer* get() noexcept { return _instance; }

  // 1. Current TSC Cycle Counter (zero latency)
  RJ_API static inline uint64_t now_cycles() noexcept {
    return sacculus_rdtsc();
  }

  // 2. Conversion: Cycles to Nanoseconds (Fixed-point multiply and shift)
  RJ_API inline uint64_t cycles_to_nanos(uint64_t cycles) const noexcept {
    // (__int128) cycles * _calib.mult_cycles_to_ns >> _calib.shift_cycles_to_ns
#if defined(__SIZEOF_INT128__)
    unsigned __int128 prod = static_cast<unsigned __int128>(cycles) * _calib.mult_cycles_to_ns;
    return static_cast<uint64_t>(prod >> _calib.shift_cycles_to_ns);
#else
    uint64_t hi = (cycles >> 32) * _calib.mult_cycles_to_ns;
    uint64_t lo = (cycles & 0xFFFFFFFFULL) * _calib.mult_cycles_to_ns;
    return (hi << (32 - _calib.shift_cycles_to_ns)) + (lo >> _calib.shift_cycles_to_ns);
#endif
  }

  // 3. Conversion: Nanoseconds to Cycles
  RJ_API inline uint64_t nanos_to_cycles(uint64_t nanos) const noexcept {
#if defined(__SIZEOF_INT128__)
    unsigned __int128 prod = static_cast<unsigned __int128>(nanos) * _calib.mult_ns_to_cycles;
    return static_cast<uint64_t>(prod >> _calib.shift_ns_to_cycles);
#else
    uint64_t hi = (nanos >> 32) * _calib.mult_ns_to_cycles;
    uint64_t lo = (nanos & 0xFFFFFFFFULL) * _calib.mult_ns_to_cycles;
    return (hi << (32 - _calib.shift_ns_to_cycles)) + (lo >> _calib.shift_ns_to_cycles);
#endif
  }

  // 4. Current Time in Monotonic Nanoseconds
  RJ_API uint64_t now_nanos() const noexcept;

  // 5. Invariant & Calibration Status
  bool   is_invariant() const noexcept { return _calib.is_invariant; }
  double cpu_frequency_ghz() const noexcept { return _calib.measured_ghz; }
  uint64_t tsc_frequency_hz() const noexcept { return _calib.tsc_frequency_hz; }

  // 6. Sub-Microsecond Busy Waits
  RJ_API static void busy_wait_nanos(uint64_t nanos) noexcept;
  RJ_API static void busy_wait_micros(uint32_t micros) noexcept;

  // 7. Multi-socket / Multi-CCD Phase Synchronization
  RJ_API void register_numa_node_sync(uint32_t numa_id, int64_t phase_delta_cycles);
  RJ_API int64_t get_numa_phase_delta(uint32_t numa_id) const noexcept;

  // Telemetry
  uint64_t calibrations_performed() const noexcept { return _total_calibrations.load(std::memory_order_relaxed); }
  uint64_t phase_checks()           const noexcept { return _total_phase_checks.load(std::memory_order_relaxed); }
  uint64_t drift_warnings()         const noexcept { return _total_drift_warnings.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETPRECISIONTIMER_HPP
