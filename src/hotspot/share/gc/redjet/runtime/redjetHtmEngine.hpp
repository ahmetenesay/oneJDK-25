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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETHTMENGINE_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETHTMENGINE_HPP

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
// Redjet Hardware Transactional Memory (HTM) Engine (PLAN.md Section 8.7, 8.8)
// - Intel TSX / RTM (_xbegin, _xend, _xabort) & AMD ASF support
// - Lock-free multi-field atomic pointer updates with zero lock contention
// - Dynamic fallback to ticket locks on persistent conflict or capacity abort
// ============================================================================

enum class RedjetHtmStatus : uint32_t {
  HTM_STARTED          = 0xFFFFFFFF,
  HTM_ABORT_EXPLICIT   = 0x01,
  HTM_ABORT_RETRY      = 0x02,
  HTM_ABORT_CONFLICT   = 0x04,
  HTM_ABORT_CAPACITY   = 0x08,
  HTM_ABORT_DEBUG      = 0x10,
  HTM_ABORT_NESTED     = 0x20
};

class RedjetHtmEngine {
public:
  static constexpr uint32_t MAX_HTM_RETRIES = 3;

private:
  static RedjetHtmEngine*   _instance;
  static std::atomic<bool>  _initialized;

  bool                      _tsx_supported{ false };
  hs_ticket_lock_t          _fallback_lock;

  // Telemetry
  std::atomic<uint64_t>     _total_transactions_attempted{ 0 };
  std::atomic<uint64_t>     _total_transactions_committed{ 0 };
  std::atomic<uint64_t>     _total_conflict_aborts{ 0 };
  std::atomic<uint64_t>     _total_capacity_aborts{ 0 };
  std::atomic<uint64_t>     _total_fallback_lock_acquisitions{ 0 };

  void detect_tsx_hardware();

public:
  RedjetHtmEngine();
  ~RedjetHtmEngine();

  static void initialize();
  static void destroy();
  static RedjetHtmEngine* get() noexcept { return _instance; }

  // 1. Transaction Boundaries
  RJ_API uint32_t begin_transaction() noexcept;
  RJ_API void     end_transaction() noexcept;
  RJ_API void     abort_transaction(uint8_t code = 0) noexcept;

  // 2. Transactional Scope Helper (attempts HTM retries before fallback lock)
  template<typename Func>
  auto run_transactional(Func&& fn) -> decltype(fn()) {
    if (!_tsx_supported) {
      hs_ticket_lock_acquire(&_fallback_lock);
      _total_fallback_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
      auto res = fn();
      hs_ticket_lock_release(&_fallback_lock);
      return res;
    }

    _total_transactions_attempted.fetch_add(1, std::memory_order_relaxed);

    for (uint32_t attempt = 0; attempt < MAX_HTM_RETRIES; ++attempt) {
      uint32_t status = begin_transaction();
      if (status == static_cast<uint32_t>(RedjetHtmStatus::HTM_STARTED)) {
        auto res = fn();
        end_transaction();
        _total_transactions_committed.fetch_add(1, std::memory_order_relaxed);
        return res;
      }

      // Check abort cause
      if (status & static_cast<uint32_t>(RedjetHtmStatus::HTM_ABORT_CONFLICT)) {
        _total_conflict_aborts.fetch_add(1, std::memory_order_relaxed);
      } else if (status & static_cast<uint32_t>(RedjetHtmStatus::HTM_ABORT_CAPACITY)) {
        _total_capacity_aborts.fetch_add(1, std::memory_order_relaxed);
        break; // Capacity aborts rarely succeed on immediate retry
      }

      hs_pause();
    }

    // Fallback to ticket lock
    hs_ticket_lock_acquire(&_fallback_lock);
    _total_fallback_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
    auto res = fn();
    hs_ticket_lock_release(&_fallback_lock);
    return res;
  }

  // Status
  bool tsx_supported() const noexcept { return _tsx_supported; }

  // Telemetry
  uint64_t transactions_attempted() const noexcept { return _total_transactions_attempted.load(std::memory_order_relaxed); }
  uint64_t transactions_committed() const noexcept { return _total_transactions_committed.load(std::memory_order_relaxed); }
  uint64_t conflict_aborts()        const noexcept { return _total_conflict_aborts.load(std::memory_order_relaxed); }
  uint64_t capacity_aborts()        const noexcept { return _total_capacity_aborts.load(std::memory_order_relaxed); }
  uint64_t fallback_locks()         const noexcept { return _total_fallback_lock_acquisitions.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETHTMENGINE_HPP
