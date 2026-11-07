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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETCOREAFFINITIZER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETCOREAFFINITIZER_HPP

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
// Redjet Heterogeneous Core Affinitizer (PLAN.md Section 8.8)
// - Distinguishes High-IPC P-Cores (Performance) from High-Throughput E-Cores (Efficiency)
// - Pins latency-critical Mutator threads to P-Cores for 0.1ns response time
// - Directs background GC scrubbing & Falline draining to E-Cores
// - Eliminates cross-cluster IPC stalls on Intel Hybrid & AMD Zen4c
// ============================================================================

enum class RedjetCoreType : uint8_t {
  CORE_HOMOGENEOUS = 0,
  CORE_PERFORMANCE = 1, // P-Core (High IPC, max clock)
  CORE_EFFICIENCY  = 2  // E-Core (High throughput, lower TDP)
};

struct RedjetCpuClassification {
  uint32_t       logical_id{ 0 };
  RedjetCoreType core_type{ RedjetCoreType::CORE_PERFORMANCE };
  uint32_t       max_frequency_mhz{ 3000 };
  uint32_t       l2_cache_size_kb{ 1024 };
};

class RedjetCoreAffinitizer {
public:
  static constexpr size_t MAX_CPUS = 256;

private:
  static RedjetCoreAffinitizer* _instance;
  static std::atomic<bool>      _initialized;

  RedjetCpuClassification       _cpus[MAX_CPUS];
  size_t                        _total_cpus{ 0 };
  size_t                        _p_core_count{ 0 };
  size_t                        _e_core_count{ 0 };
  bool                          _is_hybrid_topology{ false };

  hs_ticket_lock_t              _affinitizer_lock;

  // Telemetry
  std::atomic<uint64_t>         _total_pcore_bindings{ 0 };
  std::atomic<uint64_t>         _total_ecore_bindings{ 0 };
  std::atomic<uint64_t>         _total_migrations_prevented{ 0 };

  void detect_core_types();

public:
  RedjetCoreAffinitizer();
  ~RedjetCoreAffinitizer();

  static void initialize();
  static void destroy();
  static RedjetCoreAffinitizer* get() noexcept { return _instance; }

  // 1. Thread Pinning Operations
  RJ_API bool bind_current_thread_to_pcore();
  RJ_API bool bind_current_thread_to_ecore();

  // 2. Query Core Classification
  RedjetCoreType get_core_type(uint32_t logical_cpu) const noexcept;
  bool is_hybrid() const noexcept { return _is_hybrid_topology; }

  // Counts
  size_t p_core_count() const noexcept { return _p_core_count; }
  size_t e_core_count() const noexcept { return _e_core_count; }

  // Telemetry
  uint64_t pcore_bindings()        const noexcept { return _total_pcore_bindings.load(std::memory_order_relaxed); }
  uint64_t ecore_bindings()        const noexcept { return _total_ecore_bindings.load(std::memory_order_relaxed); }
  uint64_t migrations_prevented()  const noexcept { return _total_migrations_prevented.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETCOREAFFINITIZER_HPP
