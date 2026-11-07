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

#include "gc/redjet/runtime/redjetThermalGovernor.hpp"
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

RedjetThermalGovernor* RedjetThermalGovernor::_instance = nullptr;
std::atomic<bool> RedjetThermalGovernor::_initialized{ false };

RedjetThermalGovernor::RedjetThermalGovernor()
  : _current_temperature(45),
    _current_state(RedjetThermalState::COOL),
    _max_configured_workers(16),
    _active_worker_cap(16),
    _total_thermal_checks(0),
    _total_throttles_engaged(0),
    _total_prochot_events(0) {
  hs_ticket_lock_init(&_governor_lock);
}

RedjetThermalGovernor::~RedjetThermalGovernor() {
}

void RedjetThermalGovernor::initialize(uint32_t max_workers) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetThermalGovernor();
    _instance->_max_configured_workers = (max_workers > 0) ? max_workers : 16;
    _instance->_active_worker_cap.store(_instance->_max_configured_workers, std::memory_order_relaxed);
  }
}

void RedjetThermalGovernor::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

uint32_t RedjetThermalGovernor::read_hardware_temperature_celsius() {
  _total_thermal_checks.fetch_add(1, std::memory_order_relaxed);

  // Linux sysfs hardware monitor probe
#if defined(__linux__)
  FILE* fp = std::fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  if (fp != nullptr) {
    int temp_milli = 0;
    if (std::fscanf(fp, "%d", &temp_milli) == 1) {
      std::fclose(fp);
      return static_cast<uint32_t>(temp_milli / 1000);
    }
    std::fclose(fp);
  }
#endif

  // Fallback hardware thermal model: baseline 45°C + mild perturbation
  uint64_t tsc = hs_read_tsc();
  uint32_t delta = static_cast<uint32_t>((tsc >> 20) % 15); // 0-14°C fluctuation
  return 45 + delta;
}

RedjetThermalSnapshot RedjetThermalGovernor::sample_and_regulate() {
  hs_ticket_lock_acquire(&_governor_lock);

  uint32_t temp = read_hardware_temperature_celsius();
  _current_temperature.store(temp, std::memory_order_relaxed);

  RedjetThermalState state = RedjetThermalState::COOL;
  uint32_t workers = _max_configured_workers;

  if (temp >= TEMP_THRESHOLD_PROCHOT) {
    state = RedjetThermalState::PROCHOT;
    workers = 1; // Critical: throttle down to 1 worker to save mutator thermal headroom
    _total_prochot_events.fetch_add(1, std::memory_order_relaxed);
    _total_throttles_engaged.fetch_add(1, std::memory_order_relaxed);
  } else if (temp >= TEMP_THRESHOLD_HOT) {
    state = RedjetThermalState::HOT;
    workers = std::max(1U, _max_configured_workers / 2); // 50% concurrency
    _total_throttles_engaged.fetch_add(1, std::memory_order_relaxed);
  } else if (temp >= TEMP_THRESHOLD_WARM) {
    state = RedjetThermalState::WARM;
    workers = _max_configured_workers;
  } else {
    state = RedjetThermalState::COOL;
    workers = _max_configured_workers;
  }

  _current_state.store(state, std::memory_order_relaxed);
  _active_worker_cap.store(workers, std::memory_order_relaxed);

  RedjetThermalSnapshot snap;
  snap.package_temperature_celsius = temp;
  snap.estimated_power_watts = (state == RedjetThermalState::PROCHOT) ? 140 :
                               (state == RedjetThermalState::HOT)     ? 105 : 65;
  snap.thermal_state = state;
  snap.recommended_gc_workers = workers;
  snap.sample_tsc = hs_read_tsc();

  hs_ticket_lock_release(&_governor_lock);
  return snap;
}

void RedjetThermalGovernor::print_on(outputStream* st) const {
  if (st == nullptr) return;

  RedjetThermalState s = _current_state.load(std::memory_order_relaxed);

  st->print_cr("--- Redjet Thermal & Power Governor ---");
  st->print_cr("  Package Temperature:  %u C", _current_temperature.load(std::memory_order_relaxed));
  st->print_cr("  Thermal State:        %s",
               (s == RedjetThermalState::COOL)    ? "COOL (< 65C, 100% Concurrency)" :
               (s == RedjetThermalState::WARM)    ? "WARM (65-78C, Nominal Concurrency)" :
               (s == RedjetThermalState::HOT)     ? "HOT (78-88C, 50% Scaled Concurrency)" : "PROCHOT (> 88C, Emergency Throttling)");
  st->print_cr("  Active Worker Cap:    %u / %u", _active_worker_cap.load(std::memory_order_relaxed), _max_configured_workers);
  st->print_cr("  Thermal Checks:       %llu", static_cast<unsigned long long>(_total_thermal_checks.load(std::memory_order_relaxed)));
  st->print_cr("  Throttles Engaged:    %llu", static_cast<unsigned long long>(_total_throttles_engaged.load(std::memory_order_relaxed)));
  st->print_cr("  PROCHOT Trapped:      %llu", static_cast<unsigned long long>(_total_prochot_events.load(std::memory_order_relaxed)));
}
