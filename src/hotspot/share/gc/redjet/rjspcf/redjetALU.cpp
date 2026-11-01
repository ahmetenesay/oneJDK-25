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

#include "gc/redjet/rjspcf/redjetALU.hpp"
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

/* Global ALU Dispatch Nexus Instance */
hs_alu_dispatch_nexus_t g_hs_alu_nexus;

/* ========================================================================= */
/* 1. ALU PIPELINE NEXUS INITIALIZATION (16 Virtual Execution Lanes)         */
/* ========================================================================= */

hs_alu_dispatch_nexus_t::hs_alu_dispatch_nexus_t() {
    // 0..5: Native Hardware Execution Lines
    pipelines[0].init(64, 1, 0, 0, "Lane 0: Native FP64 (Double/FMA)");
    pipelines[1].init(32, 1, 0, 0, "Lane 1: Native FP32 (Float)");
    pipelines[2].init(16, 1, 0, 0, "Lane 2: Native FP16 (Half/BF16)");
    pipelines[3].init(64, 0, 0, 0, "Lane 3: Native INT64 (Long)");
    pipelines[4].init(32, 0, 0, 1, "Lane 4: Native INT32 (Int)");        // E-Core affine
    pipelines[5].init(16, 0, 0, 1, "Lane 5: Native INT16/INT8 (Byte)"); // E-Core affine

    // 6..15: Multi-Limb Emulation Execution Lines
    pipelines[6].init(32, 1, 1, 0, "Lane 6: Emu Float 2x32 (64b)");
    pipelines[7].init(16, 1, 1, 0, "Lane 7: Emu Float 4x16 (64b)");
    pipelines[8].init(16, 1, 1, 0, "Lane 8: Emu Float 2x16 (32b)");
    pipelines[9].init(8,  1, 1, 1, "Lane 9: Emu Float 4x8 (32b)");      // E-Core affine
    pipelines[10].init(64, 1, 1, 0, "Lane 10: Emu Float 2x64 (128b Double-Double)");
    pipelines[11].init(64, 1, 1, 0, "Lane 11: Emu Float 4x64 (256b Quad-Double)");
    pipelines[12].init(32, 0, 1, 0, "Lane 12: Emu Int 2x32 (64b)");
    pipelines[13].init(16, 0, 1, 1, "Lane 13: Emu Int 4x16 (64b)");     // E-Core affine
    pipelines[14].init(16, 0, 1, 1, "Lane 14: Emu Int 2x16 (32b)");     // E-Core affine
    pipelines[15].init(64, 0, 1, 0, "Lane 15: Emu Int 2x64 (128b Int128/BigInteger)");
}

/* ========================================================================= */
/* 2. DISPATCH & WORK-STEALING ROUTING                                       */
/* ========================================================================= */

uint32_t hs_alu_dispatch_nexus_t::dispatch_op(uint32_t preferred_alu, uint8_t priority) {
    preferred_alu %= NUM_ALU_LANES;

    // High priority operations pin execution to preferred ALU and override load balancing
    if (priority >= 8) {
        pipelines[preferred_alu].queue_depth.fetch_add(1, std::memory_order_relaxed);
        total_dispatched.fetch_add(1, std::memory_order_relaxed);
        return preferred_alu;
    }

    bool allow = allow_operation_dispatching.load(std::memory_order_acquire);
    uint32_t pref_depth = pipelines[preferred_alu].queue_depth.load(std::memory_order_relaxed);

    if (allow && pref_depth >= SATURATION_THRESHOLD) {
        total_saturated_events.fetch_add(1, std::memory_order_relaxed);
        // Preferred ALU queue is saturated: find an idle or least-loaded ALU queue
        uint32_t best_alu = preferred_alu;
        uint32_t min_depth = pref_depth;
        for (uint32_t i = 0; i < NUM_ALU_LANES; ++i) {
            uint32_t d = pipelines[i].queue_depth.load(std::memory_order_relaxed);
            if (d < min_depth) {
                min_depth = d;
                best_alu = i;
            }
        }
        if (best_alu != preferred_alu) {
            total_dispatched.fetch_add(1, std::memory_order_relaxed);
            if (best_alu >= 6 && best_alu <= 15) {
                total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
            }
            pipelines[best_alu].queue_depth.fetch_add(1, std::memory_order_relaxed);
            hs_hotlog_emit(HS_Ebin0_ALU_SATURATE_STEAL, HS_Ebin1_WARN, 0,
                           static_cast<uint16_t>(best_alu), 0, 0,
                           static_cast<uint16_t>(preferred_alu), 0, pref_depth, "alu_lane_stolen");
            return best_alu;
        }
    }

    pipelines[preferred_alu].queue_depth.fetch_add(1, std::memory_order_relaxed);
    total_dispatched.fetch_add(1, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_ALU_DISPATCH_DIRECT, HS_Ebin1_TRACE, 0,
                   static_cast<uint16_t>(preferred_alu), 0, 0, 0, 0, pref_depth, "alu_direct_dispatch");
    return preferred_alu;
}

