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

#ifndef SHARE_GC_REDLET_REDLETMATHMSIMD_HPP
#define SHARE_GC_REDLET_REDLETMATHMSIMD_HPP

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <atomic>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletSIMD.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include "gc/redlet/redletTensorViews.hpp"

#ifndef MSIMD_XIP_BASE
#define MSIMD_XIP_BASE 0x08000000ULL
#endif

/* ========================================================================= */
/* 1. MSIMD CONTEXT, TIER, PRECISION & LATTICE DATA STRUCTURES               */
/* ========================================================================= */

typedef enum msimd_status_t {
    MSIMD_SUCCESS                    = 0,
    MSIMD_ERROR_INVALID_ARGUMENT     = -1,
    MSIMD_ERROR_OUT_OF_MEMORY        = -2,
    MSIMD_ERROR_UNSUPPORTED_HARDWARE = -3,
    MSIMD_ERROR_BACKEND_FAILURE      = -4,
    MSIMD_ERROR_NOT_IMPLEMENTED      = -5,
    MSIMD_ERROR_HARDWARE_FAULT       = -6,
    MSIMD_ERROR_FLAG_CONFLICT        = -7
} msimd_status_t;

typedef enum msimd_isa_t {
    MSIMD_ISA_AUTO    = 0,
    MSIMD_ISA_SCALAR  = 1,
    MSIMD_ISA_SSE42   = 2,
    MSIMD_ISA_AVX2    = 3,
    MSIMD_ISA_AVX512  = 4,
    MSIMD_ISA_AVX10_2 = 5,
    MSIMD_ISA_AMX     = 6,
    MSIMD_ISA_NEON    = 7,
    MSIMD_ISA_SVE2    = 8,
    MSIMD_ISA_SME2    = 9,
    MSIMD_ISA_RVV     = 10,
    MSIMD_ISA_LASX    = 11
} msimd_isa_t;

typedef enum msimd_backend_t {
    MSIMD_BACKEND_AUTO     = 0,
    MSIMD_BACKEND_FALLBACK = 1,
    MSIMD_BACKEND_TBB      = 2
} msimd_backend_t;

typedef enum msimd_hw_tier_t {
    MSIMD_TIER_0_MATRIX_TILES  = 0,
    MSIMD_TIER_1_WIDE_VECTOR   = 1,
    MSIMD_TIER_2_BASIC_SIMD    = 2,
    MSIMD_TIER_3_COPROCESSORS  = 3,
    MSIMD_TIER_4_SCALAR_FPU    = 4,
    MSIMD_TIER_5_SOFTFLOAT_ALU = 5
} msimd_hw_tier_t;

typedef enum msimd_precision_t {
    MSIMD_PRECISION_FP32 = 0,
    MSIMD_PRECISION_FP64 = 1
} msimd_precision_t;

typedef struct msimd_config_t {
    msimd_profile_mode_t profile;
    size_t               static_arena_size_bytes;
    uint32_t             max_loop_iterations;
    bool                 forbid_runtime_heap;
    void*                user_static_arena_buf;
} msimd_config_t;

typedef struct msimd_cpu_info_t {
    char            vendor[16];
    int             num_physical_cores;
    int             num_logical_cores;
    int             is_numa;
    int             is_hybrid;
    int             l1_data_kb;
    int             l2_kb;
    int             l3_mb;
    int             has_fma;
    int             has_avx2;
    int             has_avx_vnni;
    int             has_avx512;
    int             has_avx512_vnni;
    int             has_avx512_bf16;
    int             has_amx;
    int             has_sve;
    int             has_sve2;
    int             has_sme;
    int             has_rvv;
    int             has_lasx;
    int             has_cordic;
    int             has_fmac;
    int             has_tmu;
    int             has_dsp;
    msimd_hw_tier_t tier;
} msimd_cpu_info_t;

typedef struct msimd_context_t {
    msimd_cpu_info_t cpu_info;
    msimd_isa_t      isa;
    msimd_backend_t  backend;
    void*            backend_handle;
    int              optimal_ladder_level;
    msimd_config_t   config;
    uint8_t*         static_arena_base;
    size_t           static_arena_capacity;
    size_t           static_arena_offset;
    bool             arena_is_user_provided;
} msimd_context_t;

typedef msimd_context_t* msimd_context;

typedef struct msimd_lattice_t {
    msimd_context     ctx;
    int               dim;
    size_t            nx;
    size_t            ny;
    size_t            nz;
    size_t            total_elements;
    size_t            elem_size;
    msimd_precision_t precision;
    void*             data_prev;
    void*             data_curr;
    void*             data_next;
    bool              allocated_from_arena;
} msimd_lattice_t;

typedef msimd_lattice_t* msimd_lattice;
typedef SF_custominiVm_tensor_t msimd_tensor_t;
typedef void (*msimd_parallel_for_fn)(void* user_data, size_t start, size_t end);

/* ========================================================================= */
/* 2. C ABI EXPORTS FOR MATHEMATICAL & NUMERICAL EXECUTION                   */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

// 2.1 Cacheline/HugePage Aligned Allocators
MSIMD_API void* sf_aligned_alloc_cl(size_t size, size_t alignment);
MSIMD_API void  sf_aligned_free_cl(void* ptr);
MSIMD_API void* sf_alloc_aligned(size_t size, size_t alignment);
MSIMD_API void  sf_free_aligned(void* ptr);
MSIMD_API void* msimd_aligned_alloc_cl(size_t size, size_t alignment);
MSIMD_API void  msimd_aligned_free_cl(void* ptr);
MSIMD_API void* msimd_alloc_aligned(size_t size, size_t alignment);
MSIMD_API void  msimd_free_aligned(void* ptr);

// 2.2 Hardware Prefetchers, Non-Temporal Float/Double Stream Stores & Memory Fences
MSIMD_API void sf_prefetch_t0(const void* addr);
MSIMD_API void sf_prefetch_t1(const void* addr);
MSIMD_API void sf_prefetch_t2(const void* addr);
MSIMD_API void sf_prefetch_nta(const void* addr);
MSIMD_API void sf_prefetch_stream(const void* base, size_t count, size_t stride);
MSIMD_API void sf_stream_store_f32(float* dst, const float* src, size_t count);
MSIMD_API void sf_stream_store_f64(double* dst, const double* src, size_t count);
MSIMD_API void sf_hw_fence_full(void);
MSIMD_API void sf_hw_fence_store(void);
MSIMD_API void sf_hw_fence_load(void);
MSIMD_API void sf_memcpy_nt(void* dst, const void* src, size_t size);
MSIMD_API void sf_memzero_nt(void* dst, size_t size);

MSIMD_API void msimd_prefetch_t0(const void* addr);
MSIMD_API void msimd_prefetch_t1(const void* addr);
MSIMD_API void msimd_prefetch_t2(const void* addr);
MSIMD_API void msimd_prefetch_nta(const void* addr);
MSIMD_API void msimd_prefetch_stream(const void* base, size_t count, size_t stride);
MSIMD_API void msimd_stream_store_f32(float* dst, const float* src, size_t count);
MSIMD_API void msimd_stream_store_f64(double* dst, const double* src, size_t count);
MSIMD_API void msimd_hw_fence_full(void);
MSIMD_API void msimd_hw_fence_store(void);
MSIMD_API void msimd_hw_fence_load(void);
MSIMD_API void msimd_memcpy_nt(void* dst, const void* src, size_t size);
MSIMD_API void msimd_memzero_nt(void* dst, size_t size);

MSIMD_API void SF_custominiVm_prefetch_t0(const void* addr);
MSIMD_API void SF_custominiVm_prefetch_t1(const void* addr);
MSIMD_API void SF_custominiVm_prefetch_t2(const void* addr);
MSIMD_API void SF_custominiVm_prefetch_nta(const void* addr);
MSIMD_API void SF_custominiVm_prefetch_stream(const void* base, size_t count, size_t stride);
MSIMD_API void SF_custominiVm_stream_store_f32(float* dst, const float* src, size_t count);
MSIMD_API void SF_custominiVm_stream_store_f64(double* dst, const double* src, size_t count);
MSIMD_API void SF_custominiVm_fence_full(void);
MSIMD_API void SF_custominiVm_fence_store(void);
MSIMD_API void SF_custominiVm_fence_load(void);
MSIMD_API void SF_custominiVm_memcpy_nt(void* dst, const void* src, size_t size);
MSIMD_API void SF_custominiVm_memzero_nt(void* dst, size_t size);

// 2.3 MSIMD Engine Context, RDTSC GFLOPS Calibration & Asymmetric Hybrid parallel_for
MSIMD_API msimd_status_t       sf_backend_fallback_init(msimd_context ctx);
MSIMD_API void                 sf_backend_fallback_destroy(msimd_context ctx);
MSIMD_API void                 sf_backend_fallback_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data);

MSIMD_API msimd_status_t       sf_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config);
MSIMD_API msimd_status_t       sf_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend);
MSIMD_API msimd_profile_mode_t sf_get_active_profile(msimd_context ctx);
MSIMD_API const msimd_config_t* sf_get_config(msimd_context ctx);
MSIMD_API void                 sf_destroy(msimd_context ctx);
MSIMD_API msimd_status_t       sf_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info);
MSIMD_API msimd_status_t       sf_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops);
MSIMD_API void                 sf_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data);

MSIMD_API msimd_status_t       msimd_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config);
MSIMD_API msimd_status_t       msimd_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend);
MSIMD_API msimd_profile_mode_t msimd_get_active_profile(msimd_context ctx);
MSIMD_API const msimd_config_t* msimd_get_config(msimd_context ctx);
MSIMD_API void                 msimd_destroy(msimd_context ctx);
MSIMD_API msimd_status_t       msimd_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info);
MSIMD_API msimd_status_t       msimd_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops);
MSIMD_API void                 msimd_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data);

MSIMD_API msimd_status_t       SF_custominiVm_msimd_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config);
MSIMD_API msimd_status_t       SF_custominiVm_msimd_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend);
MSIMD_API msimd_profile_mode_t SF_custominiVm_msimd_get_profile(msimd_context ctx);
MSIMD_API const msimd_config_t* SF_custominiVm_msimd_get_config(msimd_context ctx);
MSIMD_API void                 SF_custominiVm_msimd_destroy(msimd_context ctx);
MSIMD_API msimd_status_t       SF_custominiVm_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info);
MSIMD_API msimd_status_t       SF_custominiVm_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops);
MSIMD_API void                 SF_custominiVm_parallel_for(size_t total, msimd_parallel_for_fn fn, void* user_data);

// 2.4 3-Buffer Lattice Geometry & Stencil Memory Tensors
MSIMD_API msimd_status_t sf_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision);
MSIMD_API void           sf_lattice_destroy(msimd_lattice lat);
MSIMD_API void*          sf_lattice_get_curr(msimd_lattice lat);
MSIMD_API void*          sf_lattice_get_prev(msimd_lattice lat);
MSIMD_API void*          sf_lattice_get_next(msimd_lattice lat);
MSIMD_API void           sf_lattice_swap(msimd_lattice lat);

MSIMD_API msimd_status_t msimd_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision);
MSIMD_API void           msimd_lattice_destroy(msimd_lattice lat);
MSIMD_API void*          msimd_lattice_get_curr(msimd_lattice lat);
MSIMD_API void*          msimd_lattice_get_prev(msimd_lattice lat);
MSIMD_API void*          msimd_lattice_get_next(msimd_lattice lat);
MSIMD_API void           msimd_lattice_swap(msimd_lattice lat);

MSIMD_API msimd_status_t SF_custominiVm_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision);
MSIMD_API void           SF_custominiVm_lattice_destroy(msimd_lattice lat);
MSIMD_API void*          SF_custominiVm_lattice_get_curr(msimd_lattice lat);
MSIMD_API void*          SF_custominiVm_lattice_get_prev(msimd_lattice lat);
MSIMD_API void*          SF_custominiVm_lattice_get_next(msimd_lattice lat);
MSIMD_API void           SF_custominiVm_lattice_swap(msimd_lattice lat);

