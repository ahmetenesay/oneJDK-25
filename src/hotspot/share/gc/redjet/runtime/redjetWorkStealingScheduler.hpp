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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETWORKSTEALINGSCHEDULER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETWORKSTEALINGSCHEDULER_HPP

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
// Redjet Chase-Lev Work-Stealing Scheduler (PLAN.md Section 8.8.1)
// - Lock-free multi-worker work stealing using circular Chase-Lev deques
// - LIFO push/pop for owner thread (data locality preservation)
// - FIFO steal for idle peer workers (maximum parallel slice distribution)
// - CCD-aware steal topology: prefers stealing from workers in the same CCD
// - STRICT MUTUAL EXCLUSION: When -XX:RedjetGCScheduler=Domestic is active,
//   this scheduler is 100% quiesced and consumes ZERO CPU cycles.
// ============================================================================

typedef void (*RedjetTaskFn)(void* context, uint32_t worker_id);

struct alignas(64) RedjetTask {
  RedjetTaskFn fn{ nullptr };
  void*        context{ nullptr };
  uint32_t     task_id{ 0 };
  uint32_t     origin_ccd{ 0 };
  uint64_t     enqueue_tsc{ 0 };
  uint32_t     priority{ 0 };
};

// Circular Chase-Lev Deque with dynamic power-of-two capacity
class alignas(64) RedjetChaseLevDeque {
public:
  static constexpr size_t DEQUE_CAPACITY = 2048;
  static constexpr size_t DEQUE_MASK     = DEQUE_CAPACITY - 1;

private:
  RedjetTask            _buffer[DEQUE_CAPACITY];
  alignas(64) std::atomic<int64_t> _top{ 0 };
  alignas(64) std::atomic<int64_t> _bottom{ 0 };

  std::atomic<uint64_t> _pushes{ 0 };
  std::atomic<uint64_t> _pops{ 0 };
  std::atomic<uint64_t> _steals{ 0 };
  std::atomic<uint64_t> _overflows{ 0 };

public:
  RedjetChaseLevDeque();
  ~RedjetChaseLevDeque();

  // Owner operations
  bool push(const RedjetTask& task) noexcept;
  bool pop(RedjetTask* out_task) noexcept;

  // Remote stealer operation
  bool steal(RedjetTask* out_task) noexcept;

  size_t size() const noexcept;
  bool is_empty() const noexcept { return size() == 0; }

  uint64_t pushes()    const noexcept { return _pushes.load(std::memory_order_relaxed); }
  uint64_t pops()      const noexcept { return _pops.load(std::memory_order_relaxed); }
  uint64_t steals()    const noexcept { return _steals.load(std::memory_order_relaxed); }
  uint64_t overflows() const noexcept { return _overflows.load(std::memory_order_relaxed); }
};

// Worker Thread Descriptor
struct alignas(64) RedjetWorkerDescriptor {
  uint32_t worker_id{ 0 };
  uint32_t ccd_id{ 0 };
  uint32_t socket_id{ 0 };
  bool     is_active{ false };
  uint64_t tasks_completed{ 0 };
  uint64_t steal_cycles{ 0 };
};

class RedjetWorkStealingScheduler {
public:
  static constexpr size_t MAX_WORKERS = 64;

private:
  static RedjetWorkStealingScheduler* _instance;
  static std::atomic<bool>            _initialized;

  RedjetChaseLevDeque                 _deques[MAX_WORKERS];
  RedjetWorkerDescriptor              _descriptors[MAX_WORKERS];
  size_t                              _num_workers{ 8 };
  hs_ticket_lock_t                    _scheduler_lock;

  // Mutual exclusion suppression flag
  std::atomic<bool>                   _is_suppressed{ false };

  // Telemetry
  std::atomic<uint64_t>               _total_tasks_submitted{ 0 };
  std::atomic<uint64_t>               _total_tasks_executed{ 0 };
  std::atomic<uint64_t>               _total_tasks_stolen{ 0 };
  std::atomic<uint64_t>               _total_steal_attempts{ 0 };
  std::atomic<uint64_t>               _total_intra_ccd_steals{ 0 };
  std::atomic<uint64_t>               _total_inter_ccd_steals{ 0 };
  std::atomic<uint64_t>               _total_suppressed_submissions{ 0 };

public:
  RedjetWorkStealingScheduler();
  ~RedjetWorkStealingScheduler();

  static void initialize(size_t workers = 8);
  static void destroy();
  static RedjetWorkStealingScheduler* get() noexcept { return _instance; }

  // Set suppression state (called when Domestic Scheduler is active)
  void set_suppressed(bool suppressed) noexcept {
    _is_suppressed.store(suppressed, std::memory_order_release);
  }
  bool is_suppressed() const noexcept {
    return _is_suppressed.load(std::memory_order_relaxed);
  }

  // 1. Submit Task to Worker's local deque
  RJ_API bool submit_task(uint32_t worker_id, RedjetTaskFn fn, void* ctx, uint32_t priority = 0);

  // 2. Fetch or Steal Task (worker executes local first, then steals from peers)
  RJ_API bool get_or_steal_task(uint32_t worker_id, RedjetTask* out_task);

  // 3. Topology-Aware Steal Engine (prioritizes same CCD, then same socket)
  RJ_API bool steal_topology_aware(uint32_t thief_id, RedjetTask* out_task);

  // 4. Worker Execution Loop
  RJ_API void execute_worker_loop(uint32_t worker_id, std::atomic<bool>& terminate_flag);

  // Telemetry
  uint64_t tasks_submitted()       const noexcept { return _total_tasks_submitted.load(std::memory_order_relaxed); }
  uint64_t tasks_executed()        const noexcept { return _total_tasks_executed.load(std::memory_order_relaxed); }
  uint64_t tasks_stolen()          const noexcept { return _total_tasks_stolen.load(std::memory_order_relaxed); }
  uint64_t steal_attempts()        const noexcept { return _total_steal_attempts.load(std::memory_order_relaxed); }
  uint64_t intra_ccd_steals()      const noexcept { return _total_intra_ccd_steals.load(std::memory_order_relaxed); }
  uint64_t inter_ccd_steals()      const noexcept { return _total_inter_ccd_steals.load(std::memory_order_relaxed); }
  uint64_t suppressed_submissions()const noexcept { return _total_suppressed_submissions.load(std::memory_order_relaxed); }

  size_t worker_count() const noexcept { return _num_workers; }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETWORKSTEALINGSCHEDULER_HPP