int hs_alu_dispatch_nexus_t::resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id) {
    uint32_t native_lane = 1;
    switch (dtype) {
    case HS_DTYPE_F64:  native_lane = 0; break;
    case HS_DTYPE_F32:  native_lane = 1; break;
    case HS_DTYPE_F16:  native_lane = 2; break;
    case HS_DTYPE_I64:
    case HS_DTYPE_U64:  native_lane = 3; break;
    case HS_DTYPE_I32:
    case HS_DTYPE_U32:  native_lane = 4; break;
    case HS_DTYPE_I16:
    case HS_DTYPE_U16:
    case HS_DTYPE_I8:
    case HS_DTYPE_U8:
    case HS_DTYPE_F8:   native_lane = 5; break;
    case HS_DTYPE_F128: native_lane = 10; break; // Schema_Float_2x64 Double-Double
    case HS_DTYPE_I128:
    case HS_DTYPE_U128: native_lane = 15; break; // Schema_Int_2x64 Int128
    case HS_DTYPE_F256: native_lane = 11; break; // Schema_Float_4x64 Quad-Double
    case HS_DTYPE_I256:
    case HS_DTYPE_U256: native_lane = 15; break; // Schema_Int_4x64 Int256
    default:            native_lane = 1; break;
    }

    bool is_inherently_emulated = (native_lane >= 6 && native_lane <= 15);
    uint32_t default_schema = HS_EMU_SCHEMA_NONE;
    if (dtype == HS_DTYPE_F128) default_schema = HS_EMU_SCHEMA_FLOAT_2X64;
    else if (dtype == HS_DTYPE_I128 || dtype == HS_DTYPE_U128) default_schema = HS_EMU_SCHEMA_INT_2X64;
    else if (dtype == HS_DTYPE_F256) default_schema = HS_EMU_SCHEMA_FLOAT_4X64;
    else if (dtype == HS_DTYPE_I256 || dtype == HS_DTYPE_U256) default_schema = HS_EMU_SCHEMA_INT_4X64;

    if (priority >= 8) {
        pipelines[native_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);
        total_dispatched.fetch_add(1, std::memory_order_relaxed);
        if (out_is_emulated) *out_is_emulated = is_inherently_emulated ? 1 : 0;
        if (out_schema_id)   *out_schema_id = static_cast<int>(default_schema);
        return static_cast<int>(native_lane);
    }

    bool allow = allow_operation_dispatching.load(std::memory_order_acquire);
    uint32_t native_depth = pipelines[native_lane].queue_depth.load(std::memory_order_relaxed);

    if (allow && native_depth >= SATURATION_THRESHOLD) {
        total_saturated_events.fetch_add(1, std::memory_order_relaxed);

        uint32_t best_lane = native_lane;
        uint32_t best_schema = default_schema;
        uint32_t min_depth = native_depth;

        auto test_candidate = [&](uint32_t cand_lane, uint32_t cand_schema) {
            uint32_t d = pipelines[cand_lane].queue_depth.load(std::memory_order_relaxed);
            if (d < min_depth) {
                min_depth = d;
                best_lane = cand_lane;
                best_schema = cand_schema;
            }
        };

        if (dtype == HS_DTYPE_F64) {
            test_candidate(7, HS_EMU_SCHEMA_FLOAT_4X16); // 4x16-bit float emulation on 16b ALU
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_2X32); // 2x32-bit float emulation on 32b ALU
            test_candidate(1, HS_EMU_SCHEMA_FLOAT_2X32); // native 32-bit ALU lane
            test_candidate(2, HS_EMU_SCHEMA_FLOAT_4X16); // native 16-bit ALU lane
        } else if (dtype == HS_DTYPE_F32) {
            test_candidate(8, HS_EMU_SCHEMA_FLOAT_2X16); // 2x16-bit float emulation
            test_candidate(9, HS_EMU_SCHEMA_FLOAT_4X8);  // 4x8-bit float emulation
            test_candidate(2, HS_EMU_SCHEMA_FLOAT_2X16); // native 16-bit ALU lane
        } else if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) {
            test_candidate(13, HS_EMU_SCHEMA_INT_4X16);  // 4x16-bit integer emulation
            test_candidate(12, HS_EMU_SCHEMA_INT_2X32);  // 2x32-bit integer emulation
            test_candidate(4, HS_EMU_SCHEMA_INT_2X32);   // native 32-bit ALU lane
        } else if (dtype == HS_DTYPE_I32 || dtype == HS_DTYPE_U32) {
            test_candidate(14, HS_EMU_SCHEMA_INT_2X16);  // 2x16-bit integer emulation
            test_candidate(5, HS_EMU_SCHEMA_INT_2X16);   // native 16-bit ALU lane
        } else if (dtype == HS_DTYPE_F128) {
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_4X32);
            test_candidate(10, HS_EMU_SCHEMA_FLOAT_2X64);
        } else if (dtype == HS_DTYPE_F256) {
            test_candidate(11, HS_EMU_SCHEMA_FLOAT_4X64);
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_8X32);
        } else if (dtype == HS_DTYPE_I128 || dtype == HS_DTYPE_U128) {
            test_candidate(12, HS_EMU_SCHEMA_INT_4X32);  // 4x32-bit integer emulation
        } else if (dtype == HS_DTYPE_I256 || dtype == HS_DTYPE_U256) {
            test_candidate(12, HS_EMU_SCHEMA_INT_8X32);  // 8x32-bit integer emulation
        }

        if (best_lane != native_lane) {
            total_dispatched.fetch_add(1, std::memory_order_relaxed);
            total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
            pipelines[best_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);

            hs_hotlog_emit(HS_Ebin0_ALU_SATURATE_STEAL, HS_Ebin1_WARN, 0,
                static_cast<uint16_t>(best_lane), static_cast<uint8_t>(dtype), static_cast<uint8_t>(best_schema),
                static_cast<uint16_t>(native_lane), 0, native_depth, "alu_emulation_stolen");

            if (out_is_emulated) *out_is_emulated = 1;
            if (out_schema_id)   *out_schema_id = static_cast<int>(best_schema);
            return static_cast<int>(best_lane);
        }
    }

    pipelines[native_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);
    total_dispatched.fetch_add(1, std::memory_order_relaxed);
    if (out_is_emulated) *out_is_emulated = is_inherently_emulated ? 1 : 0;
    if (out_schema_id)   *out_schema_id = static_cast<int>(default_schema);
    return static_cast<int>(native_lane);
}

