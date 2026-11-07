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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETUPCLOCKER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETUPCLOCKER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetOSAffinity.hpp"

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
// Upclocker Turbo Boost Engine & Group Hex Identifier (PLAN.md Section 8.8.2)
// - Unique 4-byte Group Hex Identifiers (0x1000..0xFFFF) for zero-cost logging
// - Silicon Affinity: Pins high-priority / warmup threads to P-Cores (E_CORE_MASK bypass)
// - Dynamic CPU frequency upclocking & warmup turbo burst pinning
// - Puzzler tournament license mapping and telemetry
// ============================================================================

static constexpr uint32_t REDJET_GROUP_HEX_BASE = 0x1000;
static constexpr uint32_t REDJET_GROUP_HEX_MAX  = 0xFFFF;
static constexpr size_t   REDJET_MAX_ACTIVE_GROUPS = 4096;

enum class RedjetGroupPriority : uint8_t {
  PRIORITY_IDLE       = 0, // Background scavenger, E-core eligible
  PRIORITY_NORMAL     = 4, // Standard mutator allocation
  PRIORITY_HIGH       = 8, // Triggers P-Core pin & Turbo Boost
  PRIORITY_CRITICAL   = 10 // P-Core lock, max turbo frequency, OS scheduler bypass
};

struct alignas(64) RedjetGroupDescriptor {
  uint32_t            group_hex_id{ 0 };
  uint32_t            owner_thread_id{ 0 };
  RedjetGroupPriority priority{ RedjetGroupPriority::PRIORITY_NORMAL };
  uint64_t            creation_tsc{ 0 };
  uint64_t            last_active_tsc{ 0 };
  std::atomic<bool>   is_turbo_pinned{ false };
  std::atomic<bool>   is_active{ false };
  char                group_name[32]{ 0 };

  void reset() noexcept {
    group_hex_id = 0;
    owner_thread_id = 0;
    priority = RedjetGroupPriority::PRIORITY_NORMAL;
    creation_tsc = 0;
    last_active_tsc = 0;
    is_turbo_pinned.store(false, std::memory_order_relaxed);
    is_active.store(false, std::memory_order_relaxed);
    group_name[0] = '\0';
  }
};

class RedjetUpclocker : public CHeapObj<mtGC> {
public:
  static constexpr uint32_t DEFAULT_WARMUP_BURST_MS = 50;

private:
  std::atomic<uint32_t>  _group_counter{ REDJET_GROUP_HEX_BASE };
  RedjetGroupDescriptor  _group_registry[REDJET_MAX_ACTIVE_GROUPS];
  hs_ticket_lock_t       _upclocker_lock;

  // Telemetry counters
  std::atomic<uint64_t>  _total_groups_created{ 0 };
  std::atomic<uint64_t>  _total_pcore_pins{ 0 };
  std::atomic<uint64_t>  _total_turbo_boosts_applied{ 0 };
  std::atomic<uint64_t>  _total_warmup_bursts{ 0 };
  std::atomic<uint64_t>  _total_hex_hotlogs_emitted{ 0 };

  // Frequency and Turbo control internals
  bool apply_os_turbo_pin(uint32_t thread_id, uint32_t burst_ms);
  void release_os_turbo_pin(uint32_t thread_id);

public:
  RedjetUpclocker();
  ~RedjetUpclocker();

  static RedjetUpclocker& instance();

  void init();
  void destroy();

  // =========================================================================
  // 1. Group Hex ID Allocation & Management (0x1000..0xFFFF)
  // =========================================================================
  RJ_API uint32_t allocate_group_hex_id(const char* name,
                                        uint32_t owner_thread_id,
                                        RedjetGroupPriority priority = RedjetGroupPriority::PRIORITY_NORMAL);

  RJ_API void release_group_hex_id(uint32_t group_hex_id);

  RJ_API bool is_group_active(uint32_t group_hex_id) const noexcept;

  // =========================================================================
  // 2. Zero-Cost Hex HotLog / ColdLog Emission (0.1 ns)
  // Writes only the 4-byte 0x104A identifier instead of strings
  // =========================================================================
  RJ_API void emit_hex_hotlog(uint32_t group_hex_id, uint32_t event_code, uint64_t payload);

  // =========================================================================
  // 3. Silicon Affinity & Upclocker Turbo Pinning
  // Pins thread to P-Cores when priority >= 8 or warmup bypass is requested
  // =========================================================================
  RJ_API bool pin_pcore_and_upclock(uint32_t group_hex_id, uint32_t burst_ms = DEFAULT_WARMUP_BURST_MS);
  RJ_API void unpin_pcore_restore_normal(uint32_t group_hex_id);

  // Warmup burst frequency pinning
  RJ_API bool pin_max_frequency_warmup(uint32_t thread_id, uint32_t burst_ms = DEFAULT_WARMUP_BURST_MS);

  // Telemetry accessors
  uint64_t total_groups_created() const noexcept { return _total_groups_created.load(std::memory_order_relaxed); }
  uint64_t total_pcore_pins() const noexcept { return _total_pcore_pins.load(std::memory_order_relaxed); }
  uint64_t total_turbo_boosts() const noexcept { return _total_turbo_boosts_applied.load(std::memory_order_relaxed); }
  uint64_t total_warmup_bursts() const noexcept { return _total_warmup_bursts.load(std::memory_order_relaxed); }
  uint64_t total_hex_hotlogs() const noexcept { return _total_hex_hotlogs_emitted.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

extern RedjetUpclocker g_redjet_upclocker;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void hs_upclocker_set_boost(int enable);
RJ_API int  hs_upclocker_is_boost_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_RUNTIME_REDJETUPCLOCKER_HPP
