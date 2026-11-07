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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETSNATCHREBATE_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETSNATCHREBATE_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include <atomic>
#include <cstdint>
#include <cstddef>

#ifndef RJ_API
#if defined(_WIN32) || defined(__CYGWIN__)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Snatch & Rebate Mimarisi (PLAN.md Section 9.6.1)
//
// 1. Snatch: O(1) Sub-Microsecond EmptyBox Acquisition from Pretrashing Cache
// 2. Rebate Target Distribution:
//    - Heap: Internal redistribution to Runway/TLABs/Hotfield slabs (0 syscalls, 0 TLB flushes)
//    - OS: Immediate Return-To-OS via VirtualFree(MEM_DECOMMIT) / madvise(MADV_DONTNEED)
//    - Auto: Coach autonomously arbitrates based on mutator throughput and RAM pressure
// 3. Pacing / Effort:
//    - GYC (Generous Yet Cautious): Gold standard, buffers against allocation spikes
//    - RAPID: Instant decommit/redistribution per freed block
//    - LAZY: Batches empty blocks until batch threshold is satisfied
//    - AUTO: Dynamically calibrated by Coach
// ============================================================================

enum class RedjetRebateTarget : uint8_t {
  TARGET_HEAP = 0, // Internal redistribution (Max throughput)
  TARGET_OS   = 1, // Physical release to operating system (Min RSS)
  TARGET_AUTO = 2  // Autonomous Coach arbitration
};

enum class RedjetSnatchAndRebateMode : uint8_t {
  MODE_GYC   = 0, // Generous Yet Cautious (Default)
  MODE_RAPID = 1, // Aggressive immediate rebate
  MODE_LAZY  = 2, // Deferred batched rebate
  MODE_AUTO  = 3  // Autonomous pacing
};

enum RedjetSlotState : uint32_t {
  SLOT_STATE_FREE             = 0,
  SLOT_STATE_AVAILABLE        = 1,
  SLOT_STATE_CLAIMED          = 2,
  SLOT_STATE_RESERVED_DEPOSIT = 3
};

// Size-class bucket for EmptyBox O(1) snatching
struct alignas(64) RedjetEmptyBoxSlot {
  uintptr_t             address{ 0 };
  size_t                size_bytes{ 0 };
  uint32_t              size_class{ 0 };
  std::atomic<uint32_t> state{ SLOT_STATE_FREE }; // 0: Free, 1: Available, 2: Claimed, 3: Reserved for deposit
  uint64_t              snatch_tsc{ 0 };
};

// Telemetry counters for Snatch & Rebate subsystem
struct alignas(64) RedjetSnatchRebateMetrics {
  std::atomic<uint64_t> total_snatch_attempts{ 0 };
  std::atomic<uint64_t> total_snatch_hits{ 0 };
  std::atomic<uint64_t> total_snatch_misses{ 0 };
  std::atomic<uint64_t> total_snatched_bytes{ 0 };

  std::atomic<uint64_t> total_rebate_cycles{ 0 };
  std::atomic<uint64_t> heap_redistributed_bytes{ 0 };
  std::atomic<uint64_t> os_decommitted_bytes{ 0 };
  std::atomic<uint64_t> os_decommit_syscalls{ 0 };

  std::atomic<uint64_t> gyc_buffer_retained_bytes{ 0 };
  std::atomic<uint64_t> rapid_rebate_events{ 0 };
  std::atomic<uint64_t> lazy_batch_flushes{ 0 };
};

// ============================================================================
// RedjetSnatchRebateEngine Class
// ============================================================================
class RedjetSnatchRebateEngine : public AllStatic {
public:
  static constexpr size_t NUM_SIZE_CLASSES   = 32;
  static constexpr size_t SLOTS_PER_CLASS    = 128;
  static constexpr size_t GYC_BUFFER_CAPACITY = 32 * 1024 * 1024; // 32 MB cautious headroom

private:
  static alignas(64) RedjetEmptyBoxSlot      _emptybox_pool[NUM_SIZE_CLASSES][SLOTS_PER_CLASS];
  static alignas(64) std::atomic<size_t>     _pool_counts[NUM_SIZE_CLASSES];
  static alignas(64) std::atomic<RedjetRebateTarget>         _target;
  static alignas(64) std::atomic<RedjetSnatchAndRebateMode>  _mode;
  static alignas(64) std::atomic<size_t>     _gyc_buffered_bytes;
  static alignas(64) std::atomic<bool>       _initialized;

  static RedjetSnatchRebateMetrics           _metrics;

  static size_t calculate_size_class(size_t size) noexcept;

  // OS decommit helpers
  static bool decommit_to_os(uintptr_t addr, size_t size);

public:
  RJ_API static void initialize(RedjetRebateTarget target = RedjetRebateTarget::TARGET_AUTO,
                                RedjetSnatchAndRebateMode mode = RedjetSnatchAndRebateMode::MODE_GYC);
  RJ_API static void shutdown();

  // Snatch: O(1) allocation from pretrashing EmptyBox cache
  RJ_API static void* snatch_emptybox(size_t size_bytes);

  // Deposit: Return a dead block into the EmptyBox cache for future snatches
  RJ_API static bool deposit_emptybox(void* addr, size_t size_bytes);

  // Rebate: Return a physical block according to target and pacing mode
  RJ_API static void rebate_block(void* addr, size_t size_bytes);

  // Flush cached blocks to OS or internal heap pools
  RJ_API static size_t flush_buffers(bool force_os_decommit = false);

  // Configuration accessors
  static void set_target(RedjetRebateTarget target) noexcept {
    _target.store(target, std::memory_order_release);
  }
  static RedjetRebateTarget target() noexcept {
    return _target.load(std::memory_order_relaxed);
  }

  static void set_mode(RedjetSnatchAndRebateMode mode) noexcept {
    _mode.store(mode, std::memory_order_release);
  }
  static RedjetSnatchAndRebateMode mode() noexcept {
    return _mode.load(std::memory_order_relaxed);
  }

  static const char* target_name(RedjetRebateTarget target) noexcept;
  static const char* mode_name(RedjetSnatchAndRebateMode mode) noexcept;

  static const RedjetSnatchRebateMetrics& metrics() noexcept { return _metrics; }

  // HotLog / JMX diagnostic printer
  RJ_API static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETSNATCHREBATE_HPP
