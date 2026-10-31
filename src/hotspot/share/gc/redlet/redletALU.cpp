/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:1175-1265.
 */

#include "gc/redlet/redletALU.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

sf_alu_dispatch_nexus_t g_sf_alu_nexus;

sf_alu_dispatch_nexus_t::sf_alu_dispatch_nexus_t() {
    pipelines[0].init(64, 1, 0);  // Lane 0: Native FP64
    pipelines[1].init(32, 1, 0);  // Lane 1: Native FP32
    pipelines[2].init(16, 1, 0);  // Lane 2: Native FP16
    pipelines[3].init(64, 0, 0);  // Lane 3: Native INT64
    pipelines[4].init(32, 0, 0);  // Lane 4: Native INT32
    pipelines[5].init(16, 0, 0);  // Lane 5: Native INT16_8
    pipelines[6].init(32, 1, 1);  // Lane 6: Emu Float 2x32 (64b)
    pipelines[7].init(16, 1, 1);  // Lane 7: Emu Float 4x16 (64b)
    pipelines[8].init(16, 1, 1);  // Lane 8: Emu Float 2x16 (32b)
    pipelines[9].init(8,  1, 1);  // Lane 9: Emu Float 4x8  (32b)
    pipelines[10].init(64, 1, 1); // Lane 10: Emu Float 2x64 (128b)
    pipelines[11].init(64, 1, 1); // Lane 11: Emu Float 4x64 (256b)
    pipelines[12].init(32, 0, 1); // Lane 12: Emu Int 2x32 (64b)
    pipelines[13].init(16, 0, 1); // Lane 13: Emu Int 4x16 (64b)
    pipelines[14].init(16, 0, 1); // Lane 14: Emu Int 2x16 (32b)
    pipelines[15].init(64, 0, 1); // Lane 15: Emu Int 2x64 (128b)
}

uint32_t sf_alu_dispatch_nexus_t::dispatch_op(uint32_t preferred_alu, uint8_t priority) {
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
            sf_hotlog_emit(SF_Ebin0_ALU_SATURATE_STEAL, SF_Ebin1_WARN, 0,
                           static_cast<uint16_t>(best_alu), 0, 0,
                           static_cast<uint16_t>(preferred_alu), 0, pref_depth, "alu_lane_stolen");
            return best_alu;
        }
    }

    pipelines[preferred_alu].queue_depth.fetch_add(1, std::memory_order_relaxed);
    total_dispatched.fetch_add(1, std::memory_order_relaxed);
    sf_hotlog_emit(SF_Ebin0_ALU_DISPATCH_DIRECT, SF_Ebin1_TRACE, 0,
                   static_cast<uint16_t>(preferred_alu), 0, 0, 0, 0, pref_depth, "alu_direct_dispatch");
    return preferred_alu;
}