int hs_alu_dispatch_nexus_t::resolve_route_precision(uint8_t dtype, uint8_t priority, float target_epsilon, int* out_is_emulated, int* out_schema_id) {
    bool is_fp = (dtype == HS_DTYPE_F64 || dtype == HS_DTYPE_F32 || dtype == HS_DTYPE_F16 ||
                  dtype == HS_DTYPE_F8  || dtype == HS_DTYPE_F128 || dtype == HS_DTYPE_F256);

    // EXACT_ALGEBRAIC: String, memory copy, hash, bitmask, GC bitmap scans.
    // Non-zero target epsilon is rejected (0-bit deviation tolerance).
    if (!is_fp) {
        if (target_epsilon > 0.0f) {
            if (out_is_emulated) *out_is_emulated = 0;
            if (out_schema_id)   *out_schema_id = static_cast<int>(HS_EMU_SCHEMA_NONE);
            return -1; // Rejected: target precision semantics disallowed on non-FP operations
        }
        // Clamped to 0.0 tolerance, route through lossless integer multi-limb schemas only
        return resolve_route(dtype, priority, out_is_emulated, out_schema_id);
    }

    // NUMERIC_IEEE_FP: Default to strict IEEE 754 machine epsilon when unspecified or non-positive
    float eff_epsilon = target_epsilon;
    if (eff_epsilon <= 0.0f) {
        if (dtype == HS_DTYPE_F64 || dtype == HS_DTYPE_F128 || dtype == HS_DTYPE_F256) {
            eff_epsilon = static_cast<float>(DBL_EPSILON);
        } else if (dtype == HS_DTYPE_F32) {
            eff_epsilon = FLT_EPSILON;
        } else if (dtype == HS_DTYPE_F16) {
            eff_epsilon = 9.77e-4f;
        } else {
            eff_epsilon = 0.0625f;
        }
    }

    // Target execution line determination with precision demand checks:
    uint32_t native_lane = 1;
    uint32_t default_schema = HS_EMU_SCHEMA_NONE;
    bool is_upgraded = false;

    if (dtype == HS_DTYPE_F32 && eff_epsilon < FLT_EPSILON) {
        native_lane = 0; // Upgrade to FP64 native lane
        default_schema = HS_EMU_SCHEMA_FLOAT_2X32;
        is_upgraded = true;
    } else if (dtype == HS_DTYPE_F64 && eff_epsilon < static_cast<float>(DBL_EPSILON)) {
        native_lane = 10; // Upgrade to Double-Double lane
        default_schema = HS_EMU_SCHEMA_FLOAT_2X64;
        is_upgraded = true;
    } else if (dtype == HS_DTYPE_F64) {
        native_lane = 0;
    } else if (dtype == HS_DTYPE_F32) {
        native_lane = 1;
    } else if (dtype == HS_DTYPE_F16) {
        native_lane = 2;
    } else if (dtype == HS_DTYPE_F8) {
        native_lane = 5;
    } else if (dtype == HS_DTYPE_F128) {
        native_lane = 10;
        default_schema = HS_EMU_SCHEMA_FLOAT_2X64;
    } else if (dtype == HS_DTYPE_F256) {
        native_lane = 11;
        default_schema = HS_EMU_SCHEMA_FLOAT_4X64;
    }

    bool is_inherently_emulated = (native_lane >= 6 && native_lane <= 15) || is_upgraded;

    if (priority >= 8) {
        pipelines[native_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);
        total_dispatched.fetch_add(1, std::memory_order_relaxed);
        if (is_inherently_emulated) {
            total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
        }
        if (out_is_emulated) *out_is_emulated = is_inherently_emulated ? 1 : 0;
        if (out_schema_id)   *out_schema_id = static_cast<int>(default_schema);
        return static_cast<int>(native_lane);
    }

    bool allow = allow_operation_dispatching.load(std::memory_order_acquire);
    uint32_t native_depth = pipelines[native_lane].queue_depth.load(std::memory_order_relaxed);

    if (allow && native_depth >= SATURATION_THRESHOLD) {
        total_saturated_events.fetch_add(1, std::memory_order_relaxed);

        uint32_t best_lane = native_lane;
        uint32_t best_schema = default_schema;
        uint32_t min_depth = native_depth;

        auto test_candidate = [&](uint32_t cand_lane, uint32_t cand_schema) {
            uint32_t d = pipelines[cand_lane].queue_depth.load(std::memory_order_relaxed);
            if (d < min_depth) {
                min_depth = d;
                best_lane = cand_lane;
                best_schema = cand_schema;
            }
        };

        if (native_lane == 0) { // F64 (or F32 upgraded to F64)
            if (eff_epsilon >= 1e-3f) {
                test_candidate(7, HS_EMU_SCHEMA_FLOAT_4X16);
                test_candidate(2, HS_EMU_SCHEMA_FLOAT_4X16);
            }
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_2X32);
            test_candidate(1, HS_EMU_SCHEMA_FLOAT_2X32);
            test_candidate(10, HS_EMU_SCHEMA_FLOAT_2X64);
        } else if (native_lane == 1) { // F32
            if (eff_epsilon >= 1e-2f) {
                test_candidate(9, HS_EMU_SCHEMA_FLOAT_4X8);
            }
            test_candidate(8, HS_EMU_SCHEMA_FLOAT_2X16);
            test_candidate(2, HS_EMU_SCHEMA_FLOAT_2X16);
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_2X32);
        } else if (dtype == HS_DTYPE_F128 || native_lane == 10) {
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_4X32);
            test_candidate(10, HS_EMU_SCHEMA_FLOAT_2X64);
        } else if (dtype == HS_DTYPE_F256 || native_lane == 11) {
            test_candidate(11, HS_EMU_SCHEMA_FLOAT_4X64);
            test_candidate(6, HS_EMU_SCHEMA_FLOAT_8X32);
        }

        if (best_lane != native_lane) {
            total_dispatched.fetch_add(1, std::memory_order_relaxed);
            total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
            pipelines[best_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);

            hs_hotlog_emit(HS_Ebin0_ALU_SATURATE_STEAL, HS_Ebin1_WARN, 0,
                static_cast<uint16_t>(best_lane), static_cast<uint8_t>(dtype), static_cast<uint8_t>(best_schema),
                static_cast<uint16_t>(native_lane), 0, native_depth, "alu_precision_route_stolen");

            if (out_is_emulated) *out_is_emulated = 1;
            if (out_schema_id)   *out_schema_id = static_cast<int>(best_schema);
            return static_cast<int>(best_lane);
        }
    }

    pipelines[native_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);
    total_dispatched.fetch_add(1, std::memory_order_relaxed);
    if (is_inherently_emulated) {
        total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
    }
    if (out_is_emulated) *out_is_emulated = is_inherently_emulated ? 1 : 0;
    if (out_schema_id)   *out_schema_id = static_cast<int>(default_schema);
    return static_cast<int>(native_lane);
}