// 2.5 Hardware Coprocessors (Q31 CORDIC, Q31 FMAC FIR, TMU), Soft-Float ALU & XIP/ITCM
MSIMD_API void   sf_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* precision_target);
MSIMD_API void   sf_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* precision_target);
MSIMD_API void   sf_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* precision_target);
MSIMD_API float  sf_tmu_sin_f32(float rad, const msimd_tensor_t* precision_target);
MSIMD_API float  sf_tmu_cos_f32(float rad, const msimd_tensor_t* precision_target);
MSIMD_API float  sf_softfloat_add_f32(float a, float b, const msimd_tensor_t* precision_target);
MSIMD_API float  sf_softfloat_mul_f32(float a, float b, const msimd_tensor_t* precision_target);
MSIMD_API float  sf_softfloat_div_f32(float a, float b, const msimd_tensor_t* precision_target);
MSIMD_API double sf_softfloat_add_f64(double a, double b, const msimd_tensor_t* precision_target);
MSIMD_API double sf_softfloat_mul_f64(double a, double b, const msimd_tensor_t* precision_target);
MSIMD_API double sf_softfloat_div_f64(double a, double b, const msimd_tensor_t* precision_target);
MSIMD_API int    sf_xip_verify_flash(void);
MSIMD_API size_t sf_decompress_block_to_itcm(const void* src_compressed, size_t src_size, void* itcm_dest, size_t max_dest);

MSIMD_API void   msimd_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* p);
MSIMD_API void   msimd_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* p);
MSIMD_API void   msimd_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* p);
MSIMD_API float  msimd_tmu_sin_f32(float rad, const msimd_tensor_t* p);
MSIMD_API float  msimd_tmu_cos_f32(float rad, const msimd_tensor_t* p);
MSIMD_API float  msimd_softfloat_add_f32(float a, float b, const msimd_tensor_t* p);
MSIMD_API float  msimd_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p);
MSIMD_API float  msimd_softfloat_div_f32(float a, float b, const msimd_tensor_t* p);
MSIMD_API double msimd_softfloat_add_f64(double a, double b, const msimd_tensor_t* p);
MSIMD_API double msimd_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p);
MSIMD_API double msimd_softfloat_div_f64(double a, double b, const msimd_tensor_t* p);
MSIMD_API int    msimd_xip_verify_flash(void);
MSIMD_API size_t msimd_decompress_block_to_itcm(const void* src, size_t src_size, void* dest, size_t max_dest);

MSIMD_API void   SF_custominiVm_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* p);
MSIMD_API void   SF_custominiVm_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* p);
MSIMD_API void   SF_custominiVm_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* p);
MSIMD_API float  SF_custominiVm_tmu_sin_f32(float rad, const msimd_tensor_t* p);
MSIMD_API float  SF_custominiVm_tmu_cos_f32(float rad, const msimd_tensor_t* p);
MSIMD_API float  SF_custominiVm_softfloat_add_f32(float a, float b, const msimd_tensor_t* p);
MSIMD_API float  SF_custominiVm_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p);
MSIMD_API float  SF_custominiVm_softfloat_div_f32(float a, float b, const msimd_tensor_t* p);
MSIMD_API double SF_custominiVm_softfloat_add_f64(double a, double b, const msimd_tensor_t* p);
MSIMD_API double SF_custominiVm_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p);
MSIMD_API double SF_custominiVm_softfloat_div_f64(double a, double b, const msimd_tensor_t* p);
MSIMD_API int    SF_custominiVm_xip_verify_flash(void);
MSIMD_API size_t SF_custominiVm_decompress_block_to_itcm(const void* src, size_t src_size, void* dest, size_t max_dest);

// 2.6 Primitiva Raw Sector R/W (SSIOLSC[PrimitiveRW] Header-Bypass Primitives)
MSIMD_API void* SF_primitiva_alloc(size_t bytes, size_t align);
MSIMD_API void  SF_primitiva_write(void* sector_ptr, const void* src, size_t bytes);
MSIMD_API void  SF_primitiva_read(const void* sector_ptr, void* dst, size_t bytes);
MSIMD_API void  SF_primitiva_overwrite(void* sector_ptr, const void* src, size_t bytes);
MSIMD_API void  SF_primitiva_zero(void* sector_ptr, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETMATHMSIMD_HPP
