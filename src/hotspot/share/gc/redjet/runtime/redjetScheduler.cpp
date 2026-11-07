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

#include "gc/redjet/runtime/redjetScheduler.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <algorithm>
#include <thread>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <intrin.h>
#define RJ_SCHED_PAUSE() _mm_pause()
#elif defined(_M_ARM64)
#include <intrin.h>
#define RJ_SCHED_PAUSE() __yield()
#else
#define RJ_SCHED_PAUSE() ((void)0)
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define RJ_SCHED_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define RJ_SCHED_PAUSE() asm volatile("yield" ::: "memory")
#elif defined(__riscv)
#define RJ_SCHED_PAUSE() asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory")
#else
#define RJ_SCHED_PAUSE() ((void)0)
#endif

RedjetBareMetalScheduler g_redjet_scheduler;

// ============================================================================
// RedjetPerCoreQueue Implementation
// ============================================================================

void RedjetPerCoreQueue::init(uint32_t core_id, uint32_t ccd_id, uint32_t socket_id) {
  _core_id = core_id;
  _ccd_id = ccd_id;
  _socket_id = socket_id;
  _head.store(0, std::memory_order_relaxed);
  _tail.store(0, std::memory_order_relaxed);
  _tasks_executed.store(0, std::memory_order_relaxed);
  _tasks_stolen_out.store(0, std::memory_order_relaxed);
  _tasks_stolen_in.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < QUEUE_CAPACITY; ++i) {
    _tasks[i].func.store(nullptr, std::memory_order_relaxed);
    _tasks[i].data = nullptr;
    _tasks[i].affinity_core = UINT32_MAX;
    _tasks[i].priority = 0;
    _tasks[i].enqueue_tsc = 0;
  }
}

