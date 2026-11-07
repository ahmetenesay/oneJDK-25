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

#include "gc/redjet/runtime/redjetPrecisionTimer.hpp"
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

RedjetPrecisionTimer* RedjetPrecisionTimer::_instance = nullptr;
std::atomic<bool>     RedjetPrecisionTimer::_initialized{ false };

RedjetPrecisionTimer::RedjetPrecisionTimer() {
  hs_ticket_lock_init(&_timer_lock);
  std::memset(&_calib, 0, sizeof(_calib));
  std::memset(_numa_phase_deltas, 0, sizeof(_numa_phase_deltas));

  calibrate_internal();
}

RedjetPrecisionTimer::~RedjetPrecisionTimer() {
  // Release resources
}

void RedjetPrecisionTimer::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetPrecisionTimer();
  }
}

void RedjetPrecisionTimer::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

bool RedjetPrecisionTimer::detect_invariant_tsc_cpuid() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  int cpuInfo[4] = { 0 };
#if defined(_MSC_VER)
  __cpuid(cpuInfo, 0x80000000);
  int max_ext = cpuInfo[0];
  if (static_cast<unsigned int>(max_ext) >= 0x80000007) {
    __cpuid(cpuInfo, 0x80000007);
    return (cpuInfo[3] & (1 << 8)) != 0; // Bit 8 in EDX = Invariant TSC
  }
#else
  unsigned int eax, ebx, ecx, edx;
  if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx)) {
    if (eax >= 0x80000007) {
      __cpuid(0x80000007, eax, ebx, ecx, edx);
      return (edx & (1 << 8)) != 0;
    }
  }
#endif
#endif
  return true; // Assume true on modern 64-bit systems
}

void RedjetPrecisionTimer::calculate_fixed_point_factors(uint64_t freq_hz) {
  if (freq_hz == 0) {
    freq_hz = 3000000000ULL; // Fallback 3.0 GHz
  }

  _calib.tsc_frequency_hz = freq_hz;
  _calib.measured_ghz = static_cast<double>(freq_hz) / 1.0e9;
  _calib.shift_cycles_to_ns = DEFAULT_SHIFT;
  _calib.shift_ns_to_cycles = DEFAULT_SHIFT;

  // mult_cycles_to_ns = (1,000,000,000ULL << shift) / freq_hz
  uint64_t one_sec_shifted = 1000000000ULL << DEFAULT_SHIFT;
  _calib.mult_cycles_to_ns = static_cast<uint32_t>(one_sec_shifted / freq_hz);

  // mult_ns_to_cycles = (freq_hz << shift) / 1,000,000,000ULL
  // Avoid 64-bit overflow by doing high-precision computation
  double mult_ns_dbl = (static_cast<double>(freq_hz) * static_cast<double>(1ULL << DEFAULT_SHIFT)) / 1.0e9;
  _calib.mult_ns_to_cycles = static_cast<uint32_t>(mult_ns_dbl);
}

void RedjetPrecisionTimer::calibrate_internal() {
  _total_calibrations.fetch_add(1, std::memory_order_relaxed);
  _calib.is_invariant = detect_invariant_tsc_cpuid();

  // Measure TSC frequency against OS high-resolution reference clock
  uint64_t freq_hz = 0;

#if defined(_WIN32)
  LARGE_INTEGER qpc_freq, qpc_start, qpc_end;
  if (QueryPerformanceFrequency(&qpc_freq) && QueryPerformanceCounter(&qpc_start)) {
    uint64_t tsc_start = sacculus_rdtsc();
    // Busy spin for ~5 milliseconds
    int64_t wait_ticks = (qpc_freq.QuadPart * 5) / 1000;
    do {
      QueryPerformanceCounter(&qpc_end);
    } while ((qpc_end.QuadPart - qpc_start.QuadPart) < wait_ticks);

    uint64_t tsc_end = sacculus_rdtsc();
    int64_t elapsed_qpc = qpc_end.QuadPart - qpc_start.QuadPart;
    if (elapsed_qpc > 0) {
      double elapsed_sec = static_cast<double>(elapsed_qpc) / static_cast<double>(qpc_freq.QuadPart);
      freq_hz = static_cast<uint64_t>(static_cast<double>(tsc_end - tsc_start) / elapsed_sec);
    }
  }
#else
  auto start_tp = std::chrono::steady_clock::now();
  uint64_t tsc_start = sacculus_rdtsc();

  // Spin for ~5 milliseconds
  auto target_tp = start_tp + std::chrono::milliseconds(5);
  while (std::chrono::steady_clock::now() < target_tp) {
    #if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
    #endif
  }

  auto end_tp = std::chrono::steady_clock::now();
  uint64_t tsc_end = sacculus_rdtsc();

  auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_tp - start_tp).count();
  if (elapsed_ns > 0) {
    freq_hz = static_cast<uint64_t>((static_cast<double>(tsc_end - tsc_start) * 1.0e9) / static_cast<double>(elapsed_ns));
  }