int sf_alu_dispatch_nexus_t::resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id) {
    uint32_t native_lane = 1;
    switch (dtype) {
    case SF_DTYPE_F64:  native_lane = 0; break;
    case SF_DTYPE_F32:  native_lane = 1; break;
    case SF_DTYPE_F16:  native_lane = 2; break;
    case SF_DTYPE_I64:
    case SF_DTYPE_U64:  native_lane = 3; break;
    case SF_DTYPE_I32:
    case SF_DTYPE_U32:  native_lane = 4; break;
    case SF_DTYPE_I16:
    case SF_DTYPE_U16:
    case SF_DTYPE_I8:
    case SF_DTYPE_U8:
    case SF_DTYPE_F8:   native_lane = 5; break;
    case SF_DTYPE_F128: native_lane = 10; break;
    case SF_DTYPE_I128:
    case SF_DTYPE_U128: native_lane = 15; break;
    case SF_DTYPE_F256: native_lane = 11; break;
    case SF_DTYPE_I256:
    case SF_DTYPE_U256: native_lane = 15; break;
    default:            native_lane = 1; break;
    }

    bool is_inherently_emulated = (native_lane >= 6 && native_lane <= 15);
    uint32_t default_schema = SF_EMU_SCHEMA_NONE;
    if (dtype == SF_DTYPE_F128) default_schema = SF_EMU_SCHEMA_FLOAT_2X64;
    else if (dtype == SF_DTYPE_I128 || dtype == SF_DTYPE_U128) default_schema = SF_EMU_SCHEMA_INT_2X64;
    else if (dtype == SF_DTYPE_F256) default_schema = SF_EMU_SCHEMA_FLOAT_4X64;
    else if (dtype == SF_DTYPE_I256 || dtype == SF_DTYPE_U256) default_schema = SF_EMU_SCHEMA_INT_4X64;

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

        if (dtype == SF_DTYPE_F64) {
            test_candidate(7, SF_EMU_SCHEMA_FLOAT_4X16);
            test_candidate(6, SF_EMU_SCHEMA_FLOAT_2X32);
        } else if (dtype == SF_DTYPE_F32) {
            test_candidate(8, SF_EMU_SCHEMA_FLOAT_2X16);
            test_candidate(9, SF_EMU_SCHEMA_FLOAT_4X8);
        } else if (dtype == SF_DTYPE_I64 || dtype == SF_DTYPE_U64) {
            test_candidate(12, SF_EMU_SCHEMA_INT_2X32);
            test_candidate(13, SF_EMU_SCHEMA_INT_4X16);
        } else if (dtype == SF_DTYPE_I32 || dtype == SF_DTYPE_U32) {
            test_candidate(14, SF_EMU_SCHEMA_INT_2X16);
        }

        if (best_lane != native_lane) {
            total_dispatched.fetch_add(1, std::memory_order_relaxed);
            total_emulation_dispatched.fetch_add(1, std::memory_order_relaxed);
            pipelines[best_lane].queue_depth.fetch_add(1, std::memory_order_relaxed);

            sf_hotlog_emit(SF_Ebin0_ALU_SATURATE_STEAL, SF_Ebin1_WARN, 0,
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

void sf_alu_dispatch_nexus_t::complete_op(uint32_t alu_id) {
    alu_id %= NUM_ALU_LANES;
    uint32_t d = pipelines[alu_id].queue_depth.load(std::memory_order_relaxed);
    while (d > 0 && !pipelines[alu_id].queue_depth.compare_exchange_weak(d, d - 1, std::memory_order_relaxed)) {}
}

void sf_alu_dispatch_nexus_t::reset() {
    for (size_t i = 0; i < NUM_ALU_LANES; ++i) {
        pipelines[i].queue_depth.store(0, std::memory_order_relaxed);
    }
    total_dispatched.store(0, std::memory_order_relaxed);
    total_saturated_events.store(0, std::memory_order_relaxed);
    total_emulation_dispatched.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* C EXPORT APIS                                                             */
/* ========================================================================= */

MSIMD_API uint32_t SF_custominiVm_alu_dispatch(uint32_t preferred_alu, uint8_t priority) {
    return g_sf_alu_nexus.dispatch_op(preferred_alu, priority);
}

MSIMD_API int SF_custominiVm_alu_resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id) {
    return g_sf_alu_nexus.resolve_route(dtype, priority, out_is_emulated, out_schema_id);
}

MSIMD_API void SF_custominiVm_alu_complete_op(uint32_t alu_lane) {
    g_sf_alu_nexus.complete_op(alu_lane);
}

MSIMD_API void SF_custominiVm_alu_release(uint32_t alu_lane) {
    g_sf_alu_nexus.complete_op(alu_lane);
}

MSIMD_API uint32_t SF_custominiVm_get_allow_operation_dispatchings(void) {
    return g_sf_alu_nexus.allow_operation_dispatching.load(std::memory_order_acquire) ? 1 : 0;
}

MSIMD_API void SF_custominiVm_set_allow_operation_dispatchings(uint32_t enable) {
    g_sf_alu_nexus.allow_operation_dispatching.store(enable != 0, std::memory_order_release);
}

MSIMD_API const char* SF_custominiVm_get_emulation_schema_name(int schema_id) {
    switch (schema_id) {
    case SF_EMU_SCHEMA_NONE:        return "NONE";
    case SF_EMU_SCHEMA_FLOAT_2X32:  return "FLOAT_2X32";
    case SF_EMU_SCHEMA_FLOAT_4X16:  return "FLOAT_4X16";
    case SF_EMU_SCHEMA_FLOAT_2X16:  return "FLOAT_2X16";
    case SF_EMU_SCHEMA_FLOAT_4X8:   return "FLOAT_4X8";
    case SF_EMU_SCHEMA_FLOAT_2X64:  return "FLOAT_2X64";
    case SF_EMU_SCHEMA_FLOAT_4X64:  return "FLOAT_4X64";
    case SF_EMU_SCHEMA_INT_2X32:    return "INT_2X32";
    case SF_EMU_SCHEMA_INT_4X16:    return "INT_4X16";
    case SF_EMU_SCHEMA_INT_2X16:    return "INT_2X16";
    case SF_EMU_SCHEMA_INT_2X64:    return "INT_2X64";
    case SF_EMU_SCHEMA_INT_4X64:    return "INT_4X64";
    default:                        return "UNKNOWN";
    }
}

MSIMD_API const char* SF_custominiVm_get_dtype_name(uint8_t dtype) {
    switch (dtype) {
    case SF_DTYPE_F64:  return "FP64";
    case SF_DTYPE_F32:  return "FP32";
    case SF_DTYPE_F16:  return "FP16";
    case SF_DTYPE_I64:  return "INT64";
    case SF_DTYPE_I32:  return "INT32";
    case SF_DTYPE_I16:  return "INT16";
    case SF_DTYPE_U8:   return "UINT8";
    case SF_DTYPE_F8:   return "FP8";
    case SF_DTYPE_I8:   return "INT8";
    case SF_DTYPE_F128: return "FP128";
    case SF_DTYPE_I128: return "INT128";
    case SF_DTYPE_F256: return "FP256";
    case SF_DTYPE_I256: return "INT256";
    case SF_DTYPE_U16:  return "UINT16";
    case SF_DTYPE_U32:  return "UINT32";
    case SF_DTYPE_U64:  return "UINT64";
    case SF_DTYPE_U128: return "UINT128";
    case SF_DTYPE_U256: return "UINT256";
    default:            return "UNKNOWN";
    }
}
