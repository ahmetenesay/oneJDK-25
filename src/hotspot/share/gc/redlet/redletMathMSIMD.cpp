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

#include "gc/redlet/redletMathMSIMD.hpp"
#include "gc/redlet/redletIntentCLI.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletRunway.hpp"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <new>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#if defined(_MSC_VER) || (defined(__clang__) && defined(_WIN32))
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#ifndef SF_DISABLE_OPT_BEGIN
#if defined(__clang__)
#define SF_DISABLE_OPT_BEGIN _Pragma("clang optimize off")
#define SF_DISABLE_OPT_END   _Pragma("clang optimize on")
#define SF_COMPILER_BARRIER(p) __asm__ __volatile__("" : : "r"(p) : "memory")
#elif defined(__GNUC__)
#define SF_DISABLE_OPT_BEGIN _Pragma("GCC push_options") _Pragma("GCC optimize(\"O0\")")
#define SF_DISABLE_OPT_END   _Pragma("GCC pop_options")
#define SF_COMPILER_BARRIER(p) __asm__ __volatile__("" : : "r"(p) : "memory")
#elif defined(_MSC_VER)
#define SF_DISABLE_OPT_BEGIN __pragma(optimize("", off))
#define SF_DISABLE_OPT_END   __pragma(optimize("", on))
#define SF_COMPILER_BARRIER(p) do { (void)(p); _ReadWriteBarrier(); } while (0)
#else
#define SF_DISABLE_OPT_BEGIN
#define SF_DISABLE_OPT_END
#define SF_COMPILER_BARRIER(p) ((void)(p))
#endif
#endif

namespace {

static inline void sf_math_cpuid(int info[4], int leaf, int subleaf = 0) noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#if defined(_MSC_VER) || (defined(__clang__) && defined(_WIN32))
    __cpuidex(info, leaf, subleaf);
#else
    __cpuid_count(leaf, subleaf, info[0], info[1], info[2], info[3]);
#endif
#else
    info[0] = info[1] = info[2] = info[3] = 0;
#endif
}

static void* sf_context_alloc_aligned(msimd_context ctx, size_t size, size_t alignment) noexcept {
    if (!ctx || size == 0) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) alignment = 64;

    if (ctx->config.profile == MSIMD_PROFILE_SAFETY_CRITICAL || ctx->config.forbid_runtime_heap) {
        if (!ctx->static_arena_base) return nullptr;
        size_t current_addr = reinterpret_cast<size_t>(ctx->static_arena_base + ctx->static_arena_offset);
        size_t aligned_addr = (current_addr + (alignment - 1)) & ~(alignment - 1);
        size_t new_offset   = (aligned_addr - reinterpret_cast<size_t>(ctx->static_arena_base)) + size;
        if (new_offset > ctx->static_arena_capacity) {
            return nullptr;
        }
        ctx->static_arena_offset = new_offset;
        return reinterpret_cast<void*>(aligned_addr);
    }

    return sf_alloc_aligned(size, alignment);
}

static void sf_context_free_aligned(msimd_context ctx, void* ptr, bool from_arena) noexcept {
    if (!ptr) return;
    if (from_arena || (ctx && (ctx->config.profile == MSIMD_PROFILE_SAFETY_CRITICAL || ctx->config.forbid_runtime_heap))) {
        return;
    }
    sf_free_aligned(ptr);
}

static const int32_t g_cordic_angles_q31[24] = {
    536870912, 316933406, 167458907, 85004756, 42667357, 21354465,
    10679838,  5340245,   2670163,   1335086,  667544,   333772,
    166886,    83443,     41722,     20861,    10430,    5215,
    2608,      1304,      652,       326,      163,      81
};
static constexpr int32_t CORDIC_GAIN_Q31 = 1304065792;

} // namespace

extern "C" {

/* ========================================================================= */
/* 1. CACHELINE / HUGEPAGE ALIGNED ALLOCATORS                                */
/* ========================================================================= */

MSIMD_API void* sf_aligned_alloc_cl(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) alignment = 64;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);

#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (size >= 2 * 1024 * 1024) {
        madvise(ptr, size, MADV_HUGEPAGE);
    }
#endif
    return ptr;
#endif
}

MSIMD_API void sf_aligned_free_cl(void* ptr) {
    if (!ptr) return;
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

MSIMD_API void* sf_alloc_aligned(size_t size, size_t alignment)    { return sf_aligned_alloc_cl(size, alignment); }
MSIMD_API void  sf_free_aligned(void* ptr)                         { sf_aligned_free_cl(ptr); }
MSIMD_API void* msimd_aligned_alloc_cl(size_t size, size_t align)  { return sf_aligned_alloc_cl(size, align); }
MSIMD_API void  msimd_aligned_free_cl(void* ptr)                   { sf_aligned_free_cl(ptr); }
MSIMD_API void* msimd_alloc_aligned(size_t size, size_t align)     { return sf_aligned_alloc_cl(size, align); }
MSIMD_API void  msimd_free_aligned(void* ptr)                      { sf_aligned_free_cl(ptr); }

/* ========================================================================= */
/* 2. PREFETCHERS, NON-TEMPORAL FLOAT/DOUBLE STREAM STORES & MEMORY FENCES   */
/* ========================================================================= */

MSIMD_API void sf_prefetch_t0(const void* addr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, 3);
#elif defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T0);
#else
    (void)addr;
#endif
}

MSIMD_API void sf_prefetch_t1(const void* addr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, 2);
#elif defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T1);
#else
    (void)addr;
#endif
}

MSIMD_API void sf_prefetch_t2(const void* addr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, 1);
#elif defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T2);
#else
    (void)addr;
#endif
}

MSIMD_API void sf_prefetch_nta(const void* addr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, 0);
#elif defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_NTA);
#else
    (void)addr;
#endif
}

MSIMD_API void sf_prefetch_stream(const void* base, size_t count, size_t stride) {
    if (!base) return;
    const char* ptr = static_cast<const char*>(base);
    for (size_t i = 0; i < count; ++i) {
        sf_prefetch_nta(ptr + (i * stride));
    }
}

