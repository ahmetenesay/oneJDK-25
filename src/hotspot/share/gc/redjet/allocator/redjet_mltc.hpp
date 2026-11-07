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

#ifndef SHARE_GC_REDJET_REDJETMLTC_HPP
#define SHARE_GC_REDJET_REDJETMLTC_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifndef HS_RMRP_METHOD_E_DEFINED
#define HS_RMRP_METHOD_E_DEFINED
enum class hs_rmrp_method_e : uint8_t {
    PLUMP      = 0,
    SEQUENTIAL = 1
};
#endif

/* ========================================================================= */
/* 1. MLTC (MEMORY LEAK & TRAFFIC CONGESTION) SOLVER                         */
/* ========================================================================= */

struct hs_mltc_engine_t {
    float               velocity_mbps{ 0.0f };
    size_t              misaligned_count{ 0 };
    size_t              total_inspected{ 0 };
    std::atomic<bool>   continuous_upward_trend{ false };
    std::atomic<size_t> emergency_compactions{ 0 };
    std::atomic<size_t> throttled_allocations{ 0 };
    std::atomic<size_t> realignments_forced{ 0 };

    bool check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps);
    void reset();
};

/* ========================================================================= */
/* 2. RMRP (REDUCE MEMORY RESIDENT SET RECLAMATION)                          */
/* ========================================================================= */

class RedjetRMRP;

extern hs_mltc_engine_t g_hs_mltc_solver;
using RedjetMLTC = hs_mltc_engine_t;

#endif // SHARE_GC_REDJET_REDJETMLTC_HPP