bool RedjetPerCoreQueue::push_local(void (*func)(void*), void* data, uint32_t priority) {
  if (func == nullptr) return false;

  uint64_t tail = _tail.load(std::memory_order_relaxed);
  do {
    uint64_t head = _head.load(std::memory_order_acquire);
    if (tail - head >= QUEUE_CAPACITY) {
      return false; // Queue full backpressure
    }
  } while (!_tail.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

  size_t idx = static_cast<size_t>(tail & QUEUE_MASK);
  _tasks[idx].data = data;
  _tasks[idx].affinity_core = _core_id;
  _tasks[idx].priority = priority;
  _tasks[idx].enqueue_tsc = sacculus_rdtsc();
  // Store function pointer with release semantics
  _tasks[idx].func.store(func, std::memory_order_release);

  return true;
}

bool RedjetPerCoreQueue::pop_local(RedjetHardwareTask* out_task) {
  if (out_task == nullptr) return false;

  uint64_t tail = _tail.load(std::memory_order_relaxed);
  uint64_t head = _head.load(std::memory_order_acquire);

  if (tail <= head) {
    return false; // Empty
  }

  // LIFO pop from tail for hot L1/L2 data cache reuse
  uint64_t target_tail = tail - 1;
  _tail.store(target_tail, std::memory_order_relaxed);

  size_t idx = static_cast<size_t>(target_tail & QUEUE_MASK);
  auto fn_ptr = _tasks[idx].func.exchange(nullptr, std::memory_order_acq_rel);

  if (fn_ptr != nullptr) {
    out_task->func.store(fn_ptr, std::memory_order_relaxed);
    out_task->data = _tasks[idx].data;
    out_task->affinity_core = _tasks[idx].affinity_core;
    out_task->priority = _tasks[idx].priority;
    out_task->enqueue_tsc = _tasks[idx].enqueue_tsc;
    _tasks_executed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Contention with remote stealer: restore tail
  _tail.store(tail, std::memory_order_relaxed);
  return false;
}

bool RedjetPerCoreQueue::steal_remote(RedjetHardwareTask* out_task) {
  if (out_task == nullptr) return false;

  uint64_t head = _head.load(std::memory_order_acquire);
  uint64_t tail = _tail.load(std::memory_order_acquire);

  if (head >= tail) {
    return false; // Empty
  }

  // FIFO steal from head for fairness
  size_t idx = static_cast<size_t>(head & QUEUE_MASK);
  auto fn_ptr = _tasks[idx].func.load(std::memory_order_acquire);

  if (fn_ptr != nullptr) {
    if (_tasks[idx].func.compare_exchange_strong(fn_ptr, nullptr, std::memory_order_acq_rel)) {
      out_task->func.store(fn_ptr, std::memory_order_relaxed);
      out_task->data = _tasks[idx].data;
      out_task->affinity_core = _tasks[idx].affinity_core;
      out_task->priority = _tasks[idx].priority;
      out_task->enqueue_tsc = _tasks[idx].enqueue_tsc;

      _head.compare_exchange_strong(head, head + 1, std::memory_order_release);
      _tasks_stolen_out.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  return false;
}

size_t RedjetPerCoreQueue::size() const noexcept {
  uint64_t head = _head.load(std::memory_order_relaxed);
  uint64_t tail = _tail.load(std::memory_order_relaxed);
  return (tail > head) ? static_cast<size_t>(tail - head) : 0;
}

// ============================================================================
// RedjetBareMetalScheduler Implementation
// ============================================================================

RedjetBareMetalScheduler::RedjetBareMetalScheduler() :
  _active_cores(1),
  _mode(RedjetSchedulerMode::STANDALONE_BARE_METAL),
  _running(false) {
}

RedjetBareMetalScheduler::~RedjetBareMetalScheduler() {
  shutdown();
}

void RedjetBareMetalScheduler::init(uint32_t core_count, RedjetSchedulerMode mode) {
  if (core_count == 0) {
    unsigned int hw_threads = std::thread::hardware_concurrency();
    core_count = (hw_threads > 0) ? hw_threads : 1;
  }
  _active_cores = std::min<uint32_t>(core_count, static_cast<uint32_t>(MAX_HARDWARE_CORES));
  _mode = mode;

  // Configure hardware topology mapping
  // Threadripper 9995WX: 8 cores per CCD, 12 CCDs
  // Xeon 6980P: ~42 cores per SNC-3 cluster
  // Ampere Altra Max: 128 cores on mesh
  for (uint32_t i = 0; i < _active_cores; ++i) {
    uint32_t ccd = i / 8;        // Default 8-core CCX/CCD block
    uint32_t socket = i / 64;    // Default 64-core socket
    _core_queues[i].init(i, ccd, socket);
  }

  _running.store(true, std::memory_order_release);
}

void RedjetBareMetalScheduler::shutdown() {
  _running.store(false, std::memory_order_release);
}

bool RedjetBareMetalScheduler::schedule(void (*func)(void*), void* data, uint32_t core_hint, uint32_t priority) {
  if (func == nullptr || !_running.load(std::memory_order_relaxed)) {
    return false;
  }

  uint32_t target_core = (core_hint < _active_cores) ? core_hint : 0;
  if (_core_queues[target_core].push_local(func, data, priority)) {
    _total_scheduled_tasks.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Target queue full: try nearby cores in the same CCD
  uint32_t ccd = _core_queues[target_core].ccd_id();
  for (uint32_t i = 0; i < _active_cores; ++i) {
    if (_core_queues[i].ccd_id() == ccd && i != target_core) {
      if (_core_queues[i].push_local(func, data, priority)) {
        _total_scheduled_tasks.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }

  // Fallback to core 0
  if (_core_queues[0].push_local(func, data, priority)) {
    _total_scheduled_tasks.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

bool RedjetBareMetalScheduler::dispatch_next_task(uint32_t core_id) {
  if (core_id >= _active_cores) return false;

  RedjetHardwareTask task;
  // 1. Try local queue pop (fastest, zero cross-core communication)
  if (_core_queues[core_id].pop_local(&task)) {
    auto fn = task.func.load(std::memory_order_relaxed);
    if (fn != nullptr) {
      fn(task.data);
      return true;
    }
  }

  // 2. Local queue empty: initiate Mesh-Aware Work Stealing
  if (steal_mesh_aware(core_id, &task)) {
    auto fn = task.func.load(std::memory_order_relaxed);
    if (fn != nullptr) {
      fn(task.data);
      return true;
    }
  }

  return false;
}

bool RedjetBareMetalScheduler::steal_mesh_aware(uint32_t thief_core, RedjetHardwareTask* out_task) {
  if (out_task == nullptr || thief_core >= _active_cores) return false;

  uint32_t thief_ccd = _core_queues[thief_core].ccd_id();
  uint32_t thief_socket = _core_queues[thief_core].socket_id();

  // Tier 1: Steal from SMT sibling (core ^ 1)
  uint32_t smt_sibling = thief_core ^ 1;
  if (smt_sibling < _active_cores && _core_queues[smt_sibling].size() > 0) {
    if (_core_queues[smt_sibling].steal_remote(out_task)) {
      _total_mesh_steals.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  // Tier 2: Steal from same CCD / CCX (sharing local L3 cache slice)
  for (uint32_t c = 0; c < _active_cores; ++c) {
    if (c != thief_core && _core_queues[c].ccd_id() == thief_ccd) {
      if (_core_queues[c].size() > 0 && _core_queues[c].steal_remote(out_task)) {
        _total_mesh_steals.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }

  // Tier 3: Steal from same NUMA Socket / SNC Cluster
  for (uint32_t c = 0; c < _active_cores; ++c) {
    if (_core_queues[c].socket_id() == thief_socket && _core_queues[c].ccd_id() != thief_ccd) {
      if (_core_queues[c].size() > 0 && _core_queues[c].steal_remote(out_task)) {
        _total_mesh_steals.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }

  // Tier 4: Steal across Mesh Interconnect / Remote Socket
  for (uint32_t c = 0; c < _active_cores; ++c) {
    if (_core_queues[c].socket_id() != thief_socket) {
      if (_core_queues[c].size() > 0 && _core_queues[c].steal_remote(out_task)) {
        _total_mesh_steals.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }

  return false;
}
