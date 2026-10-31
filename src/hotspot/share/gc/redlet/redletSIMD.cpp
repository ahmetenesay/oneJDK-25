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

#include "gc/redlet/redletSIMD.hpp"
#include "gc/redlet/redletGearkit.hpp"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#if defined(_MSC_VER) || defined(__clang__)
#include <intrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

static std::atomic<uint8_t> g_sf_critical_simd_mode{ static_cast<uint8_t>(sf_critical_simd_e::AUTO) };
static std::atomic<bool>    g_sf_has_vcache{ false };
static std::atomic<bool>    g_sf_topo_checked{ false };
static std::atomic<bool>    g_sf_has_avx2{ false };
static std::atomic<bool>    g_sf_has_avx512{ false };

static void sf_detect_cpu_features() {
    if (g_sf_topo_checked.exchange(true, std::memory_order_acq_rel)) return;

#if defined(_M_X64) || defined(__x86_64__)
    int info[4] = { 0 };
    __cpuid(info, 0);
    int nIds = info[0];

    char vendor[13] = { 0 };
    memcpy(vendor + 0, &info[1], 4);
    memcpy(vendor + 4, &info[3], 4);
    memcpy(vendor + 8, &info[2], 4);

    if (nIds >= 1) {
        __cpuidex(info, 1, 0);
    }
    if (nIds >= 7) {
        __cpuidex(info, 7, 0);
        bool avx2 = (info[1] & (1 << 5)) != 0;
        bool avx512f = (info[1] & (1 << 16)) != 0;
        g_sf_has_avx2.store(avx2, std::memory_order_release);
        g_sf_has_avx512.store(avx512f, std::memory_order_release);
    }

    // Check AMD 3D V-Cache (L3 >= 64MB or specific AMD extended CPUID leaves)
    __cpuid(info, 0x80000000);
    unsigned int nExIds = info[0];
    if (nExIds >= 0x80000006) {
        __cpuid(info, 0x80000006);
        // EDX bits 31:18 is L3 cache size in 512KB chunks (or bits 31:16 in 64KB)
        uint32_t l3_kb = ((uint32_t)info[3] >> 18) * 512;
        if (l3_kb >= 64 * 1024) {
            g_sf_has_vcache.store(true, std::memory_order_release);
        }
    }
#endif
}

MSIMD_API bool sf_has_massive_sram_vcache(void) {
    sf_detect_cpu_features();
    return g_sf_has_vcache.load(std::memory_order_relaxed);
}

MSIMD_API bool sf_is_efficiency_core_active(void) {
#if defined(_WIN32)
    PROCESSOR_NUMBER pn;
    GetCurrentProcessorNumberEx(&pn);
    // On typical Intel hybrid (e.g. Alder/Raptor/Arrow Lake), E-cores have different group/efficiency metrics
    return false;
#else
    return false;
#endif
}

MSIMD_API void sf_set_critical_simd_mode(sf_critical_simd_e mode) {
    g_sf_critical_simd_mode.store(static_cast<uint8_t>(mode), std::memory_order_release);
}

MSIMD_API sf_critical_simd_e sf_get_critical_simd_mode(void) {
    return static_cast<sf_critical_simd_e>(g_sf_critical_simd_mode.load(std::memory_order_acquire));
}

MSIMD_API void sf_flush_wc_buffers(void) {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
    __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
    sf_hotlog_emit(SF_Ebin0_STREAMING_STORE_FLUSH, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0, 0, "wc_buffers_flushed");
}

/* ========================================================================= */
/* CORE ADAPTIVE CLEARING & STREAMING STORES (SacFor_VM.cpp:500-550)         */
/* ========================================================================= */

