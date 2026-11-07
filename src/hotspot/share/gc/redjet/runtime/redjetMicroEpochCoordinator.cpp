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

#include "gc/redjet/runtime/redjetMicroEpochCoordinator.hpp"
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

RedjetMicroEpochCoordinator* RedjetMicroEpochCoordinator::_instance = nullptr;
std::atomic<bool> RedjetMicroEpochCoordinator::_initialized{ false };

RedjetMicroEpochCoordinator::RedjetMicroEpochCoordinator()
  : _current_epoch_id(0),
    _current_phase(RedjetEpochPhase::EPOCH_IDLE),
    _budget_micros(DEFAULT_BUDGET_MICROS),
    _budget_tsc_cycles(150000),
    _total_epochs_completed(0),
    _total_budget_overruns(0),
    _total_objects_scanned(0),
    _total_bytes_swept(0) {
  hs_ticket_lock_init(&_coordinator_lock);
}

RedjetMicroEpochCoordinator::~RedjetMicroEpochCoordinator() {
}

void RedjetMicroEpochCoordinator::initialize(uint32_t budget_micros) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetMicroEpochCoordinator();
    _instance->_budget_micros = (budget_micros > 0) ? budget_micros : DEFAULT_BUDGET_MICROS;
    // Assuming 3.0 GHz baseline: 1 microsecond = ~3000 cycles
    _instance->_budget_tsc_cycles = static_cast<uint64_t>(_instance->_budget_micros) * 3000ULL;
  }
}

void RedjetMicroEpochCoordinator::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

uint64_t RedjetMicroEpochCoordinator::start_micro_epoch() {
  uint64_t epoch = _current_epoch_id.fetch_add(1, std::memory_order_relaxed) + 1;
  _current_phase.store(RedjetEpochPhase::EPOCH_DISPATCH, std::memory_order_relaxed);
  return hs_read_tsc();
}

bool RedjetMicroEpochCoordinator::advance_phase(RedjetEpochPhase expected, RedjetEpochPhase next_phase) {
  return _current_phase.compare_exchange_strong(expected, next_phase, std::memory_order_acq_rel);
}

bool RedjetMicroEpochCoordinator::is_budget_exceeded(uint64_t start_tsc) const noexcept {
  uint64_t now = hs_read_tsc();
  uint64_t elapsed = (now > start_tsc) ? (now - start_tsc) : 0;
  return elapsed >= _budget_tsc_cycles;
}

RedjetEpochStats RedjetMicroEpochCoordinator::finish_micro_epoch(uint64_t start_tsc, size_t marked, size_t reclaimed) {
  uint64_t now = hs_read_tsc();
  uint64_t elapsed_cycles = (now > start_tsc) ? (now - start_tsc) : 1;

  RedjetEpochStats stats;
  stats.epoch_id = _current_epoch_id.load(std::memory_order_relaxed);
  stats.duration_tsc = elapsed_cycles;
  stats.duration_ns = static_cast<uint32_t>(elapsed_cycles / 3); // Normalized to ns
  stats.objects_marked = marked;
  stats.bytes_reclaimed = reclaimed;
  stats.budget_exceeded = (elapsed_cycles >= _budget_tsc_cycles);

  _total_epochs_completed.fetch_add(1, std::memory_order_relaxed);
  _total_objects_scanned.fetch_add(marked, std::memory_order_relaxed);
  _total_bytes_swept.fetch_add(reclaimed, std::memory_order_relaxed);

  if (stats.budget_exceeded) {
    _total_budget_overruns.fetch_add(1, std::memory_order_relaxed);
  }

  _current_phase.store(RedjetEpochPhase::EPOCH_IDLE, std::memory_order_release);
  return stats;
}

void RedjetMicroEpochCoordinator::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Micro-Epoch Coordinator ---");
  st->print_cr("  Current Epoch:        %llu", static_cast<unsigned long long>(_current_epoch_id.load(std::memory_order_relaxed)));
  st->print_cr("  Epoch Time Budget:    %u microseconds", _budget_micros);
  st->print_cr("  Epochs Completed:     %llu", static_cast<unsigned long long>(_total_epochs_completed.load(std::memory_order_relaxed)));
  st->print_cr("  Budget Overruns:      %llu", static_cast<unsigned long long>(_total_budget_overruns.load(std::memory_order_relaxed)));
  st->print_cr("  Total Objects Scanned:%llu", static_cast<unsigned long long>(_total_objects_scanned.load(std::memory_order_relaxed)));
  st->print_cr("  Total Bytes Swept:    %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_bytes_swept.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_bytes_swept.load(std::memory_order_relaxed) / (1024 * 1024)));
}