MSIMD_API void sf_stream_store_f32(float* dst, const float* src, size_t count) {
    if (!dst || !src || count == 0) return;
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::memcpy(dst, src, count * sizeof(float));
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    while (i < count && (reinterpret_cast<uintptr_t>(dst + i) & 15) != 0) {
        dst[i] = src[i];
        ++i;
    }
#if defined(__AVX512F__)
    while (i + 16 <= count && (reinterpret_cast<uintptr_t>(dst + i) & 63) == 0) {
        __m512 v = _mm512_loadu_ps(src + i);
        _mm512_stream_ps(dst + i, v);
        i += 16;
    }
#endif
#if defined(__AVX__)
    while (i + 8 <= count && (reinterpret_cast<uintptr_t>(dst + i) & 31) == 0) {
        __m256 v = _mm256_loadu_ps(src + i);
        _mm256_stream_ps(dst + i, v);
        i += 8;
    }
#endif
    for (; i + 4 <= count; i += 4) {
        __m128 v = _mm_loadu_ps(src + i);
        _mm_stream_ps(dst + i, v);
    }
    _mm_sfence();
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))
    while (i < count && (reinterpret_cast<uintptr_t>(dst + i) & 15) != 0) {
        dst[i] = src[i];
        ++i;
    }
    for (; i + 8 <= count; i += 8) {
        __asm__ __volatile__(
            "ldp q0, q1, [%1]\n\t"
            "stnp q0, q1, [%0]\n\t"
            :
            : "r"(dst + i), "r"(src + i)
            : "v0", "v1", "memory"
        );
    }
    __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
    for (; i < count; ++i) {
        dst[i] = src[i];
    }
}

MSIMD_API void sf_stream_store_f64(double* dst, const double* src, size_t count) {
    if (!dst || !src || count == 0) return;
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::memcpy(dst, src, count * sizeof(double));
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    while (i < count && (reinterpret_cast<uintptr_t>(dst + i) & 15) != 0) {
        dst[i] = src[i];
        ++i;
    }
#if defined(__AVX512F__)
    while (i + 8 <= count && (reinterpret_cast<uintptr_t>(dst + i) & 63) == 0) {
        __m512d v = _mm512_loadu_pd(src + i);
        _mm512_stream_pd(dst + i, v);
        i += 8;
    }
#endif
#if defined(__AVX__)
    while (i + 4 <= count && (reinterpret_cast<uintptr_t>(dst + i) & 31) == 0) {
        __m256d v = _mm256_loadu_pd(src + i);
        _mm256_stream_pd(dst + i, v);
        i += 4;
    }
#endif
    for (; i + 2 <= count; i += 2) {
        __m128d v = _mm_loadu_pd(src + i);
        _mm_stream_pd(dst + i, v);
    }
    _mm_sfence();
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))
    while (i < count && (reinterpret_cast<uintptr_t>(dst + i) & 15) != 0) {
        dst[i] = src[i];
        ++i;
    }
    for (; i + 4 <= count; i += 4) {
        __asm__ __volatile__(
            "ldp q0, q1, [%1]\n\t"
            "stnp q0, q1, [%0]\n\t"
            :
            : "r"(dst + i), "r"(src + i)
            : "v0", "v1", "memory"
        );
    }
    __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
    for (; i < count; ++i) {
        dst[i] = src[i];
    }
}

MSIMD_API void sf_hw_fence_full(void) {
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }
#if defined(_M_X64) || defined(__x86_64__)
    _mm_mfence();
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))
    __asm__ __volatile__("dmb sy" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

MSIMD_API void sf_hw_fence_store(void) {
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::atomic_thread_fence(std::memory_order_release);
        return;
    }
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))
    __asm__ __volatile__("dmb st" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
}

MSIMD_API void sf_hw_fence_load(void) {
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::atomic_thread_fence(std::memory_order_acquire);
        return;
    }
#if defined(_M_X64) || defined(__x86_64__)
    _mm_lfence();
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))
    __asm__ __volatile__("dmb ld" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

MSIMD_API void sf_memcpy_nt(void* dst, const void* src, size_t size) {
    if (!dst || !src || size == 0) return;
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::memcpy(dst, src, size);
        return;
    }
    if (size >= 256) {
        size_t count_f32 = size / sizeof(float);
        sf_stream_store_f32(static_cast<float*>(dst), static_cast<const float*>(src), count_f32);
        size_t rem = size % sizeof(float);
        if (rem > 0) {
            std::memcpy(static_cast<char*>(dst) + (size - rem),
                        static_cast<const char*>(src) + (size - rem), rem);
        }
    } else {
        std::memcpy(dst, src, size);
    }
}

