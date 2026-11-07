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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETTHERMALGOVERNOR_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETTHERMALGOVERNOR_HPP

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
// Redjet Autonomous Thermal & Power Governor (PLAN.md Section 8.8, 8.9)
// - Real-time CPU Package Temperature & RAPL Power Monitoring
// - PROCHOT Throttling Mitigation: scales GC concurrency to protect mutators
// - Dynamic GC Worker Tiering (16 workers -> 4 workers when T > 85°C)
// - Seamless coupling with Upclocker Turbo Boost Engine
// ============================================================================

enum class RedjetThermalState : uint8_t {
  COOL     = 0, // < 65°C: Full concurrency (100% GC workers active)
  WARM     = 1, // 65-78°C: Nominal operation
  HOT      = 2, // 78-88°C: Scaled down (50% GC workers active)
  PROCHOT  = 3  // > 88°C: Emergency thermal throttling (GC workers reduced to 1 or paused)
};

struct RedjetThermalSnapshot {
  uint32_t           package_temperature_celsius{ 45 };
  uint32_t           estimated_power_watts{ 65 };
  RedjetThermalState thermal_state{ RedjetThermalState::COOL };
  uint32_t           recommended_gc_workers{ 8 };
  uint64_t           sample_tsc{ 0 };
};

class RedjetThermalGovernor {
public:
  static constexpr uint32_t TEMP_THRESHOLD_WARM    = 65;
  static constexpr uint32_t TEMP_THRESHOLD_HOT     = 78;
  static constexpr uint32_t TEMP_THRESHOLD_PROCHOT = 88;

private:
  static RedjetThermalGovernor* _instance;
  static std::atomic<bool>      _initialized;

  std::atomic<uint32_t>           _current_temperature{ 45 };
  std::atomic<RedjetThermalState> _current_state{ RedjetThermalState::COOL };
  uint32_t                        _max_configured_workers{ 16 };
  std::atomic<uint32_t>           _active_worker_cap{ 16 };

  hs_ticket_lock_t                _governor_lock;

  // Telemetry
  std::atomic<uint64_t>           _total_thermal_checks{ 0 };
  std::atomic<uint64_t>           _total_throttles_engaged{ 0 };
  std::atomic<uint64_t>           _total_prochot_events{ 0 };

  uint32_t read_hardware_temperature_celsius();

public:
  RedjetThermalGovernor();
  ~RedjetThermalGovernor();

  static void initialize(uint32_t max_workers = 16);
  static void destroy();
  static RedjetThermalGovernor* get() noexcept { return _instance; }

  // 1. Evaluate Thermal State & Adjust Worker Concurrency
  RJ_API RedjetThermalSnapshot sample_and_regulate();

  // 2. Query Current Active Worker Cap
  uint32_t active_worker_cap() const noexcept { return _active_worker_cap.load(std::memory_order_relaxed); }

  // 3. Status queries
  uint32_t           temperature() const noexcept { return _current_temperature.load(std::memory_order_relaxed); }
  RedjetThermalState state()       const noexcept { return _current_state.load(std::memory_order_relaxed); }

  // Telemetry
  uint64_t thermal_checks()    const noexcept { return _total_thermal_checks.load(std::memory_order_relaxed); }
  uint64_t throttles_engaged() const noexcept { return _total_throttles_engaged.load(std::memory_order_relaxed); }
  uint64_t prochot_events()    const noexcept { return _total_prochot_events.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETTHERMALGOVERNOR_HPP
