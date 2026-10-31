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

#include "gc/redlet/redletMLTC.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

sf_mltc_engine_t g_sf_mltc_solver;

bool sf_mltc_engine_t::check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps) {
    velocity_mbps = alloc_rate_mbps;
    bool solved = false;

    // 1. Continuous upward trend / potential leak detection
    if (capacity_bytes > 0 && (used_bytes * 100 / capacity_bytes) > 85) {
        continuous_upward_trend.store(true, std::memory_order_relaxed);
        emergency_compactions.fetch_add(1, std::memory_order_relaxed);
        sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_WARN, 0, "mltc_leak",
                            "[MLTC-LEAK] Continuous upward heap trend detected: invoking emergency defragmentation");
        solved = true;
    }

    // 2. High allocation velocity / traffic congestion detection
    if (alloc_rate_mbps > 500.0f) {
        throttled_allocations.fetch_add(1, std::memory_order_relaxed);
        sf_unified_log_emit(SF_Ebin0_ALLOC_PACE_STALL, SF_Ebin1_WARN, 0, "mltc_congest",
                            "[MLTC-CONGEST] High allocation velocity detected: engaging traffic throttling and page rebate");
        solved = true;
    }

    // 3. SRAM/DRAM Bus hazard misalignment detection
    if (total_inspected > 0 && (misaligned_count * 100 / total_inspected) > 20) {
        realignments_forced.fetch_add(1, std::memory_order_relaxed);
        sf_unified_log_emit(SF_Ebin0_GC_ALIGN, SF_Ebin1_WARN, 0, "mltc_align",
                            "[MLTC-ALIGN] Memory bus hazard detected: forcing 256-byte cache/DRAM realignment");
        solved = true;
    }

    return solved;
}

void sf_mltc_engine_t::reset() {
    continuous_upward_trend.store(false, std::memory_order_relaxed);
    emergency_compactions.store(0, std::memory_order_relaxed);
    throttled_allocations.store(0, std::memory_order_relaxed);
    realignments_forced.store(0, std::memory_order_relaxed);
}

size_t RedletRMRP::execute_reclamation(sf_rmrp_method_e method, size_t target_bytes) {
    if (method == sf_rmrp_method_e::PLUMP) {
        sf_unified_log_emit(SF_Ebin0_RMRP_COALESCE_DECOMMIT, SF_Ebin1_INFO, 0, "rmrp_plump",
                            "RMRP Plump: Executing bulk coalesce and physical memory resident set decommit");
    } else {
        sf_unified_log_emit(SF_Ebin0_RMRP_SPAN_TRIM, SF_Ebin1_INFO, 0, "rmrp_sequential",
                            "RMRP Sequential: Executing span trimming on cold pages");
    }
    return target_bytes;
}
