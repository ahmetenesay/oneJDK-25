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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJET_RMRP_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJET_RMRP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// RMRP (Reduce Memory Resident Set Reclamation) Subsystem (PLAN.md Section 11.1)
// Plump & Sequential Modes for Dynamic Resident Set Compression
// ============================================================================

class RedjetRMRPSubsystem {
private:
  static std::atomic<uint64_t> _total_plump_reclaimed_bytes;
  static std::atomic<uint64_t> _total_sequential_reclaimed_bytes;
  static std::atomic<uint32_t> _total_reclamation_cycles;

public:
  // Executes resident set reclamation under specified mode
  RJ_API static size_t execute_reclamation(hs_rmrp_method_e method, size_t target_bytes);

  // Plump: Bulk coalesce of idle slabs and physical memory decommit
  RJ_API static size_t execute_plump_reclamation(size_t target_bytes);

  // Sequential: Span trimming on cold, aged runway and landfill pages
  RJ_API static size_t execute_sequential_trimming(size_t target_bytes);

  // Statistics
  RJ_API static uint64_t total_plump_reclaimed() { return _total_plump_reclaimed_bytes.load(std::memory_order_relaxed); }
  RJ_API static uint64_t total_sequential_reclaimed() { return _total_sequential_reclaimed_bytes.load(std::memory_order_relaxed); }
  RJ_API static uint32_t total_cycles() { return _total_reclamation_cycles.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJET_RMRP_HPP
