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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETSCHEDULER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETSCHEDULER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Redjet Bare-Metal Scheduler & Hardware Core Queues (PLAN.md Faz 5 & Bölüm 7)
// Standalone Bare-Metal Execution & Hybrid TBB Cooperative Scheduling
// Total Silicon Exploitation for Threadripper 9995WX, Xeon 6980P & Ampere Altra Max
// ============================================================================

enum class RedjetSchedulerMode : uint8_t {
  STANDALONE_BARE_METAL  = 0, // Direct OS threads pinned to hardware core queues
  HYBRID_TBB_COOPERATIVE = 1  // Fine-grain GC/Puzzler micro-tasks on core queues, Java on TBB
};

struct alignas(64) RedjetHardwareTask {
  void (*func)(void*){ nullptr };
  void*    data{ nullptr };
  uint32_t affinity_core{ UINT32_MAX };
  uint32_t priority{ 0 };
  uint64_t enqueue_tsc{ 0 };
};

class alignas(64) RedjetPerCoreQueue {
public:
  static constexpr size_t QUEUE_CAPACITY = 512;
  static constexpr size_t QUEUE_MASK     = QUEUE_CAPACITY - 1;

private:
  RedjetHardwareTask                 _tasks[QUEUE_CAPACITY];
  alignas(64) std::atomic<uint64_t>  _head{ 0 };
  alignas(64) std::atomic<uint64_t>  _tail{ 0 };

  uint32_t                           _core_id{ 0 };
  uint32_t                           _ccd_id{ 0 };
  uint32_t                           _socket_id{ 0 };

  std::atomic<uint64_t>              _tasks_executed{ 0 };
  std::atomic<uint64_t>              _tasks_stolen_out{ 0 };
  std::atomic<uint64_t>              _tasks_stolen_in{ 0 };

public:
  RedjetPerCoreQueue() = default;

  void init(uint32_t core_id, uint32_t ccd_id, uint32_t socket_id);

  // Local push (called by core owner)
  bool push_local(void (*func)(void*), void* data, uint32_t priority = 0);

  // Local pop (LIFO/FIFO execution by core owner)
  bool pop_local(RedjetHardwareTask* out_task);

  // Remote steal (called by peer cores on starve)
  bool steal_remote(RedjetHardwareTask* out_task);

  size_t   size() const noexcept;
  uint32_t core_id() const noexcept { return _core_id; }
  uint32_t ccd_id() const noexcept { return _ccd_id; }
  uint32_t socket_id() const noexcept { return _socket_id; }

  uint64_t tasks_executed() const noexcept { return _tasks_executed.load(std::memory_order_relaxed); }
  uint64_t tasks_stolen_out() const noexcept { return _tasks_stolen_out.load(std::memory_order_relaxed); }
  uint64_t tasks_stolen_in() const noexcept { return _tasks_stolen_in.load(std::memory_order_relaxed); }
};

class RedjetBareMetalScheduler {
public:
  static constexpr size_t MAX_HARDWARE_CORES = 256;

private:
  RedjetPerCoreQueue       _core_queues[MAX_HARDWARE_CORES];
  uint32_t                 _active_cores{ 1 };
  RedjetSchedulerMode      _mode{ RedjetSchedulerMode::STANDALONE_BARE_METAL };
  std::atomic<bool>        _running{ false };

  std::atomic<uint64_t>    _total_scheduled_tasks{ 0 };
  std::atomic<uint64_t>    _total_mesh_steals{ 0 };

public:
  RedjetBareMetalScheduler();
  ~RedjetBareMetalScheduler();

  RJ_API void init(uint32_t core_count = 0, RedjetSchedulerMode mode = RedjetSchedulerMode::STANDALONE_BARE_METAL);
  RJ_API void shutdown();

  // Enqueues task to target core queue or nearest CCD queue
  RJ_API bool schedule(void (*func)(void*), void* data, uint32_t core_hint = UINT32_MAX, uint32_t priority = 0);

  // Executes next available task on calling core; if idle, initiates mesh-aware stealing
  RJ_API bool dispatch_next_task(uint32_t core_id);

  // Mesh-Aware Work Stealing Algorithm (SMT -> CCX/CCD -> SNC/Socket -> Mesh)
  RJ_API bool steal_mesh_aware(uint32_t thief_core, RedjetHardwareTask* out_task);

  // Sizing & Status
  uint32_t active_cores() const noexcept { return _active_cores; }
  RedjetSchedulerMode mode() const noexcept { return _mode; }
  bool is_running() const noexcept { return _running.load(std::memory_order_relaxed); }

  uint64_t total_scheduled_tasks() const noexcept { return _total_scheduled_tasks.load(std::memory_order_relaxed); }
  uint64_t total_mesh_steals() const noexcept { return _total_mesh_steals.load(std::memory_order_relaxed); }

  RedjetPerCoreQueue* get_core_queue(uint32_t core_id) {
    return (core_id < _active_cores) ? &_core_queues[core_id] : nullptr;
  }
};

extern RedjetBareMetalScheduler g_redjet_scheduler;

#endif // SHARE_GC_REDJET_RUNTIME_REDJETSCHEDULER_HPP
