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

#include "gc/redlet/self_advance/redletSelfAdvanceHeuristics.hpp"
#include "gc/redlet/redletSIMD.hpp"
#include <cstring>
#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#define log_warning(...) if (false) printf
#define log_error(...) if (false) printf
#endif

RedletCardTableBitmapEvolver g_redlet_bitmap_evolver;
RedletMemoryResetEvolver     g_redlet_memreset_evolver;
RedletRMRPHeuristics         g_redlet_rmrp_heuristics;
RedletResurrectionHeuristics g_redlet_resurrection_heuristics;

/* ========================================================================= */
/* Bit Manipulation Helpers                                                  */
/* ========================================================================= */

static inline int redlet_ctz_u64(uint64_t val) {
    if (val == 0) return 64;
#if defined(_MSC_VER)
#if defined(_M_X64) || defined(_M_ARM64)
    unsigned long idx = 0;
    if (_BitScanForward64(&idx, val)) {
        return static_cast<int>(idx);
    }
    return 64;
#else
    unsigned long idx = 0;
    uint32_t low = static_cast<uint32_t>(val);
    if (low != 0 && _BitScanForward(&idx, low)) {
        return static_cast<int>(idx);
    }
    uint32_t high = static_cast<uint32_t>(val >> 32);
    if (high != 0 && _BitScanForward(&idx, high)) {
        return static_cast<int>(idx + 32);
    }
    return 64;
#endif
#else
    return __builtin_ctzll(val);
#endif
}

/* ========================================================================= */
/* 1. RedletCardTableBitmapEvolver                                           */
/* ========================================================================= */

RedletCardTableBitmapEvolver::RedletCardTableBitmapEvolver() {
    reset();
}

size_t RedletCardTableBitmapEvolver::scan_scalar(const uintptr_t* bitmap, size_t word_count,
                                                 size_t* out_marked_indices, size_t max_indices) {
    if (!bitmap || !out_marked_indices || max_indices == 0) return 0;
    size_t count = 0;
    constexpr size_t bits_per_word = sizeof(uintptr_t) * 8;

    for (size_t w = 0; w < word_count && count < max_indices; ++w) {
        uintptr_t val = bitmap[w];
        if (val == 0) continue;
        for (size_t b = 0; b < bits_per_word && count < max_indices; ++b) {
            if ((val & (static_cast<uintptr_t>(1) << b)) != 0) {
                out_marked_indices[count++] = (w * bits_per_word) + b;
            }
        }
    }
    return count;
}

size_t RedletCardTableBitmapEvolver::scan_bmi2(const uintptr_t* bitmap, size_t word_count,
                                               size_t* out_marked_indices, size_t max_indices) {
    if (!bitmap || !out_marked_indices || max_indices == 0) return 0;
    size_t count = 0;
    constexpr size_t bits_per_word = sizeof(uintptr_t) * 8;

    for (size_t w = 0; w < word_count && count < max_indices; ++w) {
        uint64_t val = static_cast<uint64_t>(bitmap[w]);
        size_t base_idx = w * bits_per_word;

        // BMI2 pattern: extract lowest set bit via TZCNT, then clear it via BLSR (val &= val - 1)
        while (val != 0 && count < max_indices) {
            int bit = redlet_ctz_u64(val);
            out_marked_indices[count++] = base_idx + static_cast<size_t>(bit);
            val &= (val - 1ULL); // BLSR
        }
    }
    return count;
}

size_t RedletCardTableBitmapEvolver::scan_avx2(const uintptr_t* bitmap, size_t word_count,
                                               size_t* out_marked_indices, size_t max_indices) {
    if (!bitmap || !out_marked_indices || max_indices == 0) return 0;

    if (!RedletSIMD::has_avx2()) {
        return scan_bmi2(bitmap, word_count, out_marked_indices, max_indices);
    }

    size_t count = 0;
    constexpr size_t bits_per_word = sizeof(uintptr_t) * 8;
    size_t w = 0;

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64)
    // 256-bit AVX2 fast skip of empty 4-word blocks (256 bits)
    for (; w + 4 <= word_count && count < max_indices; w += 4) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&bitmap[w]));
        if (_mm256_testz_si256(chunk, chunk)) {
            // Entire 256-bit block has 0 marked bits, skip directly
            continue;
        }

        // Non-empty block: process each non-zero word with BMI2
        for (size_t sub = 0; sub < 4 && count < max_indices; ++sub) {
            uint64_t val = static_cast<uint64_t>(bitmap[w + sub]);
            size_t base_idx = (w + sub) * bits_per_word;
            while (val != 0 && count < max_indices) {
                int bit = redlet_ctz_u64(val);
                out_marked_indices[count++] = base_idx + static_cast<size_t>(bit);
                val &= (val - 1ULL);
            }
        }
    }
#endif

    // Remainder words
    for (; w < word_count && count < max_indices; ++w) {
        uint64_t val = static_cast<uint64_t>(bitmap[w]);
        size_t base_idx = w * bits_per_word;
        while (val != 0 && count < max_indices) {
            int bit = redlet_ctz_u64(val);
            out_marked_indices[count++] = base_idx + static_cast<size_t>(bit);
            val &= (val - 1ULL);
        }
    }
    return count;
}

size_t RedletCardTableBitmapEvolver::scan_avx512(const uintptr_t* bitmap, size_t word_count,
                                                 size_t* out_marked_indices, size_t max_indices) {
    if (!bitmap || !out_marked_indices || max_indices == 0) return 0;

    if (!RedletSIMD::has_avx512()) {
        return scan_avx2(bitmap, word_count, out_marked_indices, max_indices);
    }

    size_t count = 0;
    constexpr size_t bits_per_word = sizeof(uintptr_t) * 8;
    size_t w = 0;

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64)
    // 512-bit AVX-512 fast skip of empty 8-word blocks (512 bits)
    for (; w + 8 <= word_count && count < max_indices; w += 8) {
        __m512i chunk = _mm512_loadu_si512(reinterpret_cast<const void*>(&bitmap[w]));
        __mmask8 mask = _mm512_test_epi64_mask(chunk, chunk);
        if (mask == 0) {
            // Entire 512-bit block has 0 marked bits, skip directly
            continue;
        }

        // Non-empty block: process only non-zero words
        for (size_t sub = 0; sub < 8 && count < max_indices; ++sub) {
            if ((mask & (1U << sub)) == 0) continue;
            uint64_t val = static_cast<uint64_t>(bitmap[w + sub]);
            size_t base_idx = (w + sub) * bits_per_word;
            while (val != 0 && count < max_indices) {
                int bit = redlet_ctz_u64(val);
                out_marked_indices[count++] = base_idx + static_cast<size_t>(bit);
                val &= (val - 1ULL);
            }
        }
    }
#endif

    // Remainder words processed with AVX2 or BMI2
    for (; w < word_count && count < max_indices; ++w) {
        uint64_t val = static_cast<uint64_t>(bitmap[w]);
        size_t base_idx = w * bits_per_word;
        while (val != 0 && count < max_indices) {
            int bit = redlet_ctz_u64(val);
            out_marked_indices[count++] = base_idx + static_cast<size_t>(bit);
            val &= (val - 1ULL);
        }
    }
    return count;
}

bool RedletCardTableBitmapEvolver::verify_and_evolve_best_kernel() {
    constexpr size_t TEST_WORDS = 128;
    constexpr size_t MAX_MARKS = 512;

    uintptr_t test_bitmap[TEST_WORDS]{ 0 };
    for (size_t i = 0; i < TEST_WORDS; ++i) {
        if ((i % 3) == 0) test_bitmap[i] = 0x8001004020000001ULL ^ (static_cast<uintptr_t>(i) << 17);
        if ((i % 7) == 0) test_bitmap[i] |= 0x0000000000000003ULL;
    }

    size_t ref_indices[MAX_MARKS]{ 0 };
    size_t cand_indices[MAX_MARKS]{ 0 };

    // 1. Baseline gold standard (exact reference)
    size_t ref_count = scan_scalar(test_bitmap, TEST_WORDS, ref_indices, MAX_MARKS);

    // 2. Test BMI2 candidate
    size_t bmi2_count = scan_bmi2(test_bitmap, TEST_WORDS, cand_indices, MAX_MARKS);
    bool bmi2_exact = (bmi2_count == ref_count) &&
                      (std::memcmp(ref_indices, cand_indices, ref_count * sizeof(size_t)) == 0);

    if (!bmi2_exact) {
        _divergences_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Default promotion to BMI2
    redlet_bitmap_scan_fn_t best_fn = scan_bmi2;
    RedletBitmapScanKernel best_type = RedletBitmapScanKernel::BMI2_ACCELERATED;

    // 3. Test AVX2 candidate if hardware supports it
    if (RedletSIMD::has_avx2()) {
        std::memset(cand_indices, 0, sizeof(cand_indices));
        size_t avx2_count = scan_avx2(test_bitmap, TEST_WORDS, cand_indices, MAX_MARKS);
        bool avx2_exact = (avx2_count == ref_count) &&
                          (std::memcmp(ref_indices, cand_indices, ref_count * sizeof(size_t)) == 0);

        if (avx2_exact) {
            best_fn = scan_avx2;
            best_type = RedletBitmapScanKernel::AVX2_MASKED;
        } else {
            _divergences_rejected.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 4. Test AVX-512 candidate if hardware supports it
    if (RedletSIMD::has_avx512()) {
        std::memset(cand_indices, 0, sizeof(cand_indices));
        size_t avx512_count = scan_avx512(test_bitmap, TEST_WORDS, cand_indices, MAX_MARKS);
        bool avx512_exact = (avx512_count == ref_count) &&
                            (std::memcmp(ref_indices, cand_indices, ref_count * sizeof(size_t)) == 0);

        if (avx512_exact) {
            best_fn = scan_avx512;
            best_type = RedletBitmapScanKernel::AVX512_MASKED;
        } else {
            _divergences_rejected.fetch_add(1, std::memory_order_relaxed);
        }
    }

    _exact_matches_verified.fetch_add(1, std::memory_order_relaxed);
    _evolution_count.fetch_add(1, std::memory_order_relaxed);

    _active_kernel.store(best_fn, std::memory_order_release);
    _active_kernel_type.store(best_type, std::memory_order_release);

    char msg[128];
    snprintf(msg, sizeof(msg), "[Redlet Bitmap Evolver] Promoted kernel type %d (verified 0-bit divergence)",
             static_cast<int>(best_type));
    sf_unified_log_emit(SF_Ebin0_PROMOTE, SF_Ebin1_INFO, 0, "bitmap_evolver", msg);
    g_redlet_self_advance_telemetry.record_event(SF_EVENT_BITMAP_EVOLVED, "bitmap_evolver", msg, 0.0, static_cast<double>(best_type));

    return true;
}

void RedletCardTableBitmapEvolver::reset() {
    _active_kernel_type.store(RedletBitmapScanKernel::SCALAR_BASELINE, std::memory_order_relaxed);
    _active_kernel.store(scan_scalar, std::memory_order_relaxed);
    _evolution_count.store(0, std::memory_order_relaxed);
    _exact_matches_verified.store(0, std::memory_order_relaxed);
    _divergences_rejected.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. RedletMemoryResetEvolver                                               */
/* ========================================================================= */

RedletMemoryResetEvolver::RedletMemoryResetEvolver() {
    reset();
}

void RedletMemoryResetEvolver::zero_libc(void* dest, size_t bytes) {
    if (dest && bytes > 0) {
        std::memset(dest, 0, bytes);
    }
}

void RedletMemoryResetEvolver::zero_streaming_nt(void* dest, size_t bytes) {
    if (!dest || bytes == 0) return;

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64)
    uint8_t* ptr = static_cast<uint8_t*>(dest);
    size_t remaining = bytes;

    // Prefer AVX-512 non-temporal 64-byte streaming stores when available
    if (RedletSIMD::has_avx512() && remaining >= 128) {
        size_t align_bytes = (64 - (reinterpret_cast<uintptr_t>(ptr) & 63)) & 63;
        if (align_bytes > 0 && align_bytes <= remaining) {
            std::memset(ptr, 0, align_bytes);
            ptr += align_bytes;
            remaining -= align_bytes;
        }

        __m512i zero_512 = _mm512_setzero_si512();
        while (remaining >= 64) {
            _mm512_stream_si512(reinterpret_cast<void*>(ptr), zero_512);
            ptr += 64;
            remaining -= 64;
        }

        _mm_sfence();

        if (remaining > 0) {
            std::memset(ptr, 0, remaining);
        }
        return;
    }

    // Next prefer AVX2 non-temporal 32-byte streaming stores
    if (RedletSIMD::has_avx2() && remaining >= 64) {
        size_t align_bytes = (32 - (reinterpret_cast<uintptr_t>(ptr) & 31)) & 31;
        if (align_bytes > 0 && align_bytes <= remaining) {
            std::memset(ptr, 0, align_bytes);
            ptr += align_bytes;
            remaining -= align_bytes;
        }

        __m256i zero_vec = _mm256_setzero_si256();
        while (remaining >= 32) {
            _mm256_stream_si256(reinterpret_cast<__m256i*>(ptr), zero_vec);
            ptr += 32;
            remaining -= 32;
        }

        _mm_sfence();

        if (remaining > 0) {
            std::memset(ptr, 0, remaining);
        }
        return;
    }
#endif

    std::memset(dest, 0, bytes);
}

void RedletMemoryResetEvolver::zero_ecore_chunked(void* dest, size_t bytes) {
    if (!dest || bytes == 0) return;

    constexpr size_t CHUNK_SIZE = 64;
    uint8_t* ptr = static_cast<uint8_t*>(dest);
    size_t remaining = bytes;

    while (remaining >= CHUNK_SIZE) {
        std::memset(ptr, 0, CHUNK_SIZE);
        ptr += CHUNK_SIZE;
        remaining -= CHUNK_SIZE;
    }

    if (remaining > 0) {
        std::memset(ptr, 0, remaining);
    }
}

void RedletMemoryResetEvolver::memzero_adaptive(void* dest, size_t bytes, bool on_p_core, bool high_cache_pressure) {
    if (!dest || bytes == 0) return;

    if (bytes >= (64ULL * 1024) && on_p_core && high_cache_pressure) {
        zero_streaming_nt(dest, bytes);
        _nt_stores_invoked.fetch_add(1, std::memory_order_relaxed);
        _active_strategy.store(RedletMemzeroStrategy::STREAMING_STORES_NT, std::memory_order_relaxed);
    } else if (!on_p_core && bytes >= 4096) {
        zero_ecore_chunked(dest, bytes);
        _chunked_stores_invoked.fetch_add(1, std::memory_order_relaxed);
        _active_strategy.store(RedletMemzeroStrategy::ECORE_CHUNKED, std::memory_order_relaxed);
    } else {
        zero_libc(dest, bytes);
        _libc_memset_invoked.fetch_add(1, std::memory_order_relaxed);
        _active_strategy.store(RedletMemzeroStrategy::LIBC_MEMSET, std::memory_order_relaxed);
    }
}

void RedletMemoryResetEvolver::query_counters(uint64_t* out_nt, uint64_t* out_chunk, uint64_t* out_libc) const {
    if (out_nt) *out_nt = _nt_stores_invoked.load(std::memory_order_relaxed);
    if (out_chunk) *out_chunk = _chunked_stores_invoked.load(std::memory_order_relaxed);
    if (out_libc) *out_libc = _libc_memset_invoked.load(std::memory_order_relaxed);
}

void RedletMemoryResetEvolver::reset() {
    _active_strategy.store(RedletMemzeroStrategy::LIBC_MEMSET, std::memory_order_relaxed);
    _nt_stores_invoked.store(0, std::memory_order_relaxed);
    _chunked_stores_invoked.store(0, std::memory_order_relaxed);
    _libc_memset_invoked.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 3. RedletRMRPHeuristics                                                   */
/* ========================================================================= */

RedletRMRPHeuristics::RedletRMRPHeuristics() {
    reset();
}

RedletRMRPStrategy RedletRMRPHeuristics::evaluate_and_update(size_t used_bytes, size_t committed_bytes,
                                                             size_t total_reserved, double alloc_rate_mbps) {
    (void)committed_bytes;
    (void)alloc_rate_mbps;

    RedletRMRPStrategy target = RedletRMRPStrategy::SEQUENTIAL_INSTALLMENT;

    if (total_reserved > 0) {
        double usage_ratio = static_cast<double>(used_bytes) / static_cast<double>(total_reserved);
        if (usage_ratio > _heap_pressure_high_watermark) {
            target = RedletRMRPStrategy::PLUMP_PRUNE;
        } else if (usage_ratio < _heap_pressure_low_watermark) {
            target = RedletRMRPStrategy::SEQUENTIAL_INSTALLMENT;
        } else {
            target = _active_strategy.load(std::memory_order_relaxed);
        }
    }

    RedletRMRPStrategy old = _active_strategy.load(std::memory_order_relaxed);
    if (old != target) {
        _active_strategy.store(target, std::memory_order_release);
        _strategy_switches.fetch_add(1, std::memory_order_relaxed);
        char msg[128];
        snprintf(msg, sizeof(msg), "[Redlet RMRP Heuristic] Switched strategy: %s -> %s",
                 old == RedletRMRPStrategy::PLUMP_PRUNE ? "PLUMP_PRUNE" : "SEQUENTIAL",
                 target == RedletRMRPStrategy::PLUMP_PRUNE ? "PLUMP_PRUNE" : "SEQUENTIAL");
        sf_unified_log_emit(SF_Ebin0_RMRP_COALESCE_DECOMMIT, SF_Ebin1_INFO, 0, "rmrp_transition", msg);
        g_redlet_self_advance_telemetry.record_event(
            SF_EVENT_RMRP_TRANSITION, "rmrp_transition", msg, static_cast<double>(old), static_cast<double>(target));
    }
    return target;
}

void RedletRMRPHeuristics::reset() {
    _active_strategy.store(RedletRMRPStrategy::PLUMP_PRUNE, std::memory_order_relaxed);
    _strategy_switches.store(0, std::memory_order_relaxed);
    _total_bytes_reclaimed.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 4. RedletResurrectionHeuristics                                           */
/* ========================================================================= */

RedletResurrectionHeuristics::RedletResurrectionHeuristics() {
    reset();
}

uint32_t RedletResurrectionHeuristics::compute_quota(size_t young_alloc_rate_mbps, size_t landfill_used_bytes,
                                                     size_t landfill_total_bytes) {
    uint32_t quota = 32;

    if (young_alloc_rate_mbps > 500) {
        quota = 128;
    } else if (young_alloc_rate_mbps > 100) {
        quota = 64;
    }

    if (landfill_total_bytes > 0) {
        double fullness = static_cast<double>(landfill_used_bytes) / static_cast<double>(landfill_total_bytes);
        if (fullness > 0.85) {
            quota = std::min(quota, 16u);
        }
    }

    _resurrection_quota.store(quota, std::memory_order_relaxed);
    return quota;
}

bool RedletResurrectionHeuristics::should_admit_resurrection(uint32_t current_queue_size) noexcept {
    uint32_t max_q = _resurrection_quota.load(std::memory_order_relaxed);
    bool admit = current_queue_size < max_q;
    record_resurrection(admit);
    return admit;
}

void RedletResurrectionHeuristics::record_resurrection(bool admitted) noexcept {
    if (admitted) {
        _total_resurrections_approved.fetch_add(1, std::memory_order_relaxed);
    } else {
        _total_resurrections_rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

void RedletResurrectionHeuristics::query_counters(uint64_t* out_approved, uint64_t* out_rejected) const {
    if (out_approved) *out_approved = _total_resurrections_approved.load(std::memory_order_relaxed);
    if (out_rejected) *out_rejected = _total_resurrections_rejected.load(std::memory_order_relaxed);
}

void RedletResurrectionHeuristics::reset() {
    _resurrection_quota.store(32, std::memory_order_relaxed);
    _total_resurrections_approved.store(0, std::memory_order_relaxed);
    _total_resurrections_rejected.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 5. C EXPORT APIS                                                          */
/* ========================================================================= */

MSIMD_API int SF_self_advance_evolve_bitmap_kernel(void) {
    return g_redlet_bitmap_evolver.verify_and_evolve_best_kernel() ? 1 : 0;
}

MSIMD_API uint8_t SF_self_advance_get_active_bitmap_kernel(void) {
    return static_cast<uint8_t>(g_redlet_bitmap_evolver.get_active_kernel_type());
}

MSIMD_API void SF_self_advance_adaptive_memzero(void* dest, size_t bytes, int on_p_core, int high_pressure) {
    g_redlet_memreset_evolver.memzero_adaptive(dest, bytes, on_p_core != 0, high_pressure != 0);
}

MSIMD_API uint8_t SF_self_advance_evaluate_rmrp(size_t used_bytes, size_t committed_bytes,
                                                size_t total_reserved, double alloc_rate_mbps) {
    return static_cast<uint8_t>(g_redlet_rmrp_heuristics.evaluate_and_update(
        used_bytes, committed_bytes, total_reserved, alloc_rate_mbps));
}

MSIMD_API uint32_t SF_self_advance_compute_resurrect_quota(size_t young_rate_mbps,
                                                           size_t landfill_used, size_t landfill_total) {
    return g_redlet_resurrection_heuristics.compute_quota(young_rate_mbps, landfill_used, landfill_total);
}