MSIMD_API void sf_critical_clear_memory_core_adaptive(void* dst, size_t bytes, bool prefer_nt) {
    if (!dst || bytes == 0) return;
    uint8_t m = g_sf_critical_simd_mode.load(std::memory_order_relaxed);
    if (m == static_cast<uint8_t>(sf_critical_simd_e::DENY)) {
        memset(dst, 0, bytes);
        return;
    }

    // Core & SRAM Adaptive Logic:
    // 1. On E-cores: ALWAYS use non-temporal streaming stores for objects >= 64 bytes.
    // 2. On P-cores / Homogeneous cores:
    //    - If bytes < 64KB (or < 256KB on 3D V-Cache): use fast temporal stores to keep cell in L2.
    //    - If bytes >= 64KB: use streaming non-temporal stores to avoid cache pollution.
    bool use_nt = prefer_nt || sf_is_efficiency_core_active();
    if (!use_nt && bytes >= (sf_has_massive_sram_vcache() ? (256 * 1024) : (64 * 1024))) {
        use_nt = true;
    }

#if defined(_M_X64) || defined(__x86_64__)
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t rem = bytes;

    if (use_nt && bytes >= 64) {
        // Handle unaligned prefix before streaming stores
        while (rem > 0 && (((uintptr_t)p & 15) != 0)) {
            *p++ = 0;
            --rem;
        }
#if defined(__AVX2__)
        if (rem >= 32 && (((uintptr_t)p & 31) == 0)) {
            __m256i z256 = _mm256_setzero_si256();
            while (rem >= 32) {
                _mm256_stream_si256(reinterpret_cast<__m256i*>(p), z256);
                p += 32;
                rem -= 32;
            }
        }
#endif
        __m128i z128 = _mm_setzero_si128();
        while (rem >= 16) {
            _mm_stream_si128(reinterpret_cast<__m128i*>(p), z128);
            p += 16;
            rem -= 16;
        }
        _mm_sfence();
    }
    else {
#if defined(__AVX2__)
        __m256i z256 = _mm256_setzero_si256();
        while (rem >= 32) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), z256);
            p += 32;
            rem -= 32;
        }
#endif
        __m128i z128 = _mm_setzero_si128();
        while (rem >= 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p), z128);
            p += 16;
            rem -= 16;
        }
    }
    while (rem > 0) {
        *p++ = 0;
        --rem;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t rem = bytes;
    uint8x16_t z = vdupq_n_u8(0);
    while (rem >= 16) {
        vst1q_u8(p, z);
        p += 16;
        rem -= 16;
    }
    while (rem > 0) {
        *p++ = 0;
        --rem;
    }
#else
    memset(dst, 0, bytes);
#endif
}

MSIMD_API void sf_critical_clear_memory(void* dst, size_t bytes) {
    sf_critical_clear_memory_core_adaptive(dst, bytes, false);
}

MSIMD_API void sf_dram_stream_copy(void* dst, const void* src, size_t bytes) {
    if (!dst || !src || bytes == 0) return;
#if defined(_M_X64) || defined(__x86_64__)
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    size_t rem = bytes;

    if (rem >= 64 && (((uintptr_t)d & 15) == 0) && (((uintptr_t)s & 15) == 0)) {
#if defined(__AVX2__)
        if (rem >= 32 && (((uintptr_t)d & 31) == 0) && (((uintptr_t)s & 31) == 0)) {
            while (rem >= 32) {
                __m256i val = _mm256_load_si256(reinterpret_cast<const __m256i*>(s));
                _mm256_stream_si256(reinterpret_cast<__m256i*>(d), val);
                d += 32;
                s += 32;
                rem -= 32;
            }
        }
#endif
        while (rem >= 16) {
            __m128i val = _mm_load_si128(reinterpret_cast<const __m128i*>(s));
            _mm_stream_si128(reinterpret_cast<__m128i*>(d), val);
            d += 16;
            s += 16;
            rem -= 16;
        }
        _mm_sfence();
    }
    if (rem > 0) {
        memcpy(d, s, rem);
    }
#else
    memcpy(dst, src, bytes);
#endif
}

/* ========================================================================= */
/* VECTOR LANE HEAT DECAY SWEEPS (SacFor_VM.cpp:8368-8415)                   */
/* ========================================================================= */

MSIMD_API void sf_decay_heats_avx2(float* heats, size_t count, float decay_f, uint64_t* sweep_counts) {
#if defined(__AVX2__) || (defined(_M_X64) && defined(__AVX2__))
    __m256 v_decay = _mm256_set1_ps(decay_f);
    size_t b = 0;
    for (; b + 8 <= count; b += 8) {
        __m256 v_heats = _mm256_loadu_ps(heats + b);
        __m256 v_res = _mm256_mul_ps(v_heats, v_decay);
        _mm256_storeu_ps(heats + b, v_res);
        if (sweep_counts) {
            for (size_t k = 0; k < 8; ++k) sweep_counts[b + k]++;
        }
    }
    for (; b < count; ++b) {
        heats[b] *= decay_f;
        if (sweep_counts) sweep_counts[b]++;
    }
#else
    sf_decay_heats_scalar(heats, count, decay_f, sweep_counts);
#endif
}

MSIMD_API void sf_decay_heats_avx512(float* heats, size_t count, float decay_f, uint64_t* sweep_counts) {
#if defined(__AVX512F__)
    __m512 v_decay = _mm512_set1_ps(decay_f);
    size_t b = 0;
    for (; b + 16 <= count; b += 16) {
        __m512 v_heats = _mm512_loadu_ps(heats + b);
        __m512 v_res = _mm512_mul_ps(v_heats, v_decay);
        _mm512_storeu_ps(heats + b, v_res);
        if (sweep_counts) {
            for (size_t k = 0; k < 16; ++k) sweep_counts[b + k]++;
        }
    }
    for (; b < count; ++b) {
        heats[b] *= decay_f;
        if (sweep_counts) sweep_counts[b]++;
    }
#else
    sf_decay_heats_avx2(heats, count, decay_f, sweep_counts);
#endif
}

MSIMD_API void sf_decay_heats_neon(float* heats, size_t count, float decay_f, uint64_t* sweep_counts) {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    float32x4_t v_decay = vdupq_n_f32(decay_f);
    size_t b = 0;
    for (; b + 4 <= count; b += 4) {
        float32x4_t v_h = vld1q_f32(heats + b);
        v_h = vmulq_f32(v_h, v_decay);
        vst1q_f32(heats + b, v_h);
        if (sweep_counts) {
            for (size_t k = 0; k < 4; ++k) sweep_counts[b + k]++;
        }
    }
    for (; b < count; ++b) {
        heats[b] *= decay_f;
        if (sweep_counts) sweep_counts[b]++;
    }
#else
    sf_decay_heats_scalar(heats, count, decay_f, sweep_counts);
#endif
}

MSIMD_API void sf_decay_heats_scalar(float* heats, size_t count, float decay_f, uint64_t* sweep_counts) {
    for (size_t b = 0; b < count; ++b) {
        heats[b] *= decay_f;
        if (sweep_counts) sweep_counts[b]++;
    }
}

/* ========================================================================= */
/* VECTOR LIVE STATE FILTERING                                               */
/* ========================================================================= */

MSIMD_API size_t sf_filter_live_states_avx2(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices) {
    size_t matched = 0;
#if defined(__AVX2__) || (defined(_M_X64) && defined(__AVX2__))
    __m256i v_target = _mm256_set1_epi8(static_cast<char>(target_state));
    size_t b = 0;
    for (; b + 32 <= count; b += 32) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(states + b));
        __m256i cmp = _mm256_cmpeq_epi8(chunk, v_target);
        unsigned int mask = static_cast<unsigned int>(_mm256_movemask_epi8(cmp));
        while (mask != 0) {
#if defined(_MSC_VER)
            unsigned long bit_pos;
            _BitScanForward(&bit_pos, mask);
#else
            unsigned int bit_pos = __builtin_ctz(mask);
#endif
            if (out_indices) out_indices[matched] = b + bit_pos;
            matched++;
            mask &= mask - 1;
        }
    }
    for (; b < count; ++b) {
        if (states[b] == target_state) {
            if (out_indices) out_indices[matched] = b;
            matched++;
        }
    }
#else
    matched = sf_filter_live_states_scalar(states, count, target_state, out_indices);
#endif
    return matched;
}

MSIMD_API size_t sf_filter_live_states_avx512(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices) {
#if defined(__AVX512F__) && defined(__AVX512BW__)
    size_t matched = 0;
    __m512i v_target = _mm512_set1_epi8(static_cast<char>(target_state));
    size_t b = 0;
    for (; b + 64 <= count; b += 64) {
        __m512i chunk = _mm512_loadu_si512(reinterpret_cast<const void*>(states + b));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, v_target);
        while (mask != 0) {
#if defined(_MSC_VER)
            unsigned long bit_pos;
            _BitScanForward64(&bit_pos, mask);
#else
            unsigned int bit_pos = __builtin_ctzll(mask);
#endif
            if (out_indices) out_indices[matched] = b + bit_pos;
            matched++;
            mask &= mask - 1;
        }
    }
    for (; b < count; ++b) {
        if (states[b] == target_state) {
            if (out_indices) out_indices[matched] = b;
            matched++;
        }
    }
    return matched;
#else
    return sf_filter_live_states_avx2(states, count, target_state, out_indices);
#endif
}

MSIMD_API size_t sf_filter_live_states_neon(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices) {
    size_t matched = 0;
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    uint8x16_t v_target = vdupq_n_u8(target_state);
    size_t b = 0;
    for (; b + 16 <= count; b += 16) {
        uint8x16_t chunk = vld1q_u8(states + b);
        uint8x16_t cmp = vceqq_u8(chunk, v_target);
        uint8_t raw[16];
        vst1q_u8(raw, cmp);
        for (size_t k = 0; k < 16; ++k) {
            if (raw[k]) {
                if (out_indices) out_indices[matched] = b + k;
                matched++;
            }
        }
    }
    for (; b < count; ++b) {
        if (states[b] == target_state) {
            if (out_indices) out_indices[matched] = b;
            matched++;
        }
    }
#else
    matched = sf_filter_live_states_scalar(states, count, target_state, out_indices);
#endif
    return matched;
}

MSIMD_API size_t sf_filter_live_states_scalar(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices) {
    size_t matched = 0;
    for (size_t b = 0; b < count; ++b) {
        if (states[b] == target_state) {
            if (out_indices) out_indices[matched] = b;
            matched++;
        }
    }
    return matched;
}

/* ========================================================================= */
/* REDLET SIMD DISPATCHER                                                    */
/* ========================================================================= */

void RedletSIMD::decay_heats(float* heats, size_t count, float decay_f, uint64_t* sweep_counts) {
    if (!heats || count == 0) return;
    sf_detect_cpu_features();

    if (g_sf_has_avx512.load(std::memory_order_relaxed)) {
        sf_decay_heats_avx512(heats, count, decay_f, sweep_counts);
    } else if (g_sf_has_avx2.load(std::memory_order_relaxed)) {
        sf_decay_heats_avx2(heats, count, decay_f, sweep_counts);
    } else {
#if defined(__aarch64__) || defined(_M_ARM64)
        sf_decay_heats_neon(heats, count, decay_f, sweep_counts);
#else
        sf_decay_heats_scalar(heats, count, decay_f, sweep_counts);
#endif
    }
}

size_t RedletSIMD::filter_live_states(const uint8_t* states, size_t count, uint8_t target_state, size_t* out_indices) {
    if (!states || count == 0) return 0;
    sf_detect_cpu_features();

    if (g_sf_has_avx512.load(std::memory_order_relaxed)) {
        return sf_filter_live_states_avx512(states, count, target_state, out_indices);
    } else if (g_sf_has_avx2.load(std::memory_order_relaxed)) {
        return sf_filter_live_states_avx2(states, count, target_state, out_indices);
    } else {
#if defined(__aarch64__) || defined(_M_ARM64)
        return sf_filter_live_states_neon(states, count, target_state, out_indices);
#else
        return sf_filter_live_states_scalar(states, count, target_state, out_indices);
#endif
    }
}

bool RedletSIMD::has_avx2() {
    sf_detect_cpu_features();
    return g_sf_has_avx2.load(std::memory_order_relaxed);
}

bool RedletSIMD::has_avx512() {
    sf_detect_cpu_features();
    return g_sf_has_avx512.load(std::memory_order_relaxed);
}

bool RedletSIMD::has_neon() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}

bool RedletSIMD::has_3d_vcache() {
    return sf_has_massive_sram_vcache();
}
