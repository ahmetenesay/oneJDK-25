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

#include "gc/redjet/runtime/redjetSSIOLSCHeuristics.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "logging/log.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#define log_warning(...) if (false) printf
#endif

RedjetSSIOLSCHeuristics::RedjetSSIOLSCHeuristics(HS_hotspot_scgc_type_t scgc)
  : _scgc_type(scgc),
    _allocated_bytes(0),
    _cycle_count(0),
    _fixed_tlab_size(DEFAULT_FIXED_TLAB_SIZE),
    _monotonic_budget_bytes(64 * 1024 * 1024),
    _high_watermark_bytes(0),
    _max_observed_wcet_cycles(0),
    _wcet_violations(0),
    _invariant_checks(0),
    _budget_exceeded_events(0),
    _gc_start_tsc(0) {
}

void RedjetSSIOLSCHeuristics::set_monotonic_budget(size_t budget_bytes) noexcept {
  _monotonic_budget_bytes = std::max<size_t>(1024 * 1024, budget_bytes);
}

bool RedjetSSIOLSCHeuristics::verify_monotonic_invariants() const noexcept {
  const_cast<RedjetSSIOLSCHeuristics*>(this)->_invariant_checks.fetch_add(1, std::memory_order_relaxed);

  // Invariant 1: Dynamic expansion must never be permitted in SSIOLSC mode
  if (is_dynamic_growth_allowed()) {
    return false;
  }

  // Invariant 2: Current allocation must never exceed the static monotonic budget
  size_t current_allocated = _allocated_bytes.load(std::memory_order_relaxed);
  if (current_allocated > _monotonic_budget_bytes) {
    const_cast<RedjetSSIOLSCHeuristics*>(this)->_budget_exceeded_events.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  return true;
}

bool RedjetSSIOLSCHeuristics::should_start_gc() const {
  // Deterministic trigger: exactly when allocated memory reaches 75% of static monotonic budget
  size_t threshold = (_monotonic_budget_bytes * 3) / 4;
  size_t current = _allocated_bytes.load(std::memory_order_relaxed);

  if (current >= threshold) {
    return true;
  }

  // PrimitiveRW Mode: lockstep buffer triggers at 50%
  if (is_primitive_rw()) {
    size_t rw_threshold = _monotonic_budget_bytes / 2;
    if (current >= rw_threshold) {
      return true;
    }
  }

  return false;
}

void RedjetSSIOLSCHeuristics::record_allocation(size_t bytes) {
  size_t total = _allocated_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;

  if (total > _high_watermark_bytes) {
    _high_watermark_bytes = total;
  }

  verify_monotonic_invariants();
}

void RedjetSSIOLSCHeuristics::record_gc_start() {
  _gc_start_tsc = sacculus_rdtsc();
}

void RedjetSSIOLSCHeuristics::record_gc_end() {
  uint64_t end_tsc = sacculus_rdtsc();
  uint64_t duration = (end_tsc > _gc_start_tsc) ? (end_tsc - _gc_start_tsc) : 0;

  uint64_t prev_max = _max_observed_wcet_cycles.load(std::memory_order_relaxed);
  while (duration > prev_max) {
    if (_max_observed_wcet_cycles.compare_exchange_weak(prev_max, duration, std::memory_order_relaxed)) {
      break;
    }
  }

  if (duration > MAX_ALLOWED_WCET_CYCLES) {
    _wcet_violations.fetch_add(1, std::memory_order_relaxed);
    log_warning(gc)("Redjet SSIOLSC WCET violation: duration=" UINT64_FORMAT " cycles (max allowed=" UINT64_FORMAT ")",
                    duration, MAX_ALLOWED_WCET_CYCLES);
  }

  _cycle_count.fetch_add(1, std::memory_order_relaxed);
  _allocated_bytes.store(0, std::memory_order_relaxed);
}

void RedjetSSIOLSCHeuristics::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet SSIOLSC Safety-Critical Deterministic Heuristics ---");
  st->print_cr("  Mode:                    %s (SCGC Type: %d, PrimitiveRW: %s)",
               name(), static_cast<int>(_scgc_type), is_primitive_rw() ? "YES" : "NO");
  st->print_cr("  Dynamic Growth Allowed:  NO (Hard Zero-Jitter Guarantee)");
  st->print_cr("  Total GC Cycles:         " UINT64_FORMAT, _cycle_count.load(std::memory_order_relaxed));
  st->print_cr("  Fixed TLAB Size:         " SIZE_FORMAT " KB", _fixed_tlab_size / 1024);
  st->print_cr("  Monotonic Budget:        " SIZE_FORMAT " KB (High Watermark: " SIZE_FORMAT " KB)",
               _monotonic_budget_bytes / 1024, _high_watermark_bytes / 1024);
  st->print_cr("  WCET Worst Case:         " UINT64_FORMAT " cycles (Limit: " UINT64_FORMAT ", Violations: " UINT64_FORMAT ")",
               max_observed_wcet_cycles(), MAX_ALLOWED_WCET_CYCLES, wcet_violations());
  st->print_cr("  Invariant Checks:        " UINT64_FORMAT " (Budget Exceeded: " UINT64_FORMAT ")",
               _invariant_checks.load(std::memory_order_relaxed),
               _budget_exceeded_events.load(std::memory_order_relaxed));
}
