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

#ifndef SHARE_GC_REDJET_REDJETMATHRJ_HPP
#define SHARE_GC_REDJET_REDJETMATHRJ_HPP

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <atomic>

#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/simd/redjetSIMD.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/runtime/redjetTensorViews.hpp"

#ifndef RJ_XIP_BASE
#define RJ_XIP_BASE 0x08000000ULL
#endif

/* ========================================================================= */
/* 1. RJ SIMD CONTEXT, TIER, PRECISION & LATTICE DATA STRUCTURES             */
/* ========================================================================= */

typedef enum rj_status_t {
    RJ_SUCCESS                    = 0,
    RJ_ERROR_INVALID_ARGUMENT     = -1,
    RJ_ERROR_OUT_OF_MEMORY        = -2,
    RJ_ERROR_UNSUPPORTED_HARDWARE = -3,
    RJ_ERROR_BACKEND_FAILURE      = -4,
    RJ_ERROR_NOT_IMPLEMENTED      = -5,
    RJ_ERROR_HARDWARE_FAULT       = -6,
    RJ_ERROR_FLAG_CONFLICT        = -7
} rj_status_t;
using msimd_status_t   = rj_status_t;
using rj_simd_status_t = rj_status_t;

typedef enum rj_isa_t {
    RJ_ISA_AUTO    = 0,
    RJ_ISA_SCALAR  = 1,
    RJ_ISA_SSE42   = 2,
    RJ_ISA_AVX2    = 3,
    RJ_ISA_AVX512  = 4,
    RJ_ISA_AVX10_2 = 5,
    RJ_ISA_AMX     = 6,
    RJ_ISA_NEON    = 7,
    RJ_ISA_SVE2    = 8,
    RJ_ISA_SME2    = 9,
    RJ_ISA_RVV     = 10,
    RJ_ISA_LASX    = 11
} rj_isa_t;
using msimd_isa_t   = rj_isa_t;
using rj_simd_isa_t = rj_isa_t;

typedef enum rj_backend_t {
    RJ_BACKEND_AUTO     = 0,
    RJ_BACKEND_FALLBACK = 1,
    RJ_BACKEND_TBB      = 2
} rj_backend_t;
using msimd_backend_t   = rj_backend_t;
using rj_simd_backend_t = rj_backend_t;

typedef enum rj_hw_tier_t {
    RJ_TIER_0_MATRIX_TILES  = 0,
    RJ_TIER_1_WIDE_VECTOR   = 1,
    RJ_TIER_2_BASIC_SIMD    = 2,
    RJ_TIER_3_COPROCESSORS  = 3,
    RJ_TIER_4_SCALAR_FPU    = 4,
    RJ_TIER_5_SOFTFLOAT_ALU = 5
} rj_hw_tier_t;
using msimd_hw_tier_t   = rj_hw_tier_t;
using rj_simd_hw_tier_t = rj_hw_tier_t;

typedef enum rj_precision_t {
    RJ_PRECISION_FP32 = 0,
    RJ_PRECISION_FP64 = 1
} rj_precision_t;
using msimd_precision_t   = rj_precision_t;
using rj_simd_precision_t = rj_precision_t;

typedef struct rj_config_t {
    rj_profile_mode_t    profile;
    size_t               static_arena_size_bytes;
    uint32_t             max_loop_iterations;
    bool                 forbid_runtime_heap;
    void*                user_static_arena_buf;
} rj_config_t;
using msimd_config_t   = rj_config_t;
using rj_simd_config_t = rj_config_t;

typedef struct rj_cpu_info_t {
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
    rj_hw_tier_t    tier;
} rj_cpu_info_t;
using msimd_cpu_info_t   = rj_cpu_info_t;
using rj_simd_cpu_info_t = rj_cpu_info_t;

typedef struct rj_context_t {
    rj_cpu_info_t    cpu_info;
    rj_isa_t         isa;
    rj_backend_t     backend;
    void*            backend_handle;
    int              optimal_ladder_level;
    rj_config_t      config;
    uint8_t*         static_arena_base;
    size_t           static_arena_capacity;
    size_t           static_arena_offset;
    bool             arena_is_user_provided;
} rj_context_t;
using msimd_context_t   = rj_context_t;
using rj_simd_context_t = rj_context_t;
typedef rj_context_t* rj_context;
typedef rj_context_t* msimd_context;
typedef rj_context_t* rj_simd_context;

typedef struct rj_lattice_t {
    rj_context       ctx;
    int              dim;
    size_t           nx;
    size_t           ny;
    size_t           nz;
    size_t           total_elements;
    size_t           elem_size;
    rj_precision_t   precision;
    void*            data_prev;
    void*            data_curr;
    void*            data_next;
    bool             allocated_from_arena;
} rj_lattice_t;
using msimd_lattice_t   = rj_lattice_t;
using rj_simd_lattice_t = rj_lattice_t;
typedef rj_lattice_t* rj_lattice;
typedef rj_lattice_t* msimd_lattice;
typedef rj_lattice_t* rj_simd_lattice;
typedef HS_hotspot_tensor_t rj_tensor_t;
using msimd_tensor_t   = rj_tensor_t;
using rj_simd_tensor_t = rj_tensor_t;
typedef void (*rj_parallel_for_fn)(void* user_data, size_t start, size_t end);
typedef void (*msimd_parallel_for_fn)(void* user_data, size_t start, size_t end);
typedef void (*rj_simd_parallel_for_fn)(void* user_data, size_t start, size_t end);

/* ========================================================================= */
/* 2. C ABI EXPORTS FOR MATHEMATICAL & NUMERICAL EXECUTION                   */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

// 2.1 Cacheline/HugePage Aligned Allocators
RJ_API void* rj_aligned_alloc_cl(size_t size, size_t alignment);
RJ_API void  rj_aligned_free_cl(void* ptr);
RJ_API void* rj_alloc_aligned(size_t size, size_t alignment);
RJ_API void  rj_free_aligned(void* ptr);

RJ_API void* hs_aligned_alloc_cl(size_t size, size_t alignment);
RJ_API void  hs_aligned_free_cl(void* ptr);
RJ_API void* hs_alloc_aligned(size_t size, size_t alignment);
RJ_API void  hs_free_aligned(void* ptr);
RJ_API void* msimd_aligned_alloc_cl(size_t size, size_t alignment);
RJ_API void  msimd_aligned_free_cl(void* ptr);
RJ_API void* msimd_alloc_aligned(size_t size, size_t alignment);
RJ_API void  msimd_free_aligned(void* ptr);

// 2.2 Hardware Prefetchers, Non-Temporal Float/Double Stream Stores & Memory Fences
RJ_API void hs_prefetch_t0(const void* addr);
RJ_API void hs_prefetch_t1(const void* addr);
RJ_API void hs_prefetch_t2(const void* addr);
RJ_API void hs_prefetch_nta(const void* addr);
RJ_API void hs_prefetch_stream(const void* base, size_t count, size_t stride);
RJ_API void hs_stream_store_f32(float* dst, const float* src, size_t count);
RJ_API void hs_stream_store_f64(double* dst, const double* src, size_t count);
RJ_API void hs_hw_fence_full(void);
RJ_API void hs_hw_fence_store(void);
RJ_API void hs_hw_fence_load(void);
RJ_API void hs_memcpy_nt(void* dst, const void* src, size_t size);
RJ_API void hs_memzero_nt(void* dst, size_t size);

RJ_API void msimd_prefetch_t0(const void* addr);
RJ_API void msimd_prefetch_t1(const void* addr);
RJ_API void msimd_prefetch_t2(const void* addr);
RJ_API void msimd_prefetch_nta(const void* addr);
RJ_API void msimd_prefetch_stream(const void* base, size_t count, size_t stride);
RJ_API void msimd_stream_store_f32(float* dst, const float* src, size_t count);
RJ_API void msimd_stream_store_f64(double* dst, const double* src, size_t count);
RJ_API void msimd_hw_fence_full(void);
RJ_API void msimd_hw_fence_store(void);
RJ_API void msimd_hw_fence_load(void);
RJ_API void msimd_memcpy_nt(void* dst, const void* src, size_t size);
RJ_API void msimd_memzero_nt(void* dst, size_t size);

RJ_API void HS_hotspot_prefetch_t0(const void* addr);
RJ_API void HS_hotspot_prefetch_t1(const void* addr);
RJ_API void HS_hotspot_prefetch_t2(const void* addr);
RJ_API void HS_hotspot_prefetch_nta(const void* addr);
RJ_API void HS_hotspot_prefetch_stream(const void* base, size_t count, size_t stride);
RJ_API void HS_hotspot_stream_store_f32(float* dst, const float* src, size_t count);
RJ_API void HS_hotspot_stream_store_f64(double* dst, const double* src, size_t count);
RJ_API void HS_hotspot_fence_full(void);
RJ_API void HS_hotspot_fence_store(void);
RJ_API void HS_hotspot_fence_load(void);
RJ_API void HS_hotspot_memcpy_nt(void* dst, const void* src, size_t size);
RJ_API void HS_hotspot_memzero_nt(void* dst, size_t size);

// 2.3 RJ Engine Context, RDTSC GFLOPS Calibration & Asymmetric Hybrid parallel_for
RJ_API rj_status_t       rj_init_with_config(rj_context* out_ctx, rj_isa_t isa, rj_backend_t backend, const rj_config_t* config);
RJ_API rj_status_t       rj_init(rj_context* out_ctx, rj_isa_t isa, rj_backend_t backend);
RJ_API rj_profile_mode_t rj_get_active_profile(rj_context ctx);
RJ_API const rj_config_t* rj_get_config(rj_context ctx);
RJ_API void              rj_destroy(rj_context ctx);
RJ_API rj_status_t       rj_get_cpu_info(rj_context ctx, rj_cpu_info_t* out_info);
RJ_API rj_status_t       rj_calibrate_pipeline(rj_context ctx, int* out_best_level, double* out_gflops);
RJ_API void              rj_parallel_for(rj_context ctx, size_t total, rj_parallel_for_fn fn, void* user_data);

RJ_API msimd_status_t       hs_backend_fallback_init(msimd_context ctx);
RJ_API void                 hs_backend_fallback_destroy(msimd_context ctx);
RJ_API void                 hs_backend_fallback_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data);

RJ_API msimd_status_t       hs_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config);
RJ_API msimd_status_t       hs_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend);
RJ_API msimd_profile_mode_t hs_get_active_profile(msimd_context ctx);
RJ_API const msimd_config_t* hs_get_config(msimd_context ctx);
RJ_API void                 hs_destroy(msimd_context ctx);
RJ_API msimd_status_t       hs_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info);
RJ_API msimd_status_t       hs_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops);
RJ_API void                 hs_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data);

RJ_API msimd_status_t       msimd_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config);
RJ_API msimd_status_t       msimd_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend);
RJ_API msimd_profile_mode_t msimd_get_active_profile(msimd_context ctx);
RJ_API const msimd_config_t* msimd_get_config(msimd_context ctx);
RJ_API void                 msimd_destroy(msimd_context ctx);
RJ_API msimd_status_t       msimd_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info);
RJ_API msimd_status_t       msimd_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops);
RJ_API void                 msimd_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data);

RJ_API msimd_status_t       HS_hotspot_msimd_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config);
RJ_API msimd_status_t       HS_hotspot_msimd_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend);
RJ_API msimd_profile_mode_t HS_hotspot_msimd_get_profile(msimd_context ctx);
RJ_API const msimd_config_t* HS_hotspot_msimd_get_config(msimd_context ctx);
RJ_API void                 HS_hotspot_msimd_destroy(msimd_context ctx);
RJ_API msimd_status_t       HS_hotspot_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info);
RJ_API msimd_status_t       HS_hotspot_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops);
RJ_API void                 HS_hotspot_parallel_for(size_t total, msimd_parallel_for_fn fn, void* user_data);

// 2.4 3-Buffer Lattice Geometry & Stencil Memory Tensors
RJ_API msimd_status_t hs_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision);
RJ_API void           hs_lattice_destroy(msimd_lattice lat);
RJ_API void*          hs_lattice_get_curr(msimd_lattice lat);
RJ_API void*          hs_lattice_get_prev(msimd_lattice lat);
RJ_API void*          hs_lattice_get_next(msimd_lattice lat);
RJ_API void           hs_lattice_swap(msimd_lattice lat);

RJ_API msimd_status_t msimd_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision);
RJ_API void           msimd_lattice_destroy(msimd_lattice lat);
RJ_API void*          msimd_lattice_get_curr(msimd_lattice lat);
RJ_API void*          msimd_lattice_get_prev(msimd_lattice lat);
RJ_API void*          msimd_lattice_get_next(msimd_lattice lat);
RJ_API void           msimd_lattice_swap(msimd_lattice lat);

RJ_API msimd_status_t HS_hotspot_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision);
RJ_API void           HS_hotspot_lattice_destroy(msimd_lattice lat);
RJ_API void*          HS_hotspot_lattice_get_curr(msimd_lattice lat);
RJ_API void*          HS_hotspot_lattice_get_prev(msimd_lattice lat);
RJ_API void*          HS_hotspot_lattice_get_next(msimd_lattice lat);
RJ_API void           HS_hotspot_lattice_swap(msimd_lattice lat);

// 2.5 Hardware Coprocessors (Q31 CORDIC, Q31 FMAC FIR, TMU), Soft-Float ALU & XIP/ITCM
RJ_API void   hs_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* precision_target);
RJ_API void   hs_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* precision_target);
RJ_API void   hs_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* precision_target);
RJ_API float  hs_tmu_sin_f32(float rad, const msimd_tensor_t* precision_target);
RJ_API float  hs_tmu_cos_f32(float rad, const msimd_tensor_t* precision_target);
RJ_API float  hs_softfloat_add_f32(float a, float b, const msimd_tensor_t* precision_target);
RJ_API float  hs_softfloat_mul_f32(float a, float b, const msimd_tensor_t* precision_target);
RJ_API float  hs_softfloat_div_f32(float a, float b, const msimd_tensor_t* precision_target);
RJ_API double hs_softfloat_add_f64(double a, double b, const msimd_tensor_t* precision_target);
RJ_API double hs_softfloat_mul_f64(double a, double b, const msimd_tensor_t* precision_target);
RJ_API double hs_softfloat_div_f64(double a, double b, const msimd_tensor_t* precision_target);
RJ_API int    hs_xip_verify_flash(void);
RJ_API size_t hs_decompress_block_to_itcm(const void* src_compressed, size_t src_size, void* itcm_dest, size_t max_dest);

RJ_API void   msimd_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* p);
RJ_API void   msimd_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* p);
RJ_API void   msimd_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* p);
RJ_API float  msimd_tmu_sin_f32(float rad, const msimd_tensor_t* p);
RJ_API float  msimd_tmu_cos_f32(float rad, const msimd_tensor_t* p);
RJ_API float  msimd_softfloat_add_f32(float a, float b, const msimd_tensor_t* p);
RJ_API float  msimd_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p);
RJ_API float  msimd_softfloat_div_f32(float a, float b, const msimd_tensor_t* p);
RJ_API double msimd_softfloat_add_f64(double a, double b, const msimd_tensor_t* p);
RJ_API double msimd_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p);
RJ_API double msimd_softfloat_div_f64(double a, double b, const msimd_tensor_t* p);
RJ_API int    msimd_xip_verify_flash(void);
RJ_API size_t msimd_decompress_block_to_itcm(const void* src, size_t src_size, void* dest, size_t max_dest);

RJ_API void   HS_hotspot_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* p);
RJ_API void   HS_hotspot_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* p);
RJ_API void   HS_hotspot_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* p);
RJ_API float  HS_hotspot_tmu_sin_f32(float rad, const msimd_tensor_t* p);
RJ_API float  HS_hotspot_tmu_cos_f32(float rad, const msimd_tensor_t* p);
RJ_API float  HS_hotspot_softfloat_add_f32(float a, float b, const msimd_tensor_t* p);
RJ_API float  HS_hotspot_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p);
RJ_API float  HS_hotspot_softfloat_div_f32(float a, float b, const msimd_tensor_t* p);
RJ_API double HS_hotspot_softfloat_add_f64(double a, double b, const msimd_tensor_t* p);
RJ_API double HS_hotspot_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p);
RJ_API double HS_hotspot_softfloat_div_f64(double a, double b, const msimd_tensor_t* p);
RJ_API int    HS_hotspot_xip_verify_flash(void);
RJ_API size_t HS_hotspot_decompress_block_to_itcm(const void* src, size_t src_size, void* dest, size_t max_dest);

// 2.6 Primitiva Raw Sector R/W (SSIOLSC[PrimitiveRW] Header-Bypass Primitives)
RJ_API void* HS_primitiva_alloc(size_t bytes, size_t align);
RJ_API void  HS_primitiva_write(void* sector_ptr, const void* src, size_t bytes);
RJ_API void  HS_primitiva_read(const void* sector_ptr, void* dst, size_t bytes);
RJ_API void  HS_primitiva_overwrite(void* sector_ptr, const void* src, size_t bytes);
RJ_API void  HS_primitiva_zero(void* sector_ptr, size_t bytes);