void hs_alu_dispatch_nexus_t::complete_op(uint32_t alu_id) {
    alu_id %= NUM_ALU_LANES;
    uint32_t d = pipelines[alu_id].queue_depth.load(std::memory_order_relaxed);
    while (d > 0 && !pipelines[alu_id].queue_depth.compare_exchange_weak(d, d - 1, std::memory_order_relaxed)) {}
}

void hs_alu_dispatch_nexus_t::reset() {
    for (size_t i = 0; i < NUM_ALU_LANES; ++i) {
        pipelines[i].queue_depth.store(0, std::memory_order_relaxed);
    }
    total_dispatched.store(0, std::memory_order_relaxed);
    total_saturated_events.store(0, std::memory_order_relaxed);
    total_emulation_dispatched.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 3. HOTSPOT REDJETGC INTERNAL EXECUTION LINE ROUTING                       */
/* ========================================================================= */

uint32_t hs_alu_dispatch_nexus_t::select_card_table_lane(bool allow_stealing) {
    // Preferred lane: Lane 3 (Native INT64)
    constexpr uint32_t preferred = 3;
    bool allow = allow_stealing && allow_operation_dispatching.load(std::memory_order_acquire);
    uint32_t depth = pipelines[preferred].queue_depth.load(std::memory_order_relaxed);

    if (allow && depth >= SATURATION_THRESHOLD) {
        total_saturated_events.fetch_add(1, std::memory_order_relaxed);
        // Fallback candidates: Lane 12 (Emu Int 2x32) or Lane 5 (Native INT16_8)
        uint32_t d12 = pipelines[12].queue_depth.load(std::memory_order_relaxed);
        uint32_t d5  = pipelines[5].queue_depth.load(std::memory_order_relaxed);
        uint32_t chosen = (d12 <= d5) ? 12 : 5;
        uint32_t min_d  = std::min(d12, d5);

        if (min_d < depth) {
            pipelines[chosen].queue_depth.fetch_add(1, std::memory_order_relaxed);
            total_dispatched.fetch_add(1, std::memory_order_relaxed);
            if (chosen == 12) {
                total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
            }
            hs_hotlog_emit(HS_Ebin0_ALU_SATURATE_STEAL, HS_Ebin1_WARN, 0,
                           static_cast<uint16_t>(chosen), 0, 0,
                           static_cast<uint16_t>(preferred), 0, depth, "card_table_scan_lane_stolen");
            return chosen;
        }
    }

    pipelines[preferred].queue_depth.fetch_add(1, std::memory_order_relaxed);
    total_dispatched.fetch_add(1, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_ALU_DISPATCH_DIRECT, HS_Ebin1_TRACE, 0,
                   static_cast<uint16_t>(preferred), 0, 0, 0, 0, depth, "card_table_scan_direct");
    return preferred;
}

uint32_t hs_alu_dispatch_nexus_t::select_memzero_lane(bool pcore_busy, bool allow_stealing) {
    bool allow = allow_stealing && allow_operation_dispatching.load(std::memory_order_acquire);
    uint32_t d0 = pipelines[0].queue_depth.load(std::memory_order_relaxed);
    uint32_t d1 = pipelines[1].queue_depth.load(std::memory_order_relaxed);

    if (pcore_busy || (allow && (d0 >= SATURATION_THRESHOLD || d1 >= SATURATION_THRESHOLD))) {
        // Select idle or least loaded E-Core integer line: Lane 4 (INT32) or Lane 5 (INT16_8)
        uint32_t d4 = pipelines[4].queue_depth.load(std::memory_order_relaxed);
        uint32_t d5 = pipelines[5].queue_depth.load(std::memory_order_relaxed);
        uint32_t chosen = (d5 < d4) ? 5 : 4;

        pipelines[chosen].queue_depth.fetch_add(1, std::memory_order_relaxed);
        total_dispatched.fetch_add(1, std::memory_order_relaxed);
        hs_hotlog_emit(HS_Ebin0_ALU_DISPATCH_DIRECT, HS_Ebin1_TRACE, 0,
                       static_cast<uint16_t>(chosen), 0, 0, 0, 0, std::min(d4, d5), "memzero_ecore_steer");
        return chosen;
    }

    // Default fast zeroing on Lane 3 (Native INT64)
    constexpr uint32_t preferred = 3;
    uint32_t depth = pipelines[preferred].queue_depth.load(std::memory_order_relaxed);
    pipelines[preferred].queue_depth.fetch_add(1, std::memory_order_relaxed);
    total_dispatched.fetch_add(1, std::memory_order_relaxed);
    return preferred;
}

/* ========================================================================= */
/* 4. 18 MULTI-LIMB SCHEMAS CATALOG (Machines-Rules.td Section 14)           */
/* ========================================================================= */

static const hs_multi_limb_schema_desc_t g_multi_limb_schemas[18] = {
    { HS_EMU_SCHEMA_FLOAT_2X8,  "Schema_Float_2x8",  2, 8,  16,  1, 9,  9,  "FP16 via Dual FP8",       "2x 8-bit limbs for half precision arithmetic" },
    { HS_EMU_SCHEMA_FLOAT_4X8,  "Schema_Float_4x8",  4, 8,  32,  1, 9,  9,  "FP32 via Quad FP8",       "4x 8-bit limbs stolen on FP32 saturation" },
    { HS_EMU_SCHEMA_FLOAT_2X16, "Schema_Float_2x16", 2, 16, 32,  1, 8,  8,  "FP32 via Dual FP16",      "2x 16-bit limbs stolen on FP32 saturation" },
    { HS_EMU_SCHEMA_FLOAT_4X16, "Schema_Float_4x16", 4, 16, 64,  1, 7,  7,  "FP64 via Quad FP16",      "4x 16-bit limbs stolen on FP64 saturation" },
    { HS_EMU_SCHEMA_FLOAT_2X32, "Schema_Float_2x32", 2, 32, 64,  1, 6,  6,  "FP64 via Dual FP32",      "2x 32-bit limbs stolen on FP64 saturation" },
    { HS_EMU_SCHEMA_FLOAT_4X32, "Schema_Float_4x32", 4, 32, 128, 1, 6,  6,  "FP128 Candidate",         "4x 32-bit limbs for quad float emulation" },
    { HS_EMU_SCHEMA_FLOAT_2X64, "Schema_Float_2x64", 2, 64, 128, 1, 10, 10, "Double-Double (128-bit)", "2x 64-bit limbs Knuth/Dekker Double-Double" },
    { HS_EMU_SCHEMA_FLOAT_4X64, "Schema_Float_4x64", 4, 64, 256, 1, 11, 11, "Quad-Double (256-bit)",   "4x 64-bit limbs Bailey Quad-Double" },
    { HS_EMU_SCHEMA_FLOAT_8X32, "Schema_Float_8x32", 8, 32, 256, 1, 6,  6,  "Oct-Single (256-bit)",    "8x 32-bit limbs parallel wide single float" },
    { HS_EMU_SCHEMA_INT_2X8,    "Schema_Int_2x8",    2, 8,  16,  0, 14, 14, "INT16 via Dual INT8",     "2x 8-bit integer limbs" },
    { HS_EMU_SCHEMA_INT_4X8,    "Schema_Int_4x8",    4, 8,  32,  0, 14, 14, "INT32 via Quad INT8",     "4x 8-bit integer limbs" },
    { HS_EMU_SCHEMA_INT_2X16,   "Schema_Int_2x16",   2, 16, 32,  0, 14, 14, "INT32 via Dual INT16",    "2x 16-bit integer limbs stolen on INT32 saturation" },
    { HS_EMU_SCHEMA_INT_4X16,   "Schema_Int_4x16",   4, 16, 64,  0, 13, 13, "INT64 via Quad INT16",    "4x 16-bit integer limbs stolen on INT64 saturation" },
    { HS_EMU_SCHEMA_INT_2X32,   "Schema_Int_2x32",   2, 32, 64,  0, 12, 12, "INT64 via Dual INT32",    "2x 32-bit integer limbs stolen on INT64 saturation" },
    { HS_EMU_SCHEMA_INT_4X32,   "Schema_Int_4x32",   4, 32, 128, 0, 12, 12, "INT128 via Quad INT32",   "4x 32-bit integer limbs for 128-bit operations" },
    { HS_EMU_SCHEMA_INT_2X64,   "Schema_Int_2x64",   2, 64, 128, 0, 15, 15, "INT128 / BigInteger",     "2x 64-bit integer limbs for 128-bit integers" },
    { HS_EMU_SCHEMA_INT_4X64,   "Schema_Int_4x64",   4, 64, 256, 0, 15, 15, "INT256",                  "4x 64-bit integer limbs for 256-bit cryptography" },
    { HS_EMU_SCHEMA_INT_8X32,   "Schema_Int_8x32",   8, 32, 256, 0, 12, 12, "INT256 via Oct INT32",    "8x 32-bit integer limbs for wide vectorized int" }
};

size_t hs_multi_limb_schema_count() noexcept {
    return 18;
}

const hs_multi_limb_schema_desc_t* hs_get_multi_limb_schema_desc(int schema_id) noexcept {
    for (size_t i = 0; i < 18; ++i) {
        if (g_multi_limb_schemas[i].schema_id == static_cast<uint8_t>(schema_id)) {
            return &g_multi_limb_schemas[i];
        }
    }
    return nullptr;
}

const hs_multi_limb_schema_desc_t* hs_multi_limb_schema_at(size_t index) noexcept {
    if (index >= 18) return nullptr;
    return &g_multi_limb_schemas[index];
}

/* ========================================================================= */
/* 5. 450 PARTITIONED EMULATION FORMATS (Machines-Rules.td Section 14 & 15)  */
/* ========================================================================= */

// Generates 18 DTypes x 25 Arithmetic Layouts = 450 Formats
static const char* const g_partition_names[25] = {
    "Scalar_1x",
    "Vector_2x",
    "Vector_4x",
    "Vector_8x",
    "Vector_16x",
    "Dot_Product_Reduction",
    "Fused_Multiply_Accumulate",
    "Double_Double_Precision",
    "Quad_Double_Precision",
    "Oct_Single_Precision",
    "Lossless_Multi_Limb_Integer",
    "Saturated_Spill_INT32",
    "Saturated_Spill_INT64",
    "Saturated_Spill_FP32",
    "Saturated_Spill_FP16",
    "Saturated_Spill_INT16",
    "Reduced_Precision_1e-2",
    "Reduced_Precision_1e-3",
    "Extended_Precision_Upgrade",
    "E_Core_Affine_Partition",
    "P_Core_Pinned_Partition",
    "AVX512_Vector_512b",
    "AVX2_Vector_256b",
    "SSE_Vector_128b",
    "Bitwise_Invariance_Exact"
};

struct PartitionFormatRegistry {
    hs_partitioned_emulation_format_t table[450]{};

    PartitionFormatRegistry() {
        uint32_t idx = 0;
        for (uint8_t dt = 0; dt < 18; ++dt) {
            for (uint8_t part = 0; part < 25; ++part) {
                auto& f = table[idx];
                f.format_id = idx;
                f.src_dtype = dt;

                // Determine dst_dtype based on partition semantics
                uint8_t target_dst = dt;
                if (part == 18) { // Extended_Precision_Upgrade
                    switch (dt) {
                    case HS_DTYPE_F8:   target_dst = HS_DTYPE_F16; break;
                    case HS_DTYPE_F16:  target_dst = HS_DTYPE_F32; break;
                    case HS_DTYPE_F32:  target_dst = HS_DTYPE_F64; break;
                    case HS_DTYPE_F64:  target_dst = HS_DTYPE_F128; break;
                    case HS_DTYPE_F128: target_dst = HS_DTYPE_F256; break;
                    case HS_DTYPE_I8:   target_dst = HS_DTYPE_I16; break;
                    case HS_DTYPE_U8:   target_dst = HS_DTYPE_U16; break;
                    case HS_DTYPE_I16:  target_dst = HS_DTYPE_I32; break;
                    case HS_DTYPE_U16:  target_dst = HS_DTYPE_U32; break;
                    case HS_DTYPE_I32:  target_dst = HS_DTYPE_I64; break;
                    case HS_DTYPE_U32:  target_dst = HS_DTYPE_U64; break;
                    case HS_DTYPE_I64:  target_dst = HS_DTYPE_I128; break;
                    case HS_DTYPE_U64:  target_dst = HS_DTYPE_U128; break;
                    case HS_DTYPE_I128: target_dst = HS_DTYPE_I256; break;
                    case HS_DTYPE_U128: target_dst = HS_DTYPE_U256; break;
                    default:            target_dst = dt; break;
                    }
                } else if (part == 16 || part == 17) { // Reduced_Precision_1e-2 / 1e-3
                    switch (dt) {
                    case HS_DTYPE_F256: target_dst = HS_DTYPE_F128; break;
                    case HS_DTYPE_F128: target_dst = HS_DTYPE_F64; break;
                    case HS_DTYPE_F64:  target_dst = HS_DTYPE_F32; break;
                    case HS_DTYPE_F32:  target_dst = (part == 16) ? HS_DTYPE_F16 : HS_DTYPE_F16; break;
                    case HS_DTYPE_F16:  target_dst = HS_DTYPE_F8; break;
                    default:            target_dst = dt; break;
                    }
                }
                f.dst_dtype = target_dst;

                // Map schema according to data type and partition
                if (dt == HS_DTYPE_F64) {
                    if (part == 7)       f.schema_id = HS_EMU_SCHEMA_FLOAT_2X64;
                    else if (part == 8)  f.schema_id = HS_EMU_SCHEMA_FLOAT_4X64;
                    else if (part == 16) f.schema_id = HS_EMU_SCHEMA_FLOAT_4X16;
                    else if (part == 17) f.schema_id = HS_EMU_SCHEMA_FLOAT_2X32;
                    else if (part == 18) f.schema_id = HS_EMU_SCHEMA_FLOAT_2X64; // F128 Double-Double
                    else                 f.schema_id = HS_EMU_SCHEMA_NONE;
                    f.preferred_alu_lane = 0;
                    f.spill_alu_lane     = (part >= 16) ? 7 : 6;
                    f.requires_fma       = (part == 6 || part == 7 || part == 8 || part == 18) ? 1 : 0;
                } else if (dt == HS_DTYPE_F32) {
                    if (part == 7)       f.schema_id = HS_EMU_SCHEMA_FLOAT_2X32;
                    else if (part == 16) f.schema_id = HS_EMU_SCHEMA_FLOAT_4X8;
                    else if (part == 17) f.schema_id = HS_EMU_SCHEMA_FLOAT_2X16;
                    else if (part == 18) f.schema_id = HS_EMU_SCHEMA_FLOAT_2X32; // Upgrade to F64
                    else                 f.schema_id = HS_EMU_SCHEMA_NONE;
                    f.preferred_alu_lane = 1;
                    f.spill_alu_lane     = (part >= 16) ? 9 : 8;
                    f.requires_fma       = (part == 6) ? 1 : 0;
                } else if (dt == HS_DTYPE_F128) {
                    f.schema_id          = (part == 18) ? HS_EMU_SCHEMA_FLOAT_4X64 : HS_EMU_SCHEMA_FLOAT_2X64;
                    f.preferred_alu_lane = 10;
                    f.spill_alu_lane     = 6;
                    f.requires_fma       = 1;
                } else if (dt == HS_DTYPE_F256) {
                    f.schema_id          = HS_EMU_SCHEMA_FLOAT_4X64;
                    f.preferred_alu_lane = 11;
                    f.spill_alu_lane     = 6;
                    f.requires_fma       = 1;
                } else if (dt == HS_DTYPE_I64 || dt == HS_DTYPE_U64) {
                    if (part == 11)      f.schema_id = HS_EMU_SCHEMA_INT_2X32;
                    else if (part == 15) f.schema_id = HS_EMU_SCHEMA_INT_4X16;
                    else if (part == 18) f.schema_id = HS_EMU_SCHEMA_INT_2X64; // Int128
                    else                 f.schema_id = HS_EMU_SCHEMA_NONE;
                    f.preferred_alu_lane = 3;
                    f.spill_alu_lane     = (part == 15) ? 13 : 12;
                    f.requires_fma       = 0;
                } else if (dt == HS_DTYPE_I128 || dt == HS_DTYPE_U128) {
                    f.schema_id          = (part == 18) ? HS_EMU_SCHEMA_INT_4X64 : HS_EMU_SCHEMA_INT_2X64;
                    f.preferred_alu_lane = 15;
                    f.spill_alu_lane     = 12;
                    f.requires_fma       = 0;
                } else if (dt == HS_DTYPE_I256 || dt == HS_DTYPE_U256) {
                    f.schema_id          = HS_EMU_SCHEMA_INT_4X64;
                    f.preferred_alu_lane = 15;
                    f.spill_alu_lane     = 12;
                    f.requires_fma       = 0;
                } else {
                    if (part == 18 && (dt == HS_DTYPE_I32 || dt == HS_DTYPE_U32)) {
                        f.schema_id = HS_EMU_SCHEMA_INT_2X32;
                    } else if (part == 18 && (dt == HS_DTYPE_I16 || dt == HS_DTYPE_U16)) {
                        f.schema_id = HS_EMU_SCHEMA_INT_2X16;
                    } else {
                        f.schema_id = HS_EMU_SCHEMA_NONE;
                    }
                    f.preferred_alu_lane = (dt == HS_DTYPE_F16) ? 2 : ((dt == HS_DTYPE_I32 || dt == HS_DTYPE_U32) ? 4 : 5);
                    f.spill_alu_lane     = 14;
                    f.requires_fma       = 0;
                }

                // Vector lane count
                if (part == 0)      f.vector_lanes = 1;
                else if (part == 1) f.vector_lanes = 2;
                else if (part == 2) f.vector_lanes = 4;
                else if (part == 3) f.vector_lanes = 8;
                else if (part == 4) f.vector_lanes = 16;
                else if (part == 21) f.vector_lanes = 16;
                else if (part == 22) f.vector_lanes = 8;
                else if (part == 23) f.vector_lanes = 4;
                else                f.vector_lanes = 1;

                f.is_lossless = (dt == HS_DTYPE_I64 || dt == HS_DTYPE_I32 || dt == HS_DTYPE_I16 ||
                                 dt == HS_DTYPE_U64 || dt == HS_DTYPE_U32 || dt == HS_DTYPE_U16 ||
                                 dt == HS_DTYPE_U8  || dt == HS_DTYPE_I8  || dt == HS_DTYPE_I128 ||
                                 dt == HS_DTYPE_U128 || dt == HS_DTYPE_I256 || dt == HS_DTYPE_U256 ||
                                 part == 24) ? 1 : 0;
                f.format_name = g_partition_names[part];
                idx++;
            }
        }
    }
};

static const PartitionFormatRegistry g_format_registry;

size_t hs_partitioned_emulation_format_count() noexcept {
    return 450;
}

const hs_partitioned_emulation_format_t* hs_get_partitioned_emulation_format(uint32_t format_id) noexcept {
    if (format_id >= 450) return nullptr;
    return &g_format_registry.table[format_id];
}

const hs_partitioned_emulation_format_t* hs_find_partitioned_emulation_format(uint8_t src_dtype, uint8_t dst_dtype, uint8_t vector_lanes) noexcept {
    // 1. Exact match for src_dtype, dst_dtype, and vector_lanes
    for (size_t i = 0; i < 450; ++i) {
        const auto& f = g_format_registry.table[i];
        if (f.src_dtype == src_dtype && f.dst_dtype == dst_dtype && f.vector_lanes == vector_lanes) {
            return &f;
        }
    }
    // 2. Match for src_dtype and dst_dtype
    for (size_t i = 0; i < 450; ++i) {
        const auto& f = g_format_registry.table[i];
        if (f.src_dtype == src_dtype && f.dst_dtype == dst_dtype) {
            return &f;
        }
    }
    // 3. Fallback: match src_dtype and vector_lanes
    for (size_t i = 0; i < 450; ++i) {
        const auto& f = g_format_registry.table[i];
        if (f.src_dtype == src_dtype && f.vector_lanes == vector_lanes) {
            return &f;
        }
    }
    // 4. Default: return scalar format for src_dtype
    uint32_t fallback_id = static_cast<uint32_t>(src_dtype % 18) * 25;
    return &g_format_registry.table[fallback_id];
}

const hs_partitioned_emulation_format_t* hs_partitioned_emulation_format_at(size_t index) noexcept {
    if (index >= 450) return nullptr;
    return &g_format_registry.table[index];
}

/* ========================================================================= */
/* 6. C EXPORT APIS                                                          */
/* ========================================================================= */

MSIMD_API uint32_t HS_custominiVm_alu_dispatch(uint32_t preferred_alu, uint8_t priority) {
    return g_hs_alu_nexus.dispatch_op(preferred_alu, priority);
}

MSIMD_API int HS_custominiVm_alu_dispatch_op(uint32_t preferred_alu, uint8_t priority) {
    return static_cast<int>(g_hs_alu_nexus.dispatch_op(preferred_alu, priority));
}

MSIMD_API int HS_custominiVm_alu_resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id) {
    return g_hs_alu_nexus.resolve_route(dtype, priority, out_is_emulated, out_schema_id);
}

