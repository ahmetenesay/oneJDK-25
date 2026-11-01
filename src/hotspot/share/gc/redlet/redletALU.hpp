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
/* 1. DTYPE & EMULATION SCHEMA DEFINITIONS (Machines-Rules.td Section 14)    */
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

#ifndef SF_TENSOR_T_DEFINED
#define SF_TENSOR_T_DEFINED
typedef struct SF_custominiVm_tensor_t {
    void*   data{ nullptr };
    size_t  byte_size{ 0 };
    uint8_t dtype{ 0 };
    int32_t ndim{ 0 };
    int32_t shape[8]{ 0 };
} SF_custominiVm_tensor_t;
#endif

/* 18 Multi-Limb Schemas from Machines-Rules.td Section 14 */
typedef enum sf_emulation_schema_e {
    SF_EMU_SCHEMA_NONE         = 0x00,
    SF_EMU_SCHEMA_FLOAT_2X8    = 0x01,
    SF_EMU_SCHEMA_FLOAT_4X8    = 0x02,
    SF_EMU_SCHEMA_FLOAT_2X16   = 0x03,
    SF_EMU_SCHEMA_FLOAT_4X16   = 0x04,
    SF_EMU_SCHEMA_FLOAT_2X32   = 0x05,
    SF_EMU_SCHEMA_FLOAT_4X32   = 0x06,
    SF_EMU_SCHEMA_FLOAT_2X64   = 0x07, // Double-Double (128-bit)
    SF_EMU_SCHEMA_FLOAT_4X64   = 0x08, // Quad-Double (256-bit)
    SF_EMU_SCHEMA_FLOAT_8X32   = 0x09, // Oct-Single (256-bit)
    SF_EMU_SCHEMA_INT_2X8      = 0x11,
    SF_EMU_SCHEMA_INT_4X8      = 0x12,
    SF_EMU_SCHEMA_INT_2X16     = 0x13,
    SF_EMU_SCHEMA_INT_4X16     = 0x14,
    SF_EMU_SCHEMA_INT_2X32     = 0x15,
    SF_EMU_SCHEMA_INT_4X32     = 0x16,
    SF_EMU_SCHEMA_INT_2X64     = 0x17, // Int128 / BigInteger
    SF_EMU_SCHEMA_INT_4X64     = 0x18, // Int256
    SF_EMU_SCHEMA_INT_8X32     = 0x19
} sf_emulation_schema_e;
typedef sf_emulation_schema_e SF_custominiVm_emu_schema_t;

/* Multi-Limb Schema Full Metadata Descriptor */
struct sf_multi_limb_schema_desc_t {
    uint8_t     schema_id;
    const char* schema_name;
    uint8_t     limb_count;
    uint8_t     limb_bit_width;
    uint16_t    total_bits;
    uint8_t     is_float;
    uint8_t     target_lane;
    uint8_t     fallback_lane;
    const char* math_model;
    const char* description;
};

/* 450 Partitioned Emulation Formats (Machines-Rules.td Section 14 & 15) */
struct sf_partitioned_emulation_format_t {
    uint32_t    format_id;
    uint8_t     src_dtype;
    uint8_t     dst_dtype;
    uint8_t     schema_id;
    uint8_t     vector_lanes;
    uint8_t     preferred_alu_lane;
    uint8_t     spill_alu_lane;
    uint8_t     is_lossless;
    uint8_t     requires_fma;
    const char* format_name;
};

/* ========================================================================= */
/* 2. ALU PIPELINE SPECIFICATION (16 Virtual Execution Lines)                */
/* ========================================================================= */

struct sf_alu_pipeline_t {
    std::atomic<uint32_t> queue_depth{ 0 };
    uint8_t               limb_width_bits{ 64 };
    uint8_t               is_float{ 1 };
    uint8_t               is_emulation_bank{ 0 };
    uint8_t               is_ecore_affine{ 0 };
    const char*           name{ "Uninitialized" };

    void init(uint8_t bits, uint8_t flt, uint8_t emu, uint8_t ecore = 0, const char* lane_name = nullptr) {
        queue_depth.store(0, std::memory_order_relaxed);
        limb_width_bits   = bits;
        is_float          = flt;
        is_emulation_bank = emu;
        is_ecore_affine   = ecore;
        name              = lane_name ? lane_name : "ALU_Lane";
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
    MSIMD_API int      resolve_route_precision(uint8_t dtype, uint8_t priority, float target_epsilon, int* out_is_emulated, int* out_schema_id);
    MSIMD_API void     complete_op(uint32_t alu_id);
    MSIMD_API void     release_op(uint32_t alu_lane) { complete_op(alu_lane); }
    MSIMD_API void     reset();

    // HotSpot RedletGC Internal Execution Line Routing (Card Table & Memzero)
    MSIMD_API uint32_t select_card_table_lane(bool allow_stealing = true);
    MSIMD_API uint32_t select_memzero_lane(bool pcore_busy = false, bool allow_stealing = true);
};

extern sf_alu_dispatch_nexus_t g_sf_alu_nexus;

/* Multi-Limb Schema & Partitioned Format Catalog APIs */
MSIMD_API size_t sf_multi_limb_schema_count() noexcept;
MSIMD_API const sf_multi_limb_schema_desc_t* sf_get_multi_limb_schema_desc(int schema_id) noexcept;
MSIMD_API const sf_multi_limb_schema_desc_t* sf_multi_limb_schema_at(size_t index) noexcept;

MSIMD_API size_t sf_partitioned_emulation_format_count() noexcept;
MSIMD_API const sf_partitioned_emulation_format_t* sf_get_partitioned_emulation_format(uint32_t format_id) noexcept;
MSIMD_API const sf_partitioned_emulation_format_t* sf_find_partitioned_emulation_format(uint8_t src_dtype, uint8_t dst_dtype, uint8_t vector_lanes) noexcept;
MSIMD_API const sf_partitioned_emulation_format_t* sf_partitioned_emulation_format_at(size_t index) noexcept;

/* ========================================================================= */
/* 4. HOTSPOT C++20 STATIC INTERFACE (RedletALU)                             */
/* ========================================================================= */

class RedletALU {
public:
    static constexpr size_t NUM_LANES = sf_alu_dispatch_nexus_t::NUM_ALU_LANES;
    static constexpr uint32_t SATURATION_THRESHOLD = sf_alu_dispatch_nexus_t::SATURATION_THRESHOLD;

    static inline sf_alu_dispatch_nexus_t& nexus() noexcept {
        return g_sf_alu_nexus;
    }

    static inline uint32_t dispatch(uint32_t preferred_alu, uint8_t priority) {
        return g_sf_alu_nexus.dispatch_op(preferred_alu, priority);
    }

    static inline int resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id) {
        return g_sf_alu_nexus.resolve_route(dtype, priority, out_is_emulated, out_schema_id);
    }

    static inline int resolve_route_precision(uint8_t dtype, uint8_t priority, float target_epsilon, int* out_is_emulated, int* out_schema_id) {
        return g_sf_alu_nexus.resolve_route_precision(dtype, priority, target_epsilon, out_is_emulated, out_schema_id);
    }

    static inline void complete(uint32_t alu_id) {
        g_sf_alu_nexus.complete_op(alu_id);
    }

    static inline void release(uint32_t alu_id) {
        g_sf_alu_nexus.complete_op(alu_id);
    }

    static inline void reset() {
        g_sf_alu_nexus.reset();
    }

    static inline uint64_t total_dispatched() {
        return g_sf_alu_nexus.total_dispatched.load(std::memory_order_relaxed);
    }

    static inline uint64_t total_saturated() {
        return g_sf_alu_nexus.total_saturated_events.load(std::memory_order_relaxed);
    }

    static inline uint64_t total_emulation() {
        return g_sf_alu_nexus.total_emulation_dispatched.load(std::memory_order_relaxed);
    }

    static inline bool allow_dispatching() {
        return g_sf_alu_nexus.allow_operation_dispatching.load(std::memory_order_acquire);
    }

    static inline void set_allow_dispatching(bool enable) {
        g_sf_alu_nexus.allow_operation_dispatching.store(enable, std::memory_order_release);
    }

    static inline uint32_t queue_depth(uint32_t lane) {
        if (lane >= NUM_LANES) return 0;
        return g_sf_alu_nexus.pipelines[lane].queue_depth.load(std::memory_order_relaxed);
    }

    static inline const sf_alu_pipeline_t& pipeline(uint32_t lane) {
        return g_sf_alu_nexus.pipelines[lane % NUM_LANES];
    }

    // GC-specific fast lane routing
    static inline uint32_t select_card_table_lane(bool allow_stealing = true) {
        return g_sf_alu_nexus.select_card_table_lane(allow_stealing);
    }

    static inline uint32_t select_memzero_lane(bool pcore_busy = false, bool allow_stealing = true) {
        return g_sf_alu_nexus.select_memzero_lane(pcore_busy, allow_stealing);
    }

    // Schema & format catalog queries
    static inline const sf_multi_limb_schema_desc_t* get_schema_desc(int schema_id) noexcept {
        return sf_get_multi_limb_schema_desc(schema_id);
    }

    static inline const sf_partitioned_emulation_format_t* get_format(uint32_t format_id) noexcept {
        return sf_get_partitioned_emulation_format(format_id);
    }

    static inline const sf_partitioned_emulation_format_t* find_format(uint8_t src_dtype, uint8_t dst_dtype, uint8_t vector_lanes) noexcept {
        return sf_find_partitioned_emulation_format(src_dtype, dst_dtype, vector_lanes);
    }

    static inline size_t schema_count() noexcept {
        return sf_multi_limb_schema_count();
    }

    static inline size_t format_count() noexcept {
        return sf_partitioned_emulation_format_count();
    }
};

/* ========================================================================= */
/* 5. C EXPORT APIS                                                          */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API uint32_t SF_custominiVm_alu_dispatch(uint32_t preferred_alu, uint8_t priority);
MSIMD_API int      SF_custominiVm_alu_dispatch_op(uint32_t preferred_alu, uint8_t priority);
MSIMD_API int      SF_custominiVm_alu_resolve_route(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id);
MSIMD_API int      SF_custominiVm_alu_resolve_route_precision(uint8_t dtype, uint8_t priority, float target_epsilon, int* out_is_emulated, int* out_schema_id);
MSIMD_API int      SF_custominiVm_alu_resolve_tensor_route(const SF_custominiVm_tensor_t* tensor, uint8_t priority, int* out_is_emulated, int* out_schema_id);
MSIMD_API int      SF_custominiVm_alu_resolve_route_by_dtype(uint8_t dtype, uint8_t priority, int* out_is_emulated, int* out_schema_id);
MSIMD_API void     SF_custominiVm_alu_complete_op(uint32_t alu_lane);
MSIMD_API void     SF_custominiVm_alu_release(uint32_t alu_lane);
MSIMD_API uint64_t SF_custominiVm_get_alu_dispatched_count(void);
MSIMD_API uint64_t SF_custominiVm_get_alu_emulation_dispatched_count(void);
MSIMD_API uint32_t SF_custominiVm_get_allow_operation_dispatchings(void);
MSIMD_API void     SF_custominiVm_set_allow_operation_dispatchings(uint32_t enable);
MSIMD_API const char* SF_custominiVm_get_emulation_schema_name(int schema_id);
MSIMD_API const char* SF_custominiVm_get_dtype_name(uint8_t dtype);

// Card Table & Memzero Lane Dispatch C-APIs
MSIMD_API uint32_t SF_custominiVm_alu_select_card_table_lane(int allow_stealing);
MSIMD_API uint32_t SF_custominiVm_alu_select_memzero_lane(int pcore_busy, int allow_stealing);
MSIMD_API size_t   SF_custominiVm_get_multi_limb_schema_count(void);
MSIMD_API size_t   SF_custominiVm_get_partitioned_format_count(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETALU_HPP