// 2.7 Canonical rj_simd_* C ABI exports
RJ_API void* rj_simd_aligned_alloc_cl(size_t size, size_t alignment);
RJ_API void  rj_simd_aligned_free_cl(void* ptr);
RJ_API void* rj_simd_alloc_aligned(size_t size, size_t alignment);
RJ_API void  rj_simd_free_aligned(void* ptr);

RJ_API void rj_simd_prefetch_t0(const void* addr);
RJ_API void rj_simd_prefetch_t1(const void* addr);
RJ_API void rj_simd_prefetch_t2(const void* addr);
RJ_API void rj_simd_prefetch_nta(const void* addr);
RJ_API void rj_simd_prefetch_stream(const void* base, size_t count, size_t stride);
RJ_API void rj_simd_stream_store_f32(float* dst, const float* src, size_t count);
RJ_API void rj_simd_stream_store_f64(double* dst, const double* src, size_t count);
RJ_API void rj_simd_hw_fence_full(void);
RJ_API void rj_simd_hw_fence_store(void);
RJ_API void rj_simd_hw_fence_load(void);
RJ_API void rj_simd_memcpy_nt(void* dst, const void* src, size_t size);
RJ_API void rj_simd_memzero_nt(void* dst, size_t size);

RJ_API rj_simd_status_t       rj_simd_init_with_config(rj_simd_context* out_ctx, rj_simd_isa_t isa, rj_simd_backend_t backend, const rj_simd_config_t* config);
RJ_API rj_simd_status_t       rj_simd_init(rj_simd_context* out_ctx, rj_simd_isa_t isa, rj_simd_backend_t backend);
RJ_API rj_profile_mode_t      rj_simd_get_active_profile(rj_simd_context ctx);
RJ_API const rj_simd_config_t* rj_simd_get_config(rj_simd_context ctx);
RJ_API void                   rj_simd_destroy(rj_simd_context ctx);
RJ_API rj_simd_status_t       rj_simd_get_cpu_info(rj_simd_context ctx, rj_simd_cpu_info_t* out_info);
RJ_API rj_simd_status_t       rj_simd_calibrate_pipeline(rj_simd_context ctx, int* out_best_level, double* out_gflops);
RJ_API void                   rj_simd_parallel_for(rj_simd_context ctx, size_t total, rj_simd_parallel_for_fn fn, void* user_data);

RJ_API rj_simd_status_t rj_simd_lattice_create(rj_simd_context ctx, rj_simd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, rj_simd_precision_t precision);
RJ_API void             rj_simd_lattice_destroy(rj_simd_lattice lat);
RJ_API void*            rj_simd_lattice_get_curr(rj_simd_lattice lat);
RJ_API void*            rj_simd_lattice_get_prev(rj_simd_lattice lat);
RJ_API void*            rj_simd_lattice_get_next(rj_simd_lattice lat);
RJ_API void             rj_simd_lattice_swap(rj_simd_lattice lat);

RJ_API void   rj_simd_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const rj_simd_tensor_t* precision_target);
RJ_API void   rj_simd_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const rj_simd_tensor_t* precision_target);
RJ_API void   rj_simd_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const rj_simd_tensor_t* precision_target);
RJ_API float  rj_simd_tmu_sin_f32(float rad, const rj_simd_tensor_t* precision_target);
RJ_API float  rj_simd_tmu_cos_f32(float rad, const rj_simd_tensor_t* precision_target);
RJ_API float  rj_simd_softfloat_add_f32(float a, float b, const rj_simd_tensor_t* precision_target);
RJ_API float  rj_simd_softfloat_mul_f32(float a, float b, const rj_simd_tensor_t* precision_target);
RJ_API float  rj_simd_softfloat_div_f32(float a, float b, const rj_simd_tensor_t* precision_target);
RJ_API double rj_simd_softfloat_add_f64(double a, double b, const rj_simd_tensor_t* precision_target);
RJ_API double rj_simd_softfloat_mul_f64(double a, double b, const rj_simd_tensor_t* precision_target);
RJ_API double rj_simd_softfloat_div_f64(double a, double b, const rj_simd_tensor_t* precision_target);
RJ_API int    rj_simd_xip_verify_flash(void);
RJ_API size_t rj_simd_decompress_block_to_itcm(const void* src, size_t src_size, void* dest, size_t max_dest);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_REDJETMATHRJ_HPP