MSIMD_API int HS_custominiVm_alu_resolve_route_precision(uint8_t dtype, uint8_t priority, float target_epsilon, int* out_is_emulated, int* out_schema_id) {
    return g_hs_alu_nexus.resolve_route_precision(dtype, priority, target_epsilon, out_is_emulated, out_schema_id);
}

MSIMD_API int HS_custominiVm_alu_resolve_tensor_route(const HS_custominiVm_tensor_t* tensor, uint8_t priority,
    int* out_is_emulated, int* out_schema_id) {
    uint8_t dt = tensor ? tensor->dtype : static_cast<uint8_t>(HS_DTYPE_F32);
    return g_hs_alu_nexus.resolve_route(dt, priority, out_is_emulated, out_schema_id);
}

MSIMD_API int HS_custominiVm_alu_resolve_route_by_dtype(uint8_t dtype, uint8_t priority,
    int* out_is_emulated, int* out_schema_id) {
    return g_hs_alu_nexus.resolve_route(dtype, priority, out_is_emulated, out_schema_id);
}

MSIMD_API void HS_custominiVm_alu_complete_op(uint32_t alu_lane) {
    g_hs_alu_nexus.complete_op(alu_lane);
}

MSIMD_API void HS_custominiVm_alu_release(uint32_t alu_lane) {
    g_hs_alu_nexus.complete_op(alu_lane);
}

