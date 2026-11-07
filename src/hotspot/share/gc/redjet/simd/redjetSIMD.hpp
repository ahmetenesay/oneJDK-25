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

#ifndef SHARE_GC_REDJET_REDJETSIMD_HPP
#define SHARE_GC_REDJET_REDJETSIMD_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum class hs_critical_simd_e : uint8_t {
    AUTO  = 0,
    ALLOW = 1,
    DENY  = 2
};

/* ========================================================================= */
/* 1. CACHELINE ZEROING & NON-TEMPORAL STREAMING STORE APIS                  */
/* ========================================================================= */

RJ_API void hs_critical_clear_memory_core_adaptive(void* dst, size_t bytes, bool prefer_nt = false);
RJ_API void hs_critical_clear_memory(void* dst, size_t bytes);
RJ_API void hs_dram_stream_copy(void* dst, const void* src, size_t bytes);
RJ_API void hs_flush_wc_buffers(void);

/* ========================================================================= */
/* 2. TOPOLOGY & HARDWARE CACHE CONTEXT                                      */
/* ========================================================================= */

RJ_API bool hs_has_massive_sram_vcache(void);
RJ_API bool hs_is_efficiency_core_active(void);
RJ_API void hs_set_critical_simd_mode(hs_critical_simd_e mode);
RJ_API hs_critical_simd_e hs_get_critical_simd_mode(void);

/* ========================================================================= */
/* 3. SIMD VECTOR LANE HEAT DECAY SWEEPS (SacFor_VM.cpp:8368-8415)           */
/* ========================================================================= */

RJ_API void hs_decay_heats_avx512(float* heats, size_t count, float decay_f, uint64_t* sweep_counts);
RJ_API void hs_decay_heats_avx2(float* heats, size_t count, float decay_f, uint64_t* sweep_counts);
RJ_API void hs_decay_heats_sse42(float* heats, size_t count, float decay_f, uint64_t* sweep_counts);
RJ_API void hs_decay_heats_neon(float* heats, size_t count, float decay_f, uint64_t* sweep_counts);
RJ_API void hs_decay_heats_scalar(float* heats, size_t count, float decay_f, uint64_t* sweep_counts);

/* ========================================================================= */
/* 4. SIMD VECTOR LIVE STATE FILTERING                                       */
/* ========================================================================= */

RJ_API size_t hs_filter_live_states_avx512(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices);
RJ_API size_t hs_filter_live_states_avx2(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices);
RJ_API size_t hs_filter_live_states_sse42(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices);
RJ_API size_t hs_filter_live_states_neon(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices);
RJ_API size_t hs_filter_live_states_scalar(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices);

#ifdef __cplusplus
}
#endif

/* ========================================================================= */
/* 5. REDJET SIMD C++ DISPATCH WRAPPER                                       */
/* ========================================================================= */

class RedjetSIMD {
public:
    static inline void clear_memory_adaptive(void* dst, size_t bytes, bool prefer_nt = false) {
        hs_critical_clear_memory_core_adaptive(dst, bytes, prefer_nt);
    }

    static inline void clear_memory(void* dst, size_t bytes) {
        hs_critical_clear_memory(dst, bytes);
    }

    static inline void stream_copy(void* dst, const void* src, size_t bytes) {
        hs_dram_stream_copy(dst, src, bytes);
    }

    static void decay_heats(float* heats, size_t count, float decay_f, uint64_t* sweep_counts = nullptr);

    static size_t filter_live_states(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices);

    static bool has_avx10();
    static bool has_avx10_2();
    static bool has_avx512();
    static bool has_avx2();
    static bool has_sse42();
    static bool has_fma();
    static bool has_neon();
    static bool has_3d_vcache();

    // Dynamic SIMD Throttling & Congestion Fallback
    static bool is_simd_congested();
    static bool should_use_simd(bool is_latency_critical = true);
};

#endif // SHARE_GC_REDJET_REDJETSIMD_HPP
