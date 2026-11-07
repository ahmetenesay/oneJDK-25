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

#include "gc/redjet/runtime/redjetHandshakeGovernor.hpp"
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

RedjetHandshakeGovernor* RedjetHandshakeGovernor::_instance = nullptr;
std::atomic<bool> RedjetHandshakeGovernor::_initialized{ false };

RedjetHandshakeGovernor::RedjetHandshakeGovernor()
  : _total_handshakes_requested(0),
    _total_handshakes_completed(0),
    _total_handshake_latency_cycles(0) {
  hs_ticket_lock_init(&_handshake_lock);
  std::memset(_slots, 0, sizeof(_slots));
}

RedjetHandshakeGovernor::~RedjetHandshakeGovernor() {
}

void RedjetHandshakeGovernor::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetHandshakeGovernor();
  }
}

void RedjetHandshakeGovernor::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

bool RedjetHandshakeGovernor::request_handshake(uint32_t target_thread_id, RedjetHandshakeClosure closure, void* ctx) {
  if (closure == nullptr) return false;

  size_t slot_idx = target_thread_id % MAX_THREADS_HANDSHAKE;

  hs_ticket_lock_acquire(&_handshake_lock);

  RedjetHandshakeSlot* slot = &_slots[slot_idx];
  slot->target_thread_id = target_thread_id;
  slot->closure = closure;
  slot->context = ctx;
  slot->request_tsc = hs_read_tsc();
  slot->complete_tsc = 0;
  slot->state = RedjetHandshakeState::HANDSHAKE_REQUESTED;

  _total_handshakes_requested.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_handshake_lock);
  return true;
}

bool RedjetHandshakeGovernor::check_and_execute_handshake(uint32_t target_thread_id) {
  size_t slot_idx = target_thread_id % MAX_THREADS_HANDSHAKE;
  RedjetHandshakeSlot* slot = &_slots[slot_idx];

  if (slot->target_thread_id == target_thread_id &&
      slot->state == RedjetHandshakeState::HANDSHAKE_REQUESTED) {
    slot->state = RedjetHandshakeState::HANDSHAKE_PROCESSING;

    if (slot->closure != nullptr) {
      slot->closure(slot->context, target_thread_id);
    }

    uint64_t now_tsc = hs_read_tsc();
    slot->complete_tsc = now_tsc;
    slot->state = RedjetHandshakeState::HANDSHAKE_COMPLETED;

    uint64_t lat = (now_tsc > slot->request_tsc) ? (now_tsc - slot->request_tsc) : 0;
    _total_handshake_latency_cycles.fetch_add(lat, std::memory_order_relaxed);
    _total_handshakes_completed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

bool RedjetHandshakeGovernor::wait_for_handshake(uint32_t target_thread_id, uint32_t timeout_micros) {
  size_t slot_idx = target_thread_id % MAX_THREADS_HANDSHAKE;
  RedjetHandshakeSlot* slot = &_slots[slot_idx];

  uint64_t start_tsc = hs_read_tsc();
  uint64_t timeout_cycles = static_cast<uint64_t>(timeout_micros) * 3000ULL; // ~3GHz baseline

  while (slot->state != RedjetHandshakeState::HANDSHAKE_COMPLETED) {
    uint64_t elapsed = hs_read_tsc() - start_tsc;
    if (elapsed >= timeout_cycles) {
      return false; // Timeout
    }
    hs_pause();
  }

  return true;
}

void RedjetHandshakeGovernor::print_on(outputStream* st) const {
  if (st == nullptr) return;

  uint64_t completed = _total_handshakes_completed.load(std::memory_order_relaxed);
  uint64_t lat_cycles = _total_handshake_latency_cycles.load(std::memory_order_relaxed);
  double avg_ns = (completed > 0) ? (static_cast<double>(lat_cycles / completed) / 3.0) : 0.0;

  st->print_cr("--- Redjet Deterministic Handshake Governor ---");
  st->print_cr("  Handshakes Requested: %llu", static_cast<unsigned long long>(_total_handshakes_requested.load(std::memory_order_relaxed)));
  st->print_cr("  Handshakes Completed: %llu", static_cast<unsigned long long>(completed));
  st->print_cr("  Average Latency:      %.2f ns", avg_ns);
}
