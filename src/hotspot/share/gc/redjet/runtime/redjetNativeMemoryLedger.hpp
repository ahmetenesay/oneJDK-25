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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETNATIVEMEMORYLEDGER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETNATIVEMEMORYLEDGER_HPP

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
// Redjet Off-Heap Native Memory Ledger (PLAN.md Section 9.1)
// - Byte-accurate accounting for code_boot, log_boot, and reserved_boot
// - Tracks Reserved vs Committed virtual memory state & high-water peaks
// - Leak detection and runaway native memory watchdog
// ============================================================================

enum class RedjetNativeTag : uint8_t {
  TAG_CODE_BOOT     = 0,
  TAG_LOG_BOOT      = 1,
  TAG_RESERVED_BOOT = 2,
  TAG_METADATA      = 3,
  TAG_SCRATCH       = 4,
  TAG_COUNT         = 5
};

struct RedjetLedgerCategory {
  std::atomic<size_t> reserved_bytes{ 0 };
  std::atomic<size_t> committed_bytes{ 0 };
  std::atomic<size_t> peak_committed_bytes{ 0 };
  std::atomic<uint64_t> allocation_count{ 0 };
  std::atomic<uint64_t> deallocation_count{ 0 };
};

class RedjetNativeMemoryLedger {
private:
  static RedjetNativeMemoryLedger* _instance;
  static std::atomic<bool>         _initialized;

  RedjetLedgerCategory             _categories[static_cast<size_t>(RedjetNativeTag::TAG_COUNT)];
  hs_ticket_lock_t                 _ledger_lock;

  // Global telemetry
  std::atomic<size_t>              _total_reserved{ 0 };
  std::atomic<size_t>              _total_committed{ 0 };
  std::atomic<size_t>              _peak_committed{ 0 };

public:
  RedjetNativeMemoryLedger();
  ~RedjetNativeMemoryLedger();

  static void initialize();
  static void destroy();
  static RedjetNativeMemoryLedger* get() noexcept { return _instance; }

  // 1. Record Allocation / Reservation
  RJ_API void record_reserve(RedjetNativeTag tag, size_t bytes) noexcept;
  RJ_API void record_commit(RedjetNativeTag tag, size_t bytes) noexcept;

  // 2. Record Release / Decommit
  RJ_API void record_decommit(RedjetNativeTag tag, size_t bytes) noexcept;
  RJ_API void record_release(RedjetNativeTag tag, size_t bytes) noexcept;

  // Totals
  size_t total_reserved()  const noexcept { return _total_reserved.load(std::memory_order_relaxed); }
  size_t total_committed() const noexcept { return _total_committed.load(std::memory_order_relaxed); }
  size_t peak_committed()  const noexcept { return _peak_committed.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETNATIVEMEMORYLEDGER_HPP
