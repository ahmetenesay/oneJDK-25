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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETMICROEPOCHCOORDINATOR_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETMICROEPOCHCOORDINATOR_HPP

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
// Redjet Deterministic Micro-Epoch Coordinator (PLAN.md Section 7.1, 7.3)
// - Quantized 50-microsecond time-sliced GC work units
// - Replaces STW pauses with continuous fine-grained micro-epochs
// - Phase Progression: DISPATCH -> MARK_SLICE -> REAP_MORGUE -> DRAIN_FALLINE -> REBATE
// - Bounded execution budgets preventing any mutator pause spikes
// ============================================================================

enum class RedjetEpochPhase : uint8_t {
  EPOCH_IDLE          = 0,
  EPOCH_DISPATCH      = 1,
  EPOCH_MARK_SLICE    = 2,
  EPOCH_REAP_MORGUE   = 3,
  EPOCH_DRAIN_FALLINE = 4,
  EPOCH_REBATE        = 5,
  EPOCH_COMPLETE      = 6
};

struct RedjetEpochStats {
  uint64_t epoch_id{ 0 };
  uint64_t duration_tsc{ 0 };
  uint32_t duration_ns{ 0 };
  size_t   objects_marked{ 0 };
  size_t   bytes_reclaimed{ 0 };
  bool     budget_exceeded{ false };
};

class RedjetMicroEpochCoordinator {
public:
  static constexpr uint32_t DEFAULT_BUDGET_MICROS = 50; // 50 microseconds budget
  static constexpr size_t   MAX_HISTORY_EPOCHS    = 1024;

private:
  static RedjetMicroEpochCoordinator* _instance;
  static std::atomic<bool>            _initialized;

  std::atomic<uint64_t>         _current_epoch_id{ 0 };
  std::atomic<RedjetEpochPhase> _current_phase{ RedjetEpochPhase::EPOCH_IDLE };
  uint32_t                      _budget_micros{ DEFAULT_BUDGET_MICROS };
  uint64_t                      _budget_tsc_cycles{ 150000 }; // ~50us at 3GHz

  hs_ticket_lock_t              _coordinator_lock;

  // Telemetry
  std::atomic<uint64_t>         _total_epochs_completed{ 0 };
  std::atomic<uint64_t>         _total_budget_overruns{ 0 };
  std::atomic<uint64_t>         _total_objects_scanned{ 0 };
  std::atomic<uint64_t>         _total_bytes_swept{ 0 };

public:
  RedjetMicroEpochCoordinator();
  ~RedjetMicroEpochCoordinator();

  static void initialize(uint32_t budget_micros = DEFAULT_BUDGET_MICROS);
  static void destroy();
  static RedjetMicroEpochCoordinator* get() noexcept { return _instance; }

  // 1. Begin Micro-Epoch Cycle
  RJ_API uint64_t start_micro_epoch();

  // 2. Advance to Next Phase
  RJ_API bool advance_phase(RedjetEpochPhase expected, RedjetEpochPhase next_phase);

  // 3. Time Budget Watchdog: returns true if current epoch exceeded budget
  RJ_API bool is_budget_exceeded(uint64_t start_tsc) const noexcept;

  // 4. Complete Micro-Epoch & Commit Stats
  RJ_API RedjetEpochStats finish_micro_epoch(uint64_t start_tsc, size_t marked, size_t reclaimed);

  // Status queries
  uint64_t         current_epoch() const noexcept { return _current_epoch_id.load(std::memory_order_relaxed); }
  RedjetEpochPhase current_phase() const noexcept { return _current_phase.load(std::memory_order_relaxed); }
  uint32_t         budget_micros() const noexcept { return _budget_micros; }

  // Telemetry
  uint64_t epochs_completed() const noexcept { return _total_epochs_completed.load(std::memory_order_relaxed); }
  uint64_t budget_overruns()  const noexcept { return _total_budget_overruns.load(std::memory_order_relaxed); }
  uint64_t objects_scanned()  const noexcept { return _total_objects_scanned.load(std::memory_order_relaxed); }
  uint64_t bytes_swept()      const noexcept { return _total_bytes_swept.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETMICROEPOCHCOORDINATOR_HPP
