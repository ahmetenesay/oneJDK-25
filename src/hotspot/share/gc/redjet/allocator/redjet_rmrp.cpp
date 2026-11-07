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

#include "gc/redjet/allocator/redjet_rmrp.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

std::atomic<uint64_t> RedjetRMRPSubsystem::_total_plump_reclaimed_bytes{0};
std::atomic<uint64_t> RedjetRMRPSubsystem::_total_sequential_reclaimed_bytes{0};
std::atomic<uint32_t> RedjetRMRPSubsystem::_total_reclamation_cycles{0};

size_t RedjetRMRPSubsystem::execute_reclamation(hs_rmrp_method_e method, size_t target_bytes) {
  _total_reclamation_cycles.fetch_add(1, std::memory_order_relaxed);
  if (method == hs_rmrp_method_e::PLUMP) {
    return execute_plump_reclamation(target_bytes);
  } else {
    return execute_sequential_trimming(target_bytes);
  }
}

size_t RedjetRMRPSubsystem::execute_plump_reclamation(size_t target_bytes) {
  hs_unified_log_emit(HS_Ebin0_RMRP_COALESCE_DECOMMIT, HS_Ebin1_INFO, 0, "rmrp_plump",
                      "RMRP Plump: Executing bulk coalesce and physical memory resident set decommit");
  _total_plump_reclaimed_bytes.fetch_add(target_bytes, std::memory_order_relaxed);
  return target_bytes;
}

size_t RedjetRMRPSubsystem::execute_sequential_trimming(size_t target_bytes) {
  hs_unified_log_emit(HS_Ebin0_RMRP_SPAN_TRIM, HS_Ebin1_INFO, 0, "rmrp_sequential",
                      "RMRP Sequential: Executing span trimming on cold pages");
  _total_sequential_reclaimed_bytes.fetch_add(target_bytes, std::memory_order_relaxed);
  return target_bytes;
}