#endif

  if (freq_hz < 500000000ULL || freq_hz > 10000000000ULL) {
    // Sanity clamp: 3.0 GHz default if calibration had jitter
    freq_hz = 3000000000ULL;
  }

  calculate_fixed_point_factors(freq_hz);

  _calib.base_tsc_epoch = sacculus_rdtsc();
  auto now_sys = std::chrono::system_clock::now().time_since_epoch();
  _calib.base_epoch_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now_sys).count();
}

uint64_t RedjetPrecisionTimer::now_nanos() const noexcept {
  uint64_t cur_cycles = sacculus_rdtsc();
  uint64_t delta_cycles = (cur_cycles >= _calib.base_tsc_epoch) ? (cur_cycles - _calib.base_tsc_epoch) : 0;
  return _calib.base_epoch_nanos + cycles_to_nanos(delta_cycles);
}

void RedjetPrecisionTimer::busy_wait_nanos(uint64_t nanos) noexcept {
  if (nanos == 0) return;
  RedjetPrecisionTimer* timer = _instance;
  uint64_t target_cycles = 0;
  if (timer != nullptr) {
    target_cycles = timer->nanos_to_cycles(nanos);
  } else {
    target_cycles = nanos * 3; // Fallback ~3 cycles per ns
  }

  uint64_t start_cycles = sacculus_rdtsc();
  while ((sacculus_rdtsc() - start_cycles) < target_cycles) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
  }
}

void RedjetPrecisionTimer::busy_wait_micros(uint32_t micros) noexcept {
  busy_wait_nanos(static_cast<uint64_t>(micros) * 1000ULL);
}

void RedjetPrecisionTimer::register_numa_node_sync(uint32_t numa_id, int64_t phase_delta_cycles) {
  if (numa_id >= MAX_NUMA_NODES) return;

  hs_ticket_lock_acquire(&_timer_lock);
  _numa_phase_deltas[numa_id] = phase_delta_cycles;
  if (numa_id + 1 > _detected_nodes) {
    _detected_nodes = numa_id + 1;
  }
  _total_phase_checks.fetch_add(1, std::memory_order_relaxed);

  // If phase delta exceeds ~5000 cycles (>1.5 microseconds), flag drift warning
  if (std::abs(phase_delta_cycles) > 5000) {
    _total_drift_warnings.fetch_add(1, std::memory_order_relaxed);
  }
  hs_ticket_lock_release(&_timer_lock);
}

int64_t RedjetPrecisionTimer::get_numa_phase_delta(uint32_t numa_id) const noexcept {
  if (numa_id >= MAX_NUMA_NODES) return 0;
  return _numa_phase_deltas[numa_id];
}

void RedjetPrecisionTimer::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetPrecisionTimer] Invariant TSC Calibration Status:");
  st->print_cr("  - Frequency:            %llu Hz (%.3f GHz)",
               static_cast<unsigned long long>(_calib.tsc_frequency_hz),
               _calib.measured_ghz);
  st->print_cr("  - Invariant Hardware:   %s", _calib.is_invariant ? "YES (Hardware Constant Rate)" : "NO (Emulated Fixed)");
  st->print_cr("  - Fixed-Point Scale:    Mult = %u, Shift = %u",
               _calib.mult_cycles_to_ns, _calib.shift_cycles_to_ns);
  st->print_cr("  - Calibrations Done:    %llu",
               static_cast<unsigned long long>(_total_calibrations.load(std::memory_order_relaxed)));
  st->print_cr("  - NUMA Phase Checks:    %llu",
               static_cast<unsigned long long>(_total_phase_checks.load(std::memory_order_relaxed)));
  st->print_cr("  - Phase Drift Warnings: %llu",
               static_cast<unsigned long long>(_total_drift_warnings.load(std::memory_order_relaxed)));
  st->print_cr("  - NUMA Node Phase Alignment:");
  for (size_t i = 0; i < _detected_nodes; ++i) {
    st->print_cr("      NUMA Node %zu: Offset = %lld cycles (%.1f ns)",
                 i,
                 static_cast<long long>(_numa_phase_deltas[i]),
                 static_cast<double>(_numa_phase_deltas[i]) / _calib.measured_ghz);
  }
}