MSIMD_API uint64_t HS_custominiVm_get_alu_dispatched_count(void) {
    return g_hs_alu_nexus.total_dispatched.load(std::memory_order_acquire);
}

MSIMD_API uint64_t HS_custominiVm_get_alu_emulation_dispatched_count(void) {
    return g_hs_alu_nexus.total_emulation_dispatched.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_get_allow_operation_dispatchings(void) {
    return g_hs_alu_nexus.allow_operation_dispatching.load(std::memory_order_acquire) ? 1 : 0;
}

MSIMD_API void HS_custominiVm_set_allow_operation_dispatchings(uint32_t enable) {
    g_hs_alu_nexus.allow_operation_dispatching.store(enable != 0, std::memory_order_release);
}

MSIMD_API const char* HS_custominiVm_get_emulation_schema_name(int schema_id) {
    switch (schema_id) {
    case HS_EMU_SCHEMA_NONE:         return "Native_ALU";
    case HS_EMU_SCHEMA_FLOAT_2X8:    return "Float_2x8 (16b: 2x 8-bit limbs)";
    case HS_EMU_SCHEMA_FLOAT_4X8:    return "Float_4x8 (32b: 4x 8-bit limbs)";
    case HS_EMU_SCHEMA_FLOAT_2X16:   return "Float_2x16 (32b: 2x 16-bit limbs)";
    case HS_EMU_SCHEMA_FLOAT_4X16:   return "Float_4x16 (64b: 4x 16-bit limbs)";
    case HS_EMU_SCHEMA_FLOAT_2X32:   return "Float_2x32 (64b: 2x 32-bit limbs)";
    case HS_EMU_SCHEMA_FLOAT_4X32:   return "Float_4x32 (128b: 4x 32-bit limbs)";
    case HS_EMU_SCHEMA_FLOAT_2X64:   return "Float_2x64 (128b: 2x 64-bit limbs Double-Double)";
    case HS_EMU_SCHEMA_FLOAT_4X64:   return "Float_4x64 (256b: 4x 64-bit limbs Quad-Double)";
    case HS_EMU_SCHEMA_FLOAT_8X32:   return "Float_8x32 (256b: 8x 32-bit limbs Oct-Single)";
    case HS_EMU_SCHEMA_INT_2X8:      return "Int_2x8 (16b: 2x 8-bit limbs)";
    case HS_EMU_SCHEMA_INT_4X8:      return "Int_4x8 (32b: 4x 8-bit limbs)";
    case HS_EMU_SCHEMA_INT_2X16:     return "Int_2x16 (32b: 2x 16-bit limbs)";
    case HS_EMU_SCHEMA_INT_4X16:     return "Int_4x16 (64b: 4x 16-bit limbs)";
    case HS_EMU_SCHEMA_INT_2X32:     return "Int_2x32 (64b: 2x 32-bit limbs)";
    case HS_EMU_SCHEMA_INT_4X32:     return "Int_4x32 (128b: 4x 32-bit limbs)";
    case HS_EMU_SCHEMA_INT_2X64:     return "Int_2x64 (128b: 2x 64-bit limbs Int128)";
    case HS_EMU_SCHEMA_INT_4X64:     return "Int_4x64 (256b: 4x 64-bit limbs Int256)";
    case HS_EMU_SCHEMA_INT_8X32:     return "Int_8x32 (256b: 8x 32-bit limbs)";
    default:                         return "Unknown_Emulation_Schema";
    }
}

MSIMD_API const char* HS_custominiVm_get_dtype_name(uint8_t dtype) {
    switch (dtype) {
    case HS_DTYPE_F64:  return "FP64";
    case HS_DTYPE_F32:  return "FP32";
    case HS_DTYPE_F16:  return "FP16";
    case HS_DTYPE_I64:  return "INT64";
    case HS_DTYPE_I32:  return "INT32";
    case HS_DTYPE_I16:  return "INT16";
    case HS_DTYPE_U8:   return "UINT8";
    case HS_DTYPE_F8:   return "FP8";
    case HS_DTYPE_I8:   return "INT8";
    case HS_DTYPE_F128: return "FP128";
    case HS_DTYPE_I128: return "INT128";
    case HS_DTYPE_F256: return "FP256";
    case HS_DTYPE_I256: return "INT256";
    case HS_DTYPE_U16:  return "UINT16";
    case HS_DTYPE_U32:  return "UINT32";
    case HS_DTYPE_U64:  return "UINT64";
    case HS_DTYPE_U128: return "UINT128";
    case HS_DTYPE_U256: return "UINT256";
    default:            return "UNKNOWN";
    }
}

MSIMD_API uint32_t HS_custominiVm_alu_select_card_table_lane(int allow_stealing) {
    return g_hs_alu_nexus.select_card_table_lane(allow_stealing != 0);
}

MSIMD_API uint32_t HS_custominiVm_alu_select_memzero_lane(int pcore_busy, int allow_stealing) {
    return g_hs_alu_nexus.select_memzero_lane(pcore_busy != 0, allow_stealing != 0);
}

MSIMD_API size_t HS_custominiVm_get_multi_limb_schema_count(void) {
    return hs_multi_limb_schema_count();
}

MSIMD_API size_t HS_custominiVm_get_partitioned_format_count(void) {
    return hs_partitioned_emulation_format_count();
}