SF_DISABLE_OPT_BEGIN
MSIMD_API void sf_memzero_nt(void* dst, size_t size) {
    if (!dst || size == 0) return;
    if (!SF_custominiVm_is_iasm_active(0)) {
        std::memset(dst, 0, size);
        SF_COMPILER_BARRIER(dst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return;
    }
    uint8_t* d = static_cast<uint8_t*>(dst);
    size_t rem = size;
#if defined(_M_X64) || defined(__x86_64__)
    while (rem > 0 && (reinterpret_cast<uintptr_t>(d) & 15) != 0) {
        *d++ = 0;
        --rem;
    }
#if defined(__AVX512F__)
    if (rem >= 64) {
        while (rem > 0 && (reinterpret_cast<uintptr_t>(d) & 63) != 0) {
            *d++ = 0;
            --rem;
        }
        __m512i zero512 = _mm512_setzero_si512();
        while (rem >= 64) {
            _mm512_stream_si512(reinterpret_cast<__m512i*>(d), zero512);
            d += 64;
            rem -= 64;
        }
    }
#elif defined(__AVX2__)
    if (rem >= 32) {
        while (rem > 0 && (reinterpret_cast<uintptr_t>(d) & 31) != 0) {
            *d++ = 0;
            --rem;
        }
        __m256i zero256 = _mm256_setzero_si256();
        while (rem >= 32) {
            _mm256_stream_si256(reinterpret_cast<__m256i*>(d), zero256);
            d += 32;
            rem -= 32;
        }
    }
#endif
    __m128i zero128 = _mm_setzero_si128();
    while (rem >= 16) {
        _mm_stream_si128(reinterpret_cast<__m128i*>(d), zero128);
        d += 16;
        rem -= 16;
    }
    _mm_sfence();
#endif
    if (rem > 0) {
        std::memset(d, 0, rem);
    }
    SF_COMPILER_BARRIER(dst);
}
SF_DISABLE_OPT_END

MSIMD_API void msimd_prefetch_t0(const void* addr)                              { sf_prefetch_t0(addr); }
MSIMD_API void msimd_prefetch_t1(const void* addr)                              { sf_prefetch_t1(addr); }
MSIMD_API void msimd_prefetch_t2(const void* addr)                              { sf_prefetch_t2(addr); }
MSIMD_API void msimd_prefetch_nta(const void* addr)                             { sf_prefetch_nta(addr); }
MSIMD_API void msimd_prefetch_stream(const void* base, size_t cnt, size_t str)  { sf_prefetch_stream(base, cnt, str); }
MSIMD_API void msimd_stream_store_f32(float* dst, const float* src, size_t cnt) { sf_stream_store_f32(dst, src, cnt); }
MSIMD_API void msimd_stream_store_f64(double* dst, const double* src, size_t c) { sf_stream_store_f64(dst, src, c); }
MSIMD_API void msimd_hw_fence_full(void)                                        { sf_hw_fence_full(); }
MSIMD_API void msimd_hw_fence_store(void)                                       { sf_hw_fence_store(); }
MSIMD_API void msimd_hw_fence_load(void)                                        { sf_hw_fence_load(); }
MSIMD_API void msimd_memcpy_nt(void* dst, const void* src, size_t size)         { sf_memcpy_nt(dst, src, size); }
MSIMD_API void msimd_memzero_nt(void* dst, size_t size)                         { sf_memzero_nt(dst, size); }

MSIMD_API void SF_custominiVm_prefetch_t0(const void* addr)                              { sf_prefetch_t0(addr); }
MSIMD_API void SF_custominiVm_prefetch_t1(const void* addr)                              { sf_prefetch_t1(addr); }
MSIMD_API void SF_custominiVm_prefetch_t2(const void* addr)                              { sf_prefetch_t2(addr); }
MSIMD_API void SF_custominiVm_prefetch_nta(const void* addr)                             { sf_prefetch_nta(addr); }
MSIMD_API void SF_custominiVm_prefetch_stream(const void* base, size_t cnt, size_t str)  { sf_prefetch_stream(base, cnt, str); }
MSIMD_API void SF_custominiVm_stream_store_f32(float* dst, const float* src, size_t cnt) { sf_stream_store_f32(dst, src, cnt); }
MSIMD_API void SF_custominiVm_stream_store_f64(double* dst, const double* src, size_t c) { sf_stream_store_f64(dst, src, c); }
MSIMD_API void SF_custominiVm_fence_full(void)                                           { sf_hw_fence_full(); }
MSIMD_API void SF_custominiVm_fence_store(void)                                          { sf_hw_fence_store(); }
MSIMD_API void SF_custominiVm_fence_load(void)                                           { sf_hw_fence_load(); }
MSIMD_API void SF_custominiVm_memcpy_nt(void* dst, const void* src, size_t size)         { sf_memcpy_nt(dst, src, size); }
MSIMD_API void SF_custominiVm_memzero_nt(void* dst, size_t size)                         { sf_memzero_nt(dst, size); }

/* ========================================================================= */
/* 3. MSIMD ENGINE CONTEXT, CALIBRATION & ASYMMETRIC PARALLEL_FOR            */
/* ========================================================================= */

MSIMD_API msimd_status_t sf_backend_fallback_init(msimd_context ctx) {
    if (!ctx) return MSIMD_ERROR_INVALID_ARGUMENT;
    auto* nexus = sf_get_hardware_nexus();
    int threads = 4;
    if (nexus) {
        auto budget = nexus->smt.calculate_thread_budget();
        if (budget.vm_threads > 0) threads = static_cast<int>(budget.vm_threads);
    }
    if (threads <= 0 && ctx->cpu_info.num_physical_cores > 0) {
        threads = ctx->cpu_info.num_physical_cores;
    }
    if (threads <= 0) threads = 4;

    sf_fallback_pool_t* pool = new (std::nothrow) sf_fallback_pool_t(threads);
    if (!pool) return MSIMD_ERROR_OUT_OF_MEMORY;
    ctx->backend_handle = pool;
    return MSIMD_SUCCESS;
}

MSIMD_API void sf_backend_fallback_destroy(msimd_context ctx) {
    if (!ctx || !ctx->backend_handle) return;
    delete static_cast<sf_fallback_pool_t*>(ctx->backend_handle);
    ctx->backend_handle = nullptr;
}

MSIMD_API void sf_backend_fallback_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data) {
    if (!ctx || total == 0 || !fn) return;
    auto* nexus = sf_get_hardware_nexus();
    if (nexus && nexus->focus.is_scp) {
        fn(user_data, 0, total);
        return;
    }

    auto* pool = static_cast<sf_fallback_pool_t*>(ctx->backend_handle);
    if (!pool || pool->thread_count <= 1 || total <= 256) {
        fn(user_data, 0, total);
        return;
    }

    int nthreads = pool->thread_count;
    if (nexus && nexus->focus.is_hybrid && nexus->focus.p_weight > 1.0) {
        uint32_t p_count = nexus->focus.p_cores;
        if (p_count == 0) p_count = static_cast<uint32_t>(nthreads / 2);
        if (p_count > static_cast<uint32_t>(nthreads)) p_count = static_cast<uint32_t>(nthreads);
        uint32_t e_count = static_cast<uint32_t>(nthreads) > p_count ? (static_cast<uint32_t>(nthreads) - p_count) : 0;

        double p_w = nexus->focus.p_weight;
        double e_w = nexus->focus.e_weight;
        double total_weight = (p_count * p_w) + (e_count * e_w);
        if (total_weight <= 0.0) total_weight = static_cast<double>(nthreads);

        size_t cur = 0;
        for (int t = 0; t < nthreads; ++t) {
            if (cur >= total) break;
            double w = (t < static_cast<int>(p_count)) ? p_w : e_w;
            size_t c_size = static_cast<size_t>(static_cast<double>(total) * (w / total_weight) + 0.5);
            if (c_size == 0) c_size = 1;
            size_t start = cur;
            size_t end = (std::min)(start + c_size, total);
            if (t == nthreads - 1) end = total;
            cur = end;

            pool->submit_to_worker(t, [fn, user_data, start, end]() {
                fn(user_data, start, end);
            });
        }
    } else {
        size_t chunk_size = (total + static_cast<size_t>(nthreads) - 1) / static_cast<size_t>(nthreads);
        for (int t = 0; t < nthreads; ++t) {
            size_t start = static_cast<size_t>(t) * chunk_size;
            if (start >= total) break;
            size_t end = (std::min)(start + chunk_size, total);
            pool->submit_to_worker(t, [fn, user_data, start, end]() {
                fn(user_data, start, end);
            });
        }
    }
    pool->wait_all();
}

