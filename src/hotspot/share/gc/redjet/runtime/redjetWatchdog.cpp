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

#include "gc/redjet/runtime/redjetWatchdog.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetWatchdog* RedjetWatchdog::_instance = nullptr;
std::atomic<bool> RedjetWatchdog::_initialized{ false };

RedjetWatchdog::RedjetWatchdog()
  : _thread_count(0),
    _total_watchdog_ticks(0),
    _total_stalls_detected(0),
    _total_signals_dispatched(0),
    _total_deadlocks_broken(0) {
  hs_ticket_lock_init(&_watchdog_lock);
  std::memset(_threads, 0, sizeof(_threads));
}

RedjetWatchdog::~RedjetWatchdog() {
}

void RedjetWatchdog::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetWatchdog();
  }
}

void RedjetWatchdog::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetWatchdog::register_thread(uint32_t thread_id) {
  hs_ticket_lock_acquire(&_watchdog_lock);

  for (size_t i = 0; i < _thread_count; ++i) {
    if (_threads[i].thread_id == thread_id) {
      hs_ticket_lock_release(&_watchdog_lock);
      return;
    }
  }

  if (_thread_count < MAX_WATCHED_THREADS) {
    RedjetThreadWatchEntry* entry = &_threads[_thread_count++];
    entry->thread_id = thread_id;
    entry->last_heartbeat_tsc = hs_read_tsc();
    entry->state = RedjetThreadWatchState::THREAD_RESPONSIVE;
    entry->stall_duration_ms = 0;
  }

  hs_ticket_lock_release(&_watchdog_lock);
}

void RedjetWatchdog::unregister_thread(uint32_t thread_id) {
  hs_ticket_lock_acquire(&_watchdog_lock);

  size_t target_idx = MAX_WATCHED_THREADS;
  for (size_t i = 0; i < _thread_count; ++i) {
    if (_threads[i].thread_id == thread_id) {
      target_idx = i;
      break;
    }
  }

  if (target_idx < _thread_count) {
    for (size_t i = target_idx; i + 1 < _thread_count; ++i) {
      _threads[i] = _threads[i + 1];
    }
    _threads[--_thread_count] = RedjetThreadWatchEntry();
  }

  hs_ticket_lock_release(&_watchdog_lock);
}

void RedjetWatchdog::post_heartbeat(uint32_t thread_id) noexcept {
  for (size_t i = 0; i < _thread_count; ++i) {
    if (_threads[i].thread_id == thread_id) {
      _threads[i].last_heartbeat_tsc = hs_read_tsc();
      _threads[i].state = RedjetThreadWatchState::THREAD_RESPONSIVE;
      _threads[i].stall_duration_ms = 0;
      return;
    }
  }
}

size_t RedjetWatchdog::inspect_and_escalate() {
  hs_ticket_lock_acquire(&_watchdog_lock);

  _total_watchdog_ticks.fetch_add(1, std::memory_order_relaxed);
  uint64_t now_tsc = hs_read_tsc();
  size_t escalated_count = 0;

  for (size_t i = 0; i < _thread_count; ++i) {
    RedjetThreadWatchEntry* e = &_threads[i];
    uint64_t elapsed_cycles = (now_tsc > e->last_heartbeat_tsc) ? (now_tsc - e->last_heartbeat_tsc) : 0;
    uint32_t elapsed_ms = static_cast<uint32_t>(elapsed_cycles / 3000000ULL); // ~3GHz baseline

    e->stall_duration_ms = elapsed_ms;

    if (elapsed_ms >= STALL_THRESHOLD_MS && e->state != RedjetThreadWatchState::THREAD_ESCALATED) {
      e->state = RedjetThreadWatchState::THREAD_STALLED;
      _total_stalls_detected.fetch_add(1, std::memory_order_relaxed);

      if (dispatch_preemption_signal(e->thread_id)) {
        _total_signals_dispatched.fetch_add(1, std::memory_order_relaxed);
        e->state = RedjetThreadWatchState::THREAD_ESCALATED;
        _total_deadlocks_broken.fetch_add(1, std::memory_order_relaxed);
        escalated_count++;
      }
    }
  }

  hs_ticket_lock_release(&_watchdog_lock);
  return escalated_count;
}

#if defined(_WIN32)
static void CALLBACK dummy_apc(ULONG_PTR) {}
#endif

bool RedjetWatchdog::dispatch_preemption_signal(uint32_t thread_id) {
#if defined(_WIN32)
  HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, static_cast<DWORD>(thread_id));
  if (hThread != nullptr) {
    QueueUserAPC(dummy_apc, hThread, 0);
    CloseHandle(hThread);
    return true;
  }
  return false;
#elif defined(__linux__)
  // Targeted pthread preemption signal
  pthread_t pt = static_cast<pthread_t>(thread_id);
  return (pthread_kill(pt, SIGUSR2) == 0);
#else
  return false;
#endif
}

void RedjetWatchdog::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Real-Time Thread Watchdog ---");
  st->print_cr("  Watched Threads:      %zu / %zu", _thread_count, MAX_WATCHED_THREADS);
  st->print_cr("  Watchdog Ticks:       %llu", static_cast<unsigned long long>(_total_watchdog_ticks.load(std::memory_order_relaxed)));
  st->print_cr("  Stalls Detected:      %llu (> %u ms)",
               static_cast<unsigned long long>(_total_stalls_detected.load(std::memory_order_relaxed)), STALL_THRESHOLD_MS);
  st->print_cr("  Signals Dispatched:   %llu", static_cast<unsigned long long>(_total_signals_dispatched.load(std::memory_order_relaxed)));
  st->print_cr("  Deadlocks Broken:     %llu", static_cast<unsigned long long>(_total_deadlocks_broken.load(std::memory_order_relaxed)));
}
