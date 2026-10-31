/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:1175-1265.
 */

#ifndef SHARE_GC_REDLET_REDLETALU_HPP
#define SHARE_GC_REDLET_REDLETALU_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. DTYPE & EMULATION SCHEMA DEFINITIONS                                   */
/* ========================================================================= */

typedef enum sfDType {
    SF_DTYPE_F64  = 0,
    SF_DTYPE_F32  = 1,
    SF_DTYPE_F16  = 2,
    SF_DTYPE_I64  = 3,
    SF_DTYPE_I32  = 4,
    SF_DTYPE_I16  = 5,
    SF_DTYPE_U8   = 6,
    SF_DTYPE_F8   = 7,
    SF_DTYPE_I8   = 8,
    SF_DTYPE_F128 = 9,
    SF_DTYPE_I128 = 10,
    SF_DTYPE_F256 = 11,
    SF_DTYPE_I256 = 12,
    SF_DTYPE_U16  = 13,
    SF_DTYPE_U32  = 14,
    SF_DTYPE_U64  = 15,
    SF_DTYPE_U128 = 16,
    SF_DTYPE_U256 = 17
} sfDType;

typedef enum sf_emulation_schema_e {
    SF_EMU_SCHEMA_NONE        = 0,
    SF_EMU_SCHEMA_FLOAT_2X32  = 1,
    SF_EMU_SCHEMA_FLOAT_4X16  = 2,
    SF_EMU_SCHEMA_FLOAT_2X16  = 3,
    SF_EMU_SCHEMA_FLOAT_4X8   = 4,
    SF_EMU_SCHEMA_FLOAT_2X64  = 5,
    SF_EMU_SCHEMA_FLOAT_4X64  = 6,
    SF_EMU_SCHEMA_INT_2X32    = 7,
    SF_EMU_SCHEMA_INT_4X16    = 8,
    SF_EMU_SCHEMA_INT_2X16    = 9,
    SF_EMU_SCHEMA_INT_2X64    = 10,
    SF_EMU_SCHEMA_INT_4X64    = 11
} sf_emulation_schema_e;

/* ========================================================================= */
/* 2. ALU PIPELINE SPECIFICATION                                             */
/* ========================================================================= */

struct sf_alu_pipeline_t {
    std::atomic<uint32_t> queue_depth{ 0 };
    uint8_t               limb_width_bits{ 64 };
    uint8_t               is_float{ 1 };
    uint8_t               is_emulation_bank{ 0 };
    uint8_t               _pad{ 0 };

    void init(uint8_t bits, uint8_t flt, uint8_t emu) {
        queue_depth.store(0, std::memory_order_relaxed);
        limb_width_bits = bits;
        is_float = flt;
        is_emulation_bank = emu;
        _pad = 0;
    }
};

/* ========================================================================= */
/* 3. ALU DISPATCH NEXUS & WORK-STEALING BALANCER (16 LANES)                 */
/* ========================================================================= */

struct sf_alu_dispatch_nexus_t {
    static constexpr size_t NUM_ALU_LANES = 16;
    static constexpr uint32_t SATURATION_THRESHOLD = 4;

    sf_alu_pipeline_t     pipelines[NUM_ALU_LANES];
    std::atomic<uint64_t> total_dispatched{ 0 };
    std::atomic<uint64_t> total_saturated_events{ 0 };
    std::atomic<uint64_t> total_emulation_dispatched{ 0 };
    std::atomic<bool>     allow_operation_dispatching{ true };

    sf_alu_dispatch_nexus_t();

    MSIMD_API uint32_t dispatch_op(uint32_t preferred_alu, uint8_t priority);
    MSIMD_API int      resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id);
    MSIMD_API void     complete_op(uint32_t alu_id);
    MSIMD_API void     release_op(uint32_t alu_lane) { complete_op(alu_lane); }
    MSIMD_API void     reset();
};

extern sf_alu_dispatch_nexus_t g_sf_alu_nexus;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API uint32_t SF_custominiVm_alu_dispatch(uint32_t preferred_alu, uint8_t priority);
MSIMD_API int      SF_custominiVm_alu_resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id);
MSIMD_API void     SF_custominiVm_alu_complete_op(uint32_t alu_lane);
MSIMD_API void     SF_custominiVm_alu_release(uint32_t alu_lane);
MSIMD_API uint32_t SF_custominiVm_get_allow_operation_dispatchings(void);
MSIMD_API void     SF_custominiVm_set_allow_operation_dispatchings(uint32_t enable);
MSIMD_API const char* SF_custominiVm_get_emulation_schema_name(int schema_id);
MSIMD_API const char* SF_custominiVm_get_dtype_name(uint8_t dtype);

#ifdef __cplusplus
}
#endif

using RedletALU = sf_alu_dispatch_nexus_t;

#endif // SHARE_GC_REDLET_REDLETALU_HPP