MSIMD_API msimd_status_t sf_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend, const msimd_config_t* config) {
    if (!out_ctx) return MSIMD_ERROR_INVALID_ARGUMENT;

    msimd_config_t effective_config{};
    if (config) {
        effective_config = *config;
    } else {
        effective_config.profile                 = MSIMD_PROFILE_STANDARD;
        effective_config.static_arena_size_bytes = 16ULL * 1024 * 1024;
        effective_config.max_loop_iterations     = 0;
        effective_config.forbid_runtime_heap     = false;
        effective_config.user_static_arena_buf   = nullptr;
    }

    const char* env_sc = std::getenv("MSIMD_PROFILE");
    if (!env_sc) env_sc = std::getenv("SFVM_SC");
    if (env_sc && (std::strcmp(env_sc, "SC") == 0 || std::strcmp(env_sc, "SAFETY_CRITICAL") == 0 || std::strcmp(env_sc, "1") == 0)) {
        effective_config.profile = MSIMD_PROFILE_SAFETY_CRITICAL;
    }

    if (effective_config.profile == MSIMD_PROFILE_SAFETY_CRITICAL) {
        effective_config.forbid_runtime_heap = true;
        if (effective_config.max_loop_iterations == 0) {
            effective_config.max_loop_iterations = 1000000;
        }
        if (effective_config.static_arena_size_bytes == 0 && !effective_config.user_static_arena_buf) {
            effective_config.static_arena_size_bytes = 16ULL * 1024 * 1024;
        }
    }

    msimd_context ctx = new (std::nothrow) msimd_context_t();
    if (!ctx) return MSIMD_ERROR_OUT_OF_MEMORY;
    std::memset(ctx, 0, sizeof(*ctx));
    ctx->config  = effective_config;
    ctx->isa     = isa;
    ctx->backend = backend;

    if (ctx->config.profile == MSIMD_PROFILE_SAFETY_CRITICAL) {
        if (ctx->config.user_static_arena_buf) {
            ctx->static_arena_base      = static_cast<uint8_t*>(ctx->config.user_static_arena_buf);
            ctx->static_arena_capacity  = ctx->config.static_arena_size_bytes;
            ctx->arena_is_user_provided = true;
        } else if (ctx->config.static_arena_size_bytes > 0) {
            ctx->static_arena_base = static_cast<uint8_t*>(sf_alloc_aligned(ctx->config.static_arena_size_bytes, 64));
            if (!ctx->static_arena_base) {
                delete ctx;
                return MSIMD_ERROR_OUT_OF_MEMORY;
            }
            ctx->static_arena_capacity  = ctx->config.static_arena_size_bytes;
            ctx->arena_is_user_provided = false;
        }
    }

    auto* nexus = sf_get_hardware_nexus();
    if (nexus) {
        ctx->cpu_info.num_physical_cores = static_cast<int>(nexus->topology.physical_cores);
        ctx->cpu_info.num_logical_cores  = static_cast<int>(nexus->topology.logical_cores);
        ctx->cpu_info.is_numa            = (nexus->topology.numa_nodes > 1) ? 1 : 0;
        ctx->cpu_info.is_hybrid          = nexus->topology.is_heterogeneous ? 1 : 0;
        ctx->cpu_info.l1_data_kb         = static_cast<int>(nexus->topology.l1d.size_kb);
        ctx->cpu_info.l2_kb              = static_cast<int>(nexus->topology.l2.size_kb);
        ctx->cpu_info.l3_mb              = static_cast<int>(nexus->topology.l3.size_kb / 1024);
    }

#if defined(_M_X64) || defined(__x86_64__)
    int regs[4] = { 0 };
    sf_math_cpuid(regs, 0);
    std::memcpy(&ctx->cpu_info.vendor[0], &regs[1], 4);
    std::memcpy(&ctx->cpu_info.vendor[4], &regs[3], 4);
    std::memcpy(&ctx->cpu_info.vendor[8], &regs[2], 4);
    ctx->cpu_info.vendor[12] = '\0';

    sf_math_cpuid(regs, 1);
    ctx->cpu_info.has_fma = (regs[2] & (1 << 12)) ? 1 : 0;

    sf_math_cpuid(regs, 7, 0);
    ctx->cpu_info.has_avx2        = (regs[1] & (1 << 5))  ? 1 : 0;
    ctx->cpu_info.has_avx512      = (regs[1] & (1 << 16)) ? 1 : 0;
    ctx->cpu_info.has_avx512_vnni = (regs[2] & (1 << 11)) ? 1 : 0;
    ctx->cpu_info.has_amx         = (regs[3] & (1 << 24)) ? 1 : 0;

    sf_math_cpuid(regs, 7, 1);
    ctx->cpu_info.has_avx_vnni    = (regs[0] & (1 << 4)) ? 1 : 0;
    ctx->cpu_info.has_avx512_bf16 = (regs[0] & (1 << 5)) ? 1 : 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
    std::snprintf(ctx->cpu_info.vendor, sizeof(ctx->cpu_info.vendor), "ARM");
#if defined(__ARM_FEATURE_SVE)
    ctx->cpu_info.has_sve = 1;
#endif
#if defined(__ARM_FEATURE_SVE2)
    ctx->cpu_info.has_sve2 = 1;
#endif
#if defined(__ARM_FEATURE_SME)
    ctx->cpu_info.has_sme = 1;
#endif
#endif

    if (ctx->cpu_info.has_amx || ctx->cpu_info.has_sme) {
        ctx->cpu_info.tier = MSIMD_TIER_0_MATRIX_TILES;
    } else if (ctx->cpu_info.has_avx512 || ctx->cpu_info.has_sve2 || ctx->cpu_info.has_lasx || ctx->cpu_info.has_rvv) {
        ctx->cpu_info.tier = MSIMD_TIER_1_WIDE_VECTOR;
    } else if (ctx->cpu_info.has_avx2 || ctx->cpu_info.has_sve) {
        ctx->cpu_info.tier = MSIMD_TIER_2_BASIC_SIMD;
    } else {
        ctx->cpu_info.tier = MSIMD_TIER_4_SCALAR_FPU;
    }

    msimd_status_t bstatus = sf_backend_fallback_init(ctx);
    ctx->backend = MSIMD_BACKEND_FALLBACK;
    if (bstatus != MSIMD_SUCCESS) {
        if (!ctx->arena_is_user_provided && ctx->static_arena_base) {
            sf_free_aligned(ctx->static_arena_base);
        }
        delete ctx;
        return bstatus;
    }

    *out_ctx = ctx;
    return MSIMD_SUCCESS;
}

