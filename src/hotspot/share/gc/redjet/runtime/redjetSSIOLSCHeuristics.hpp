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

#ifndef SHARE_GC_REDJET_SSIOLSC_REDJETSSIOLSCHEURISTICS_HPP
#define SHARE_GC_REDJET_SSIOLSC_REDJETSSIOLSCHEURISTICS_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include "utilities/macros.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "utilities/ostream.hpp"
#else
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

// ============================================================================
// RedjetSSIOLSCHeuristics (PLAN.md Section 7.5 & 8.10)
// Safety-Critical Static In-Order Lock-Step Deterministic Mode Heuristics:
// - Deterministic Worst-Case Execution Time (WCET) Budgeting (<1.0 ms)
// - Monotonic Memory Watermark Verification (Zero-Jitter Hard Guarantee)
// - Zero Dynamic Growth Guarantee (ForbidDynamicGrowth = true)
// - Strict Lock-Step Allocation Quota Enforcement
// ============================================================================
class RedjetSSIOLSCHeuristics {
public:
  static constexpr size_t DEFAULT_FIXED_TLAB_SIZE = 2 * 1024 * 1024; // Fixed 2 MB per thread
  static constexpr uint64_t MAX_ALLOWED_WCET_CYCLES = 3000000ULL;    // 1.0 ms at 3.0 GHz

private:
  HS_hotspot_scgc_type_t _scgc_type{ HS_SCGC_TYPE_DEFAULT };
  std::atomic<size_t>   _allocated_bytes{ 0 };
  std::atomic<uint64_t> _cycle_count{ 0 };
  size_t                _fixed_tlab_size{ DEFAULT_FIXED_TLAB_SIZE };
  size_t                _monotonic_budget_bytes{ 64 * 1024 * 1024 }; // Fixed 64 MB budget
  size_t                _high_watermark_bytes{ 0 };

  // WCET tracking
  std::atomic<uint64_t> _max_observed_wcet_cycles{ 0 };
  std::atomic<uint64_t> _wcet_violations{ 0 };
  std::atomic<uint64_t> _invariant_checks{ 0 };
  std::atomic<uint64_t> _budget_exceeded_events{ 0 };

  uint64_t              _gc_start_tsc{ 0 };

public:
  RedjetSSIOLSCHeuristics(HS_hotspot_scgc_type_t scgc = HS_SCGC_TYPE_DEFAULT);
  virtual ~RedjetSSIOLSCHeuristics() = default;

  virtual const char* name() const { return "SSIOLSC"; }
  virtual bool should_start_gc() const;
  virtual bool is_dynamic_growth_allowed() const { return false; } // Zero-jitter hard guarantee
  virtual size_t max_tlab_size() const { return _fixed_tlab_size; }
  virtual bool is_primitive_rw() const { return _scgc_type == HS_SCGC_TYPE_PRIMITIVE_RW; }

  // Lifecycle
  virtual void record_allocation(size_t bytes);
  virtual void record_gc_start();
  virtual void record_gc_end();

  // Verification & Bounds
  bool verify_monotonic_invariants() const noexcept;
  void set_monotonic_budget(size_t budget_bytes) noexcept;
  size_t monotonic_budget_bytes() const noexcept { return _monotonic_budget_bytes; }
  size_t high_watermark_bytes() const noexcept { return _high_watermark_bytes; }
  uint64_t max_observed_wcet_cycles() const noexcept { return _max_observed_wcet_cycles.load(std::memory_order_relaxed); }
  uint64_t wcet_violations() const noexcept { return _wcet_violations.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_SSIOLSC_REDJETSSIOLSCHEURISTICS_HPP
