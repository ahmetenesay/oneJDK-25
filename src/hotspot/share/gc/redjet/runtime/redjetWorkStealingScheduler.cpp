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

#include "gc/redjet/runtime/redjetWorkStealingScheduler.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <intrin.h>
#define RJ_WS_PAUSE() _mm_pause()
#elif defined(_M_ARM64)
#include <intrin.h>
#define RJ_WS_PAUSE() __yield()
#else
#define RJ_WS_PAUSE() ((void)0)
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define RJ_WS_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define RJ_WS_PAUSE() asm volatile("yield" ::: "memory")
#elif defined(__riscv)
#define RJ_WS_PAUSE() asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory")
#else
#define RJ_WS_PAUSE() ((void)0)
#endif

// ============================================================================
// Chase-Lev Deque Implementation
// ============================================================================

RedjetChaseLevDeque::RedjetChaseLevDeque()
  : _top(0), _bottom(0),
    _pushes(0), _pops(0), _steals(0), _overflows(0) {
  std::memset(_buffer, 0, sizeof(_buffer));
}

RedjetChaseLevDeque::~RedjetChaseLevDeque() {
}

bool RedjetChaseLevDeque::push(const RedjetTask& task) noexcept {
  int64_t b = _bottom.load(std::memory_order_relaxed);
  int64_t t = _top.load(std::memory_order_acquire);

  if (b - t >= static_cast<int64_t>(DEQUE_CAPACITY)) {
    _overflows.fetch_add(1, std::memory_order_relaxed);
    return false; // Deque capacity saturated
  }

  _buffer[b & DEQUE_MASK] = task;
  std::atomic_thread_fence(std::memory_order_release);
  _bottom.store(b + 1, std::memory_order_relaxed);
  _pushes.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool RedjetChaseLevDeque::pop(RedjetTask* out_task) noexcept {
  int64_t b = _bottom.load(std::memory_order_relaxed) - 1;
  _bottom.store(b, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  int64_t t = _top.load(std::memory_order_relaxed);

  if (t <= b) {
    // Non-empty deque
    RedjetTask item = _buffer[b & DEQUE_MASK];
    if (t == b) {
      // Last item: contest with concurrent stealers
      if (!_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        _bottom.store(b + 1, std::memory_order_relaxed);
        return false;
      }
      _bottom.store(b + 1, std::memory_order_relaxed);
    }
    if (out_task != nullptr) *out_task = item;
    _pops.fetch_add(1, std::memory_order_relaxed);
    return true;
  } else {
    // Empty deque
    _bottom.store(b + 1, std::memory_order_relaxed);
    return false;
  }
}

bool RedjetChaseLevDeque::steal(RedjetTask* out_task) noexcept {
  int64_t t = _top.load(std::memory_order_acquire);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  int64_t b = _bottom.load(std::memory_order_acquire);

  if (t < b) {
    RedjetTask item = _buffer[t & DEQUE_MASK];
    if (_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
      if (out_task != nullptr) *out_task = item;
      _steals.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

size_t RedjetChaseLevDeque::size() const noexcept {
  int64_t b = _bottom.load(std::memory_order_relaxed);
  int64_t t = _top.load(std::memory_order_relaxed);
  return (b > t) ? static_cast<size_t>(b - t) : 0;
}

// ============================================================================
// Redjet Work-Stealing Scheduler Implementation
// ============================================================================

RedjetWorkStealingScheduler* RedjetWorkStealingScheduler::_instance = nullptr;
std::atomic<bool> RedjetWorkStealingScheduler::_initialized{ false };

RedjetWorkStealingScheduler::RedjetWorkStealingScheduler()
  : _num_workers(8),
    _is_suppressed(false),
    _total_tasks_submitted(0),
    _total_tasks_executed(0),
    _total_tasks_stolen(0),
    _total_steal_attempts(0),
    _total_intra_ccd_steals(0),
    _total_inter_ccd_steals(0),
    _total_suppressed_submissions(0) {
  hs_ticket_lock_init(&_scheduler_lock);
  for (size_t i = 0; i < MAX_WORKERS; ++i) {
    _descriptors[i].worker_id = static_cast<uint32_t>(i);
    _descriptors[i].ccd_id = static_cast<uint32_t>(i / 8);
    _descriptors[i].socket_id = static_cast<uint32_t>(i / 32);
    _descriptors[i].is_active = false;
    _descriptors[i].tasks_completed = 0;
    _descriptors[i].steal_cycles = 0;
  }
}

RedjetWorkStealingScheduler::~RedjetWorkStealingScheduler() {
}

void RedjetWorkStealingScheduler::initialize(size_t workers) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetWorkStealingScheduler();
    _instance->_num_workers = std::max<size_t>(1, std::min<size_t>(workers, MAX_WORKERS));
    for (size_t i = 0; i < _instance->_num_workers; ++i) {
      _instance->_descriptors[i].is_active = true;
    }
  }
}

void RedjetWorkStealingScheduler::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

bool RedjetWorkStealingScheduler::submit_task(uint32_t worker_id, RedjetTaskFn fn, void* ctx, uint32_t priority) {
  if (fn == nullptr || worker_id >= _num_workers) return false;

  // Strict mutual exclusion enforcement:
  // If Domestic Scheduler is active, TBB work-stealing submission is suppressed!
  if (_is_suppressed.load(std::memory_order_relaxed)) {
    _total_suppressed_submissions.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  RedjetTask t;
  t.fn = fn;
  t.context = ctx;
  t.task_id = static_cast<uint32_t>(_total_tasks_submitted.fetch_add(1, std::memory_order_relaxed));
  t.origin_ccd = _descriptors[worker_id].ccd_id;
  t.enqueue_tsc = sacculus_rdtsc();
  t.priority = priority;

  return _deques[worker_id].push(t);
}

bool RedjetWorkStealingScheduler::get_or_steal_task(uint32_t worker_id, RedjetTask* out_task) {
  if (out_task == nullptr || worker_id >= _num_workers) return false;

  if (_is_suppressed.load(std::memory_order_relaxed)) {
    return false;
  }

  // 1. Try local worker deque (LIFO - highest cache locality)
  if (_deques[worker_id].pop(out_task)) {
    _total_tasks_executed.fetch_add(1, std::memory_order_relaxed);
    _descriptors[worker_id].tasks_completed++;
    return true;
  }

  // 2. Local deque is empty: invoke topology-aware stealer (FIFO - peer deque)
  return steal_topology_aware(worker_id, out_task);
}

bool RedjetWorkStealingScheduler::steal_topology_aware(uint32_t thief_id, RedjetTask* out_task) {
  if (thief_id >= _num_workers || out_task == nullptr) return false;

  if (_is_suppressed.load(std::memory_order_relaxed)) {
    return false;
  }

  _total_steal_attempts.fetch_add(1, std::memory_order_relaxed);
  uint32_t thief_ccd = _descriptors[thief_id].ccd_id;
  uint32_t thief_socket = _descriptors[thief_id].socket_id;
  uint64_t steal_start = sacculus_rdtsc();

  // Tier 1: Intra-CCD steals (Workers sharing same L3 cache)
  size_t start_offset = static_cast<size_t>(steal_start) % _num_workers;
  for (size_t step = 0; step < _num_workers; ++step) {
    uint32_t victim = static_cast<uint32_t>((start_offset + step) % _num_workers);
    if (victim != thief_id && _descriptors[victim].ccd_id == thief_ccd) {
      if (_deques[victim].steal(out_task)) {
        _total_tasks_stolen.fetch_add(1, std::memory_order_relaxed);
        _total_intra_ccd_steals.fetch_add(1, std::memory_order_relaxed);
        _total_tasks_executed.fetch_add(1, std::memory_order_relaxed);
        _descriptors[thief_id].tasks_completed++;
        _descriptors[thief_id].steal_cycles += (sacculus_rdtsc() - steal_start);
        return true;
      }
    }
  }

  // Tier 2: Inter-CCD intra-socket steals
  for (size_t step = 0; step < _num_workers; ++step) {
    uint32_t victim = static_cast<uint32_t>((start_offset + step) % _num_workers);
    if (victim != thief_id && _descriptors[victim].socket_id == thief_socket &&
        _descriptors[victim].ccd_id != thief_ccd) {
      if (_deques[victim].steal(out_task)) {
        _total_tasks_stolen.fetch_add(1, std::memory_order_relaxed);
        _total_inter_ccd_steals.fetch_add(1, std::memory_order_relaxed);
        _total_tasks_executed.fetch_add(1, std::memory_order_relaxed);
        _descriptors[thief_id].tasks_completed++;
        _descriptors[thief_id].steal_cycles += (sacculus_rdtsc() - steal_start);
        return true;
      }
    }
  }

  return false;
}

void RedjetWorkStealingScheduler::execute_worker_loop(uint32_t worker_id, std::atomic<bool>& terminate_flag) {
  if (worker_id >= _num_workers) return;

  uint32_t consecutive_idle = 0;
  RedjetTask task;

  while (!terminate_flag.load(std::memory_order_relaxed)) {
    if (_is_suppressed.load(std::memory_order_relaxed)) {
      // Quiesced mode: thread sleeps to avoid bus noise
      for (int p = 0; p < 16; ++p) {
        RJ_WS_PAUSE();
      }
      continue;
    }

    if (get_or_steal_task(worker_id, &task)) {
      consecutive_idle = 0;
      if (task.fn != nullptr) {
        task.fn(task.context, worker_id);
      }
    } else {
      consecutive_idle++;
      if (consecutive_idle < 32) {
        RJ_WS_PAUSE();
      } else if (consecutive_idle < 256) {
        for (int p = 0; p < 4; ++p) RJ_WS_PAUSE();
      } else {
        std::this_thread::yield();
      }
    }
  }
}

void RedjetWorkStealingScheduler::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet Work-Stealing Scheduler (TBB Engine) ===");
  st->print_cr("  Status: %s", _is_suppressed.load(std::memory_order_relaxed) ? "QUIESCED (Suppressed by Domestic)" : "ACTIVE");
  st->print_cr("  Workers: " SIZE_FORMAT, _num_workers);
  st->print_cr("  Tasks Submitted: " UINT64_FORMAT, tasks_submitted());
  st->print_cr("  Tasks Executed: " UINT64_FORMAT, tasks_executed());
  st->print_cr("  Tasks Stolen: " UINT64_FORMAT, tasks_stolen());
  st->print_cr("  Steal Attempts: " UINT64_FORMAT, steal_attempts());
  st->print_cr("  Intra-CCD Steals: " UINT64_FORMAT, intra_ccd_steals());
  st->print_cr("  Inter-CCD Steals: " UINT64_FORMAT, inter_ccd_steals());
  st->print_cr("  Suppressed Submissions: " UINT64_FORMAT, suppressed_submissions());

  for (size_t i = 0; i < _num_workers; ++i) {
    st->print_cr("    Worker %u: CCD %u, Completed " UINT64_FORMAT ", Queue Size " SIZE_FORMAT,
                 _descriptors[i].worker_id, _descriptors[i].ccd_id,
                 _descriptors[i].tasks_completed, _deques[i].size());
  }
}