MSIMD_API msimd_status_t sf_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t backend) {
    return sf_init_with_config(out_ctx, isa, backend, nullptr);
}

MSIMD_API msimd_profile_mode_t sf_get_active_profile(msimd_context ctx) {
    return ctx ? ctx->config.profile : MSIMD_PROFILE_STANDARD;
}

MSIMD_API const msimd_config_t* sf_get_config(msimd_context ctx) {
    return ctx ? &ctx->config : nullptr;
}

MSIMD_API void sf_destroy(msimd_context ctx) {
    if (!ctx) return;
    sf_backend_fallback_destroy(ctx);
    if (!ctx->arena_is_user_provided && ctx->static_arena_base) {
        sf_free_aligned(ctx->static_arena_base);
        ctx->static_arena_base = nullptr;
    }
    delete ctx;
}

MSIMD_API msimd_status_t sf_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info) {
    if (!ctx || !out_info) return MSIMD_ERROR_INVALID_ARGUMENT;
    *out_info = ctx->cpu_info;
    return MSIMD_SUCCESS;
}

SF_DISABLE_OPT_BEGIN
MSIMD_API msimd_status_t sf_calibrate_pipeline(msimd_context ctx, int* out_best_level, double* out_gflops) {
    if (!ctx) return MSIMD_ERROR_INVALID_ARGUMENT;

    alignas(64) float a[64], b[64], c[64];
    for (int i = 0; i < 64; ++i) {
        a[i] = 1.001f * static_cast<float>(i + 1);
        b[i] = 0.999f * static_cast<float>(i + 1);
        c[i] = 0.0f;
    }

    uint64_t t0 = sf_rdtsc_begin();
    const size_t iters = 25000;
    for (size_t iter = 0; iter < iters; ++iter) {
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
        __m256 va = _mm256_load_ps(&a[0]);
        __m256 vb = _mm256_load_ps(&b[0]);
        __m256 vc = _mm256_load_ps(&c[0]);
        vc = _mm256_add_ps(_mm256_mul_ps(va, vb), vc);
        _mm256_store_ps(&c[0], vc);
#else
        for (int j = 0; j < 8; ++j) c[j] += a[j] * b[j];
#endif
        SF_COMPILER_BARRIER(c);
    }
    uint64_t t1 = sf_rdtsc_end();

    uint64_t freq = sf_rdtsc_freq_hz();
    double elapsed_s = (freq > 0 && t1 > t0) ? (static_cast<double>(t1 - t0) / static_cast<double>(freq)) : 1e-6;
    if (elapsed_s <= 0.0) elapsed_s = 1e-6;

    double total_flops     = static_cast<double>(iters) * 16.0;
    double measured_gflops = (total_flops / elapsed_s) / 1e9;
    if (measured_gflops < 1.0) measured_gflops = 64.0;

    int best_lvl = ctx->cpu_info.has_avx512 ? 0 : (ctx->cpu_info.has_avx2 ? 1 : 2);
    ctx->optimal_ladder_level = best_lvl;
    if (out_best_level) *out_best_level = best_lvl;
    if (out_gflops)     *out_gflops     = measured_gflops;
    return MSIMD_SUCCESS;
}
SF_DISABLE_OPT_END

MSIMD_API void sf_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data) {
    if (total == 0 || !fn) return;
    if (ctx) {
        sf_backend_fallback_parallel_for(ctx, total, fn, user_data);
        return;
    }

    auto* nexus = sf_get_hardware_nexus();
    if (nexus && nexus->focus.is_scp) {
        fn(user_data, 0, total);
        return;
    }

    static sf_fallback_pool_t s_default_pool(0);
    int nthreads = s_default_pool.thread_count;
    if (nthreads <= 1 || total <= 256) {
        fn(user_data, 0, total);
        return;
    }

    size_t chunk_size = (total + static_cast<size_t>(nthreads) - 1) / static_cast<size_t>(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        size_t start = static_cast<size_t>(t) * chunk_size;
        if (start >= total) break;
        size_t end = (std::min)(start + chunk_size, total);
        s_default_pool.submit_to_worker(t, [fn, user_data, start, end]() {
            fn(user_data, start, end);
        });
    }
    s_default_pool.wait_all();
}

MSIMD_API msimd_status_t       msimd_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t b, const msimd_config_t* cfg) { return sf_init_with_config(out_ctx, isa, b, cfg); }
MSIMD_API msimd_status_t       msimd_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t b)                                        { return sf_init(out_ctx, isa, b); }
MSIMD_API msimd_profile_mode_t msimd_get_active_profile(msimd_context ctx)                                                                   { return sf_get_active_profile(ctx); }
MSIMD_API const msimd_config_t* msimd_get_config(msimd_context ctx)                                                                          { return sf_get_config(ctx); }
MSIMD_API void                 msimd_destroy(msimd_context ctx)                                                                              { sf_destroy(ctx); }
MSIMD_API msimd_status_t       msimd_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info)                                             { return sf_get_cpu_info(ctx, out_info); }
MSIMD_API msimd_status_t       msimd_calibrate_pipeline(msimd_context ctx, int* out_lvl, double* out_gflops)                                 { return sf_calibrate_pipeline(ctx, out_lvl, out_gflops); }
MSIMD_API void                 msimd_parallel_for(msimd_context ctx, size_t total, msimd_parallel_for_fn fn, void* user_data)                { sf_parallel_for(ctx, total, fn, user_data); }

MSIMD_API msimd_status_t       SF_custominiVm_msimd_init_with_config(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t b, const msimd_config_t* cfg) { return sf_init_with_config(out_ctx, isa, b, cfg); }
MSIMD_API msimd_status_t       SF_custominiVm_msimd_init(msimd_context* out_ctx, msimd_isa_t isa, msimd_backend_t b)                                        { return sf_init(out_ctx, isa, b); }
MSIMD_API msimd_profile_mode_t SF_custominiVm_msimd_get_profile(msimd_context ctx)                                                                          { return sf_get_active_profile(ctx); }
MSIMD_API const msimd_config_t* SF_custominiVm_msimd_get_config(msimd_context ctx)                                                                          { return sf_get_config(ctx); }
MSIMD_API void                 SF_custominiVm_msimd_destroy(msimd_context ctx)                                                                              { sf_destroy(ctx); }
MSIMD_API msimd_status_t       SF_custominiVm_get_cpu_info(msimd_context ctx, msimd_cpu_info_t* out_info)                                                   { return sf_get_cpu_info(ctx, out_info); }
MSIMD_API msimd_status_t       SF_custominiVm_calibrate_pipeline(msimd_context ctx, int* out_lvl, double* out_gflops)                                       { return sf_calibrate_pipeline(ctx, out_lvl, out_gflops); }
MSIMD_API void                 SF_custominiVm_parallel_for(size_t total, msimd_parallel_for_fn fn, void* user_data)                                         { sf_parallel_for(nullptr, total, fn, user_data); }

/* ========================================================================= */
/* 4. 3-BUFFER LATTICE GEOMETRY & STENCIL MEMORY TENSORS                     */
/* ========================================================================= */

MSIMD_API msimd_status_t sf_lattice_create(msimd_context ctx, msimd_lattice* out_lattice, int dim, size_t nx, size_t ny, size_t nz, msimd_precision_t precision) {
    if (!ctx || !out_lattice || nx == 0) return MSIMD_ERROR_INVALID_ARGUMENT;

    bool is_sc = (ctx->config.profile == MSIMD_PROFILE_SAFETY_CRITICAL || ctx->config.forbid_runtime_heap);
    msimd_lattice lat = nullptr;
    if (is_sc) {
        lat = static_cast<msimd_lattice>(sf_context_alloc_aligned(ctx, sizeof(msimd_lattice_t), alignof(msimd_lattice_t)));
        if (lat) {
            std::memset(lat, 0, sizeof(msimd_lattice_t));
            lat->allocated_from_arena = true;
        }
    } else {
        lat = new (std::nothrow) msimd_lattice_t();
        if (lat) {
            std::memset(lat, 0, sizeof(msimd_lattice_t));
            lat->allocated_from_arena = false;
        }
    }
    if (!lat) return MSIMD_ERROR_OUT_OF_MEMORY;

    lat->ctx            = ctx;
    lat->dim            = dim;
    lat->nx             = nx;
    lat->ny             = (dim >= 2 && ny > 0) ? ny : 1;
    lat->nz             = (dim >= 3 && nz > 0) ? nz : 1;
    lat->total_elements = lat->nx * lat->ny * lat->nz;
    lat->precision      = precision;
    lat->elem_size      = (precision == MSIMD_PRECISION_FP64) ? sizeof(double) : sizeof(float);
    size_t total_bytes  = lat->total_elements * lat->elem_size;

    lat->data_prev = sf_context_alloc_aligned(ctx, total_bytes, 64);
    lat->data_curr = sf_context_alloc_aligned(ctx, total_bytes, 64);
    lat->data_next = sf_context_alloc_aligned(ctx, total_bytes, 64);

    if (!lat->data_prev || !lat->data_curr || !lat->data_next) {
        sf_context_free_aligned(ctx, lat->data_prev, lat->allocated_from_arena);
        sf_context_free_aligned(ctx, lat->data_curr, lat->allocated_from_arena);
        sf_context_free_aligned(ctx, lat->data_next, lat->allocated_from_arena);
        if (!lat->allocated_from_arena) delete lat;
        return MSIMD_ERROR_OUT_OF_MEMORY;
    }

    std::memset(lat->data_prev, 0, total_bytes);
    std::memset(lat->data_curr, 0, total_bytes);
    std::memset(lat->data_next, 0, total_bytes);

    *out_lattice = lat;
    return MSIMD_SUCCESS;
}

MSIMD_API void sf_lattice_destroy(msimd_lattice lat) {
    if (!lat) return;
    msimd_context ctx = lat->ctx;
    bool from_arena   = lat->allocated_from_arena;
    sf_context_free_aligned(ctx, lat->data_prev, from_arena);
    sf_context_free_aligned(ctx, lat->data_curr, from_arena);
    sf_context_free_aligned(ctx, lat->data_next, from_arena);
    if (!from_arena) {
        delete lat;
    }
}

MSIMD_API void* sf_lattice_get_curr(msimd_lattice lat) { return lat ? lat->data_curr : nullptr; }
MSIMD_API void* sf_lattice_get_prev(msimd_lattice lat) { return lat ? lat->data_prev : nullptr; }
MSIMD_API void* sf_lattice_get_next(msimd_lattice lat) { return lat ? lat->data_next : nullptr; }

MSIMD_API void sf_lattice_swap(msimd_lattice lat) {
    if (!lat) return;
    void* tmp      = lat->data_prev;
    lat->data_prev = lat->data_curr;
    lat->data_curr = lat->data_next;
    lat->data_next = tmp;
}

MSIMD_API msimd_status_t msimd_lattice_create(msimd_context ctx, msimd_lattice* out_l, int d, size_t nx, size_t ny, size_t nz, msimd_precision_t p) { return sf_lattice_create(ctx, out_l, d, nx, ny, nz, p); }
MSIMD_API void           msimd_lattice_destroy(msimd_lattice lat)  { sf_lattice_destroy(lat); }
MSIMD_API void*          msimd_lattice_get_curr(msimd_lattice lat) { return sf_lattice_get_curr(lat); }
MSIMD_API void*          msimd_lattice_get_prev(msimd_lattice lat) { return sf_lattice_get_prev(lat); }
MSIMD_API void*          msimd_lattice_get_next(msimd_lattice lat) { return sf_lattice_get_next(lat); }
MSIMD_API void           msimd_lattice_swap(msimd_lattice lat)     { sf_lattice_swap(lat); }

MSIMD_API msimd_status_t SF_custominiVm_lattice_create(msimd_context ctx, msimd_lattice* out_l, int d, size_t nx, size_t ny, size_t nz, msimd_precision_t p) { return sf_lattice_create(ctx, out_l, d, nx, ny, nz, p); }
MSIMD_API void           SF_custominiVm_lattice_destroy(msimd_lattice lat)  { sf_lattice_destroy(lat); }
MSIMD_API void*          SF_custominiVm_lattice_get_curr(msimd_lattice lat) { return sf_lattice_get_curr(lat); }
MSIMD_API void*          SF_custominiVm_lattice_get_prev(msimd_lattice lat) { return sf_lattice_get_prev(lat); }
MSIMD_API void*          SF_custominiVm_lattice_get_next(msimd_lattice lat) { return sf_lattice_get_next(lat); }
MSIMD_API void           SF_custominiVm_lattice_swap(msimd_lattice lat)     { sf_lattice_swap(lat); }

/* ========================================================================= */
/* 5. HARDWARE COPROCESSORS (CORDIC, FMAC, TMU), SOFT-FLOAT ALU & XIP/ITCM   */
/* ========================================================================= */

MSIMD_API void sf_cordic_sin_cos_q31(int32_t theta_q31, int32_t* out_sin, int32_t* out_cos, const msimd_tensor_t* precision_target) {
    (void)precision_target;
    int64_t x = CORDIC_GAIN_Q31;
    int64_t y = 0;
    int64_t z = static_cast<int64_t>(theta_q31);
    int sign_flip = 1;

    // Quadrant pre-rotation into [-pi/2, +pi/2] (+/-1073741824 in Q31)
    if (z > 1073741824LL) {
        z -= 2147483648LL;
        sign_flip = -1;
    } else if (z < -1073741824LL) {
        z += 2147483648LL;
        sign_flip = -1;
    }

    for (int i = 0; i < 24; ++i) {
        int64_t x_new, y_new;
        if (z < 0) {
            x_new = x + (y >> i);
            y_new = y - (x >> i);
            z += g_cordic_angles_q31[i];
        } else {
            x_new = x - (y >> i);
            y_new = y + (x >> i);
            z -= g_cordic_angles_q31[i];
        }
        x = x_new;
        y = y_new;
    }
    if (sign_flip < 0) {
        x = -x;
        y = -y;
    }
    if (x > 2147483647LL) x = 2147483647LL;
    else if (x < -2147483648LL) x = -2147483648LL;
    if (y > 2147483647LL) y = 2147483647LL;
    else if (y < -2147483648LL) y = -2147483648LL;
    if (out_cos) *out_cos = static_cast<int32_t>(x);
    if (out_sin) *out_sin = static_cast<int32_t>(y);
}

MSIMD_API void sf_cordic_atan2_q31(int32_t y_q31, int32_t x_q31, int32_t* out_theta, uint32_t* out_mod, const msimd_tensor_t* precision_target) {
    (void)precision_target;
    int64_t x = x_q31;
    int64_t y = y_q31;
    int64_t z = 0;

    if (x < 0) {
        if (y >= 0) {
            int64_t tmp = x;
            x = y;
            y = -tmp;
            z = 1073741824LL;
        } else {
            int64_t tmp = x;
            x = -y;
            y = tmp;
            z = -1073741824LL;
        }
    }

    for (int i = 0; i < 24; ++i) {
        int64_t x_new, y_new;
        if (y > 0) {
            x_new = x + (y >> i);
            y_new = y - (x >> i);
            z += g_cordic_angles_q31[i];
        } else {
            x_new = x - (y >> i);
            y_new = y + (x >> i);
            z -= g_cordic_angles_q31[i];
        }
        x = x_new;
        y = y_new;
    }
    if (z > 2147483647LL) z = 2147483647LL;
    else if (z < -2147483648LL) z = -2147483648LL;
    if (out_theta) *out_theta = static_cast<int32_t>(z);
    if (out_mod)   *out_mod   = static_cast<uint32_t>((x * 1304065792LL) >> 31);
}

MSIMD_API void sf_fmac_fir_q31(const int32_t* x, const int32_t* coeffs, size_t n, size_t num_taps, int32_t* y, const msimd_tensor_t* precision_target) {
    (void)precision_target;
    if (!x || !coeffs || !y || num_taps == 0) return;
    for (size_t i = 0; i < n; ++i) {
        int64_t acc = 0;
        for (size_t k = 0; k < num_taps; ++k) {
            if (i >= k) {
                acc += (static_cast<int64_t>(x[i - k]) * static_cast<int64_t>(coeffs[k])) >> 31;
            }
        }
        if (acc > 2147483647LL) acc = 2147483647LL;
        else if (acc < -2147483648LL) acc = -2147483648LL;
        y[i] = static_cast<int32_t>(acc);
    }
}

MSIMD_API float  sf_tmu_sin_f32(float rad, const msimd_tensor_t* p)              { (void)p; return std::sin(rad); }
MSIMD_API float  sf_tmu_cos_f32(float rad, const msimd_tensor_t* p)              { (void)p; return std::cos(rad); }
MSIMD_API float  sf_softfloat_add_f32(float a, float b, const msimd_tensor_t* p) { (void)p; return a + b; }
MSIMD_API float  sf_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p) { (void)p; return a * b; }
MSIMD_API float  sf_softfloat_div_f32(float a, float b, const msimd_tensor_t* p) { (void)p; return (b != 0.0f) ? (a / b) : 0.0f; }
MSIMD_API double sf_softfloat_add_f64(double a, double b, const msimd_tensor_t* p) { (void)p; return a + b; }
MSIMD_API double sf_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p) { (void)p; return a * b; }
MSIMD_API double sf_softfloat_div_f64(double a, double b, const msimd_tensor_t* p) { (void)p; return (b != 0.0) ? (a / b) : 0.0; }

SF_DISABLE_OPT_BEGIN
MSIMD_API int sf_xip_verify_flash(void) {
    volatile uint32_t* flash_ptr = reinterpret_cast<volatile uint32_t*>(MSIMD_XIP_BASE);
    SF_COMPILER_BARRIER(flash_ptr);
    return (flash_ptr != nullptr) ? 1 : 0;
}
SF_DISABLE_OPT_END

MSIMD_API size_t sf_decompress_block_to_itcm(const void* src_compressed, size_t src_size, void* itcm_dest, size_t max_dest) {
    if (!src_compressed || !itcm_dest || src_size == 0) return 0;
    size_t copy_len = (src_size < max_dest) ? src_size : max_dest;
    std::memcpy(itcm_dest, src_compressed, copy_len);
    return copy_len;
}

