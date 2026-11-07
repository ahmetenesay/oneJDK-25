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

#ifndef SHARE_GC_REDJET_ENGINE_REDJETTRAMPOLINETABLE_HPP
#define SHARE_GC_REDJET_ENGINE_REDJETTRAMPOLINETABLE_HPP

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
// Redjet Slot 16 Dynamic Trampoline Routing Table (PLAN.md Section 3.2, 6.2)
// - Manages 16-byte aligned atomic jump trampolines for dynamic JIT/AOT hot-swapping
// - Implements Slot 16 atomic destination repointing via atomic 64-bit / 128-bit CAS
// - Zero-STW live code transitions between Level 1 JIT code and Level 2 Superloop code
// - Cross-core instruction cache line invalidation and multi-CPU serialization
// ============================================================================

enum class RedjetTrampolineTier : uint8_t {
  TIER_INTERPRETER_FALLBACK = 0,
  TIER_LEVEL1_FAST_JIT      = 1,
  TIER_LEVEL2_SUPERLOOP_MIRC= 2,
  TIER_PUZZLER_HOTSWAP_WIN  = 3
};

struct alignas(16) RedjetTrampolineSlot {
  uint8_t               code_bytes[16];   // 16-byte aligned executable jump pad
  void*                 target_address;   // Active target entrypoint
  uint32_t              method_id;
  RedjetTrampolineTier  active_tier;
  bool                  is_occupied;
  uint64_t              installed_tsc;
  uint64_t              swaps_count;
};

class RedjetTrampolineTable {
public:
  static constexpr size_t MAX_TRAMPOLINES      = 8192;
  static constexpr size_t TRAMPOLINE_ALIGNMENT = 16;

private:
  static RedjetTrampolineTable* _instance;
  static std::atomic<bool>      _initialized;

  RedjetTrampolineSlot          _slots[MAX_TRAMPOLINES];
  size_t                        _allocated_slots{ 0 };
  hs_ticket_lock_t              _table_lock;

  // Telemetry
  std::atomic<uint64_t>         _total_trampolines_installed{ 0 };
  std::atomic<uint64_t>         _total_hot_swaps_performed{ 0 };
  std::atomic<uint64_t>         _total_icache_flushes{ 0 };

  static void emit_trampoline_code(uint8_t* code_buf, void* target_address) noexcept;
  static void flush_instruction_cache(void* addr, size_t size) noexcept;

public:
  RedjetTrampolineTable();
  ~RedjetTrampolineTable();

  static void initialize();
  static void destroy();
  static RedjetTrampolineTable* get() noexcept { return _instance; }

  // 1. Allocate & Install Slot 16 Trampoline
  RJ_API void* install_trampoline(uint32_t method_id,
                                  void* initial_target,
                                  RedjetTrampolineTier tier);

  // 2. Atomic Hot-Swap (Zero-STW repointing of Slot 16 destination)
  RJ_API bool hot_swap_target(uint32_t method_id,
                              void* new_target,
                              RedjetTrampolineTier new_tier);

  // 3. Query Active Target
  RJ_API void* get_active_target(uint32_t method_id) const noexcept;

  // 4. Release Trampoline Slot
  RJ_API bool release_trampoline(uint32_t method_id);

  // Telemetry queries
  uint64_t trampolines_installed() const noexcept { return _total_trampolines_installed.load(std::memory_order_relaxed); }
  uint64_t hot_swaps_performed()   const noexcept { return _total_hot_swaps_performed.load(std::memory_order_relaxed); }
  uint64_t icache_flushes()        const noexcept { return _total_icache_flushes.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ENGINE_REDJETTRAMPOLINETABLE_HPP