MSIMD_API void   msimd_cordic_sin_cos_q31(int32_t t, int32_t* s, int32_t* c, const msimd_tensor_t* p)  { sf_cordic_sin_cos_q31(t, s, c, p); }
MSIMD_API void   msimd_cordic_atan2_q31(int32_t y, int32_t x, int32_t* t, uint32_t* m, const msimd_tensor_t* p) { sf_cordic_atan2_q31(y, x, t, m, p); }
MSIMD_API void   msimd_fmac_fir_q31(const int32_t* x, const int32_t* c, size_t n, size_t k, int32_t* y, const msimd_tensor_t* p) { sf_fmac_fir_q31(x, c, n, k, y, p); }
MSIMD_API float  msimd_tmu_sin_f32(float rad, const msimd_tensor_t* p)                                 { return sf_tmu_sin_f32(rad, p); }
MSIMD_API float  msimd_tmu_cos_f32(float rad, const msimd_tensor_t* p)                                 { return sf_tmu_cos_f32(rad, p); }
MSIMD_API float  msimd_softfloat_add_f32(float a, float b, const msimd_tensor_t* p)                    { return sf_softfloat_add_f32(a, b, p); }
MSIMD_API float  msimd_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p)                    { return sf_softfloat_mul_f32(a, b, p); }
MSIMD_API float  msimd_softfloat_div_f32(float a, float b, const msimd_tensor_t* p)                    { return sf_softfloat_div_f32(a, b, p); }
MSIMD_API double msimd_softfloat_add_f64(double a, double b, const msimd_tensor_t* p)                  { return sf_softfloat_add_f64(a, b, p); }
MSIMD_API double msimd_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p)                  { return sf_softfloat_mul_f64(a, b, p); }
MSIMD_API double msimd_softfloat_div_f64(double a, double b, const msimd_tensor_t* p)                  { return sf_softfloat_div_f64(a, b, p); }
MSIMD_API int    msimd_xip_verify_flash(void)                                                          { return sf_xip_verify_flash(); }
MSIMD_API size_t msimd_decompress_block_to_itcm(const void* s, size_t sz, void* d, size_t m)           { return sf_decompress_block_to_itcm(s, sz, d, m); }

MSIMD_API void   SF_custominiVm_cordic_sin_cos_q31(int32_t t, int32_t* s, int32_t* c, const msimd_tensor_t* p)  { sf_cordic_sin_cos_q31(t, s, c, p); }
MSIMD_API void   SF_custominiVm_cordic_atan2_q31(int32_t y, int32_t x, int32_t* t, uint32_t* m, const msimd_tensor_t* p) { sf_cordic_atan2_q31(y, x, t, m, p); }
MSIMD_API void   SF_custominiVm_fmac_fir_q31(const int32_t* x, const int32_t* c, size_t n, size_t k, int32_t* y, const msimd_tensor_t* p) { sf_fmac_fir_q31(x, c, n, k, y, p); }
MSIMD_API float  SF_custominiVm_tmu_sin_f32(float rad, const msimd_tensor_t* p)                                 { return sf_tmu_sin_f32(rad, p); }
MSIMD_API float  SF_custominiVm_tmu_cos_f32(float rad, const msimd_tensor_t* p)                                 { return sf_tmu_cos_f32(rad, p); }
MSIMD_API float  SF_custominiVm_softfloat_add_f32(float a, float b, const msimd_tensor_t* p)                    { return sf_softfloat_add_f32(a, b, p); }
MSIMD_API float  SF_custominiVm_softfloat_mul_f32(float a, float b, const msimd_tensor_t* p)                    { return sf_softfloat_mul_f32(a, b, p); }
MSIMD_API float  SF_custominiVm_softfloat_div_f32(float a, float b, const msimd_tensor_t* p)                    { return sf_softfloat_div_f32(a, b, p); }
MSIMD_API double SF_custominiVm_softfloat_add_f64(double a, double b, const msimd_tensor_t* p)                  { return sf_softfloat_add_f64(a, b, p); }
MSIMD_API double SF_custominiVm_softfloat_mul_f64(double a, double b, const msimd_tensor_t* p)                  { return sf_softfloat_mul_f64(a, b, p); }
MSIMD_API double SF_custominiVm_softfloat_div_f64(double a, double b, const msimd_tensor_t* p)                  { return sf_softfloat_div_f64(a, b, p); }
MSIMD_API int    SF_custominiVm_xip_verify_flash(void)                                                          { return sf_xip_verify_flash(); }
MSIMD_API size_t SF_custominiVm_decompress_block_to_itcm(const void* s, size_t sz, void* d, size_t m)           { return sf_decompress_block_to_itcm(s, sz, d, m); }

/* ========================================================================= */
/* 6. PRIMITIVA RAW SECTOR R/W (SSIOLSC[PrimitiveRW] HEADER-BYPASS)          */
/* ========================================================================= */

MSIMD_API void* SF_primitiva_alloc(size_t bytes, size_t align) {
    if (!bytes) return nullptr;
    if (SacFor_Gearkit_GetMode() != MSIMD_PROFILE_SAFETY_CRITICAL ||
        SF_custominiVm_get_scgc_type() != SF_SCGC_TYPE_PRIMITIVE_RW) {
        return nullptr;
    }
    if (align < 1) align = 64;
    if ((align & (align - 1)) != 0) {
        size_t next_pow2 = 1;
        while (next_pow2 < align) next_pow2 <<= 1;
        align = next_pow2;
    }
    auto* runway = sf_tensor_views_get_runway();
    if (!runway) return nullptr;
    size_t padded = bytes + align;
    void* raw = runway->allocator.allocate(padded);
    if (!raw) return nullptr;
    uintptr_t addr = (reinterpret_cast<uintptr_t>(raw) + (align - 1)) & ~(static_cast<uintptr_t>(align) - 1);
    return reinterpret_cast<void*>(addr);
}

MSIMD_API void SF_primitiva_write(void* sector_ptr, const void* src, size_t bytes) {
    if (!sector_ptr || !src || !bytes) return;
    std::memcpy(sector_ptr, src, bytes);
    sf_hw_fence_store();
}

MSIMD_API void SF_primitiva_read(const void* sector_ptr, void* dst, size_t bytes) {
    if (!sector_ptr || !dst || !bytes) return;
    sf_hw_fence_load();
    std::memcpy(dst, sector_ptr, bytes);
}

MSIMD_API void SF_primitiva_overwrite(void* sector_ptr, const void* src, size_t bytes) {
    if (!sector_ptr || !src || !bytes) return;
    sf_dram_stream_store(sector_ptr, src, bytes);
    sf_hw_fence_store();
}

SF_DISABLE_OPT_BEGIN
MSIMD_API void SF_primitiva_zero(void* sector_ptr, size_t bytes) {
    if (!sector_ptr || !bytes) return;
    sf_critical_clear_memory(sector_ptr, bytes);
    SF_COMPILER_BARRIER(sector_ptr);
}
SF_DISABLE_OPT_END

} // extern "C"
