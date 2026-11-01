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

#ifndef SHARE_GC_REDLET_SELF_ADVANCE_REDLETSELFADVANCEHEURISTICS_HPP
#define SHARE_GC_REDLET_SELF_ADVANCE_REDLETSELFADVANCEHEURISTICS_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletSIMD.hpp"
#include "gc/redlet/self_advance/redletSelfAdvanceHooks.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. CARD TABLE & BITMAP SCAN KERNELS & EVOLUTION                           */
/* ========================================================================= */

typedef size_t (*redlet_bitmap_scan_fn_t)(const uintptr_t* bitmap, size_t word_count,
                                          size_t* out_marked_indices, size_t max_indices);

enum class RedletBitmapScanKernel : uint8_t {
    SCALAR_BASELINE  = 0,
    BMI2_ACCELERATED = 1,
    AVX2_MASKED      = 2,
    AVX512_MASKED    = 3
};

struct RedletBitmapEvalResult {
    RedletBitmapScanKernel kernel{ RedletBitmapScanKernel::SCALAR_BASELINE };
    bool     bit_exact_pass{ false };
    uint64_t elapsed_cycles{ 0 };
    size_t   marks_found{ 0 };
    char     kernel_name[32]{ 0 };
};

class RedletCardTableBitmapEvolver {
private:
    std::atomic<RedletBitmapScanKernel>  _active_kernel_type{ RedletBitmapScanKernel::SCALAR_BASELINE };
    std::atomic<redlet_bitmap_scan_fn_t> _active_kernel{ nullptr };
    std::atomic<uint64_t>                _evolution_count{ 0 };
    std::atomic<uint64_t>                _exact_matches_verified{ 0 };
    std::atomic<uint64_t>                _divergences_rejected{ 0 };

public:
    RedletCardTableBitmapEvolver();

    // Baseline reference scanner (deterministic gold standard)
    static size_t scan_scalar(const uintptr_t* bitmap, size_t word_count,
                              size_t* out_marked_indices, size_t max_indices);

    // BMI2 accelerated scanner using POPCNT, TZCNT, BLSR
    static size_t scan_bmi2(const uintptr_t* bitmap, size_t word_count,
                            size_t* out_marked_indices, size_t max_indices);

    // AVX2 vectorized masked scanner
    static size_t scan_avx2(const uintptr_t* bitmap, size_t word_count,
                            size_t* out_marked_indices, size_t max_indices);

    // AVX-512 vectorized masked scanner
    static size_t scan_avx512(const uintptr_t* bitmap, size_t word_count,
                              size_t* out_marked_indices, size_t max_indices);

    // Run verification experiment: must achieve exact 0-bit divergence against scalar
    bool verify_and_evolve_best_kernel();

    // Execute active scan kernel
    inline size_t scan(const uintptr_t* bitmap, size_t word_count,
                       size_t* out_marked_indices, size_t max_indices) const {
        auto fn = _active_kernel.load(std::memory_order_relaxed);
        return fn ? fn(bitmap, word_count, out_marked_indices, max_indices)
                  : scan_scalar(bitmap, word_count, out_marked_indices, max_indices);
    }

    RedletBitmapScanKernel get_active_kernel_type() const noexcept {
        return _active_kernel_type.load(std::memory_order_relaxed);
    }

    uint64_t get_evolution_count() const noexcept {
        return _evolution_count.load(std::memory_order_relaxed);
    }

    uint64_t get_exact_matches_verified() const noexcept {
        return _exact_matches_verified.load(std::memory_order_relaxed);
    }

    uint64_t get_divergences_rejected() const noexcept {
        return _divergences_rejected.load(std::memory_order_relaxed);
    }

    void reset();
};

/* ========================================================================= */
/* 2. TRASHPIT & LANDFILL RESET (MEMZERO / MEMCPY) HEURISTICS                */
/* ========================================================================= */

typedef void (*redlet_memzero_fn_t)(void* dest, size_t bytes);

enum class RedletMemzeroStrategy : uint8_t {
    LIBC_MEMSET          = 0,
    STREAMING_STORES_NT  = 1,
    ECORE_CHUNKED        = 2
};

class RedletMemoryResetEvolver {
private:
    std::atomic<RedletMemzeroStrategy> _active_strategy{ RedletMemzeroStrategy::LIBC_MEMSET };
    std::atomic<uint64_t>              _nt_stores_invoked{ 0 };
    std::atomic<uint64_t>              _chunked_stores_invoked{ 0 };
    std::atomic<uint64_t>              _libc_memset_invoked{ 0 };

public:
    RedletMemoryResetEvolver();

    // Static implementations
    static void zero_libc(void* dest, size_t bytes);
    static void zero_streaming_nt(void* dest, size_t bytes);
    static void zero_ecore_chunked(void* dest, size_t bytes);

    // Dynamic selection based on cache pressure, region size, and core tier
    void memzero_adaptive(void* dest, size_t bytes, bool on_p_core, bool high_cache_pressure);

    RedletMemzeroStrategy get_active_strategy() const noexcept {
        return _active_strategy.load(std::memory_order_relaxed);
    }

    void query_counters(uint64_t* out_nt, uint64_t* out_chunk, uint64_t* out_libc) const;
    void reset();
};

/* ========================================================================= */
/* 3. DYNAMIC RMRP (REDUCE MEMORY RESIDENT PROPORTION) STRATEGY             */
/* ========================================================================= */

enum class RedletRMRPStrategy : uint8_t {
    SEQUENTIAL_INSTALLMENT = 0, // Gradual reclamation, minimal latency/jitter
    PLUMP_PRUNE            = 1  // Batch coalesce and decommit, maximum RSS reduction
};

class RedletRMRPHeuristics {
private:
    std::atomic<RedletRMRPStrategy> _active_strategy{ RedletRMRPStrategy::PLUMP_PRUNE };
    std::atomic<uint64_t>           _strategy_switches{ 0 };
    std::atomic<size_t>             _total_bytes_reclaimed{ 0 };

    double _heap_pressure_high_watermark{ 0.80 }; // >80% heap used -> Plump
    double _heap_pressure_low_watermark{ 0.40 };  // <40% heap used -> Sequential

public:
    RedletRMRPHeuristics();

    // Evaluates memory pressure, RSS, and allocation rate to pick strategy
    RedletRMRPStrategy evaluate_and_update(size_t used_bytes, size_t committed_bytes,
                                           size_t total_reserved, double alloc_rate_mbps);

    RedletRMRPStrategy get_current_strategy() const noexcept {
        return _active_strategy.load(std::memory_order_relaxed);
    }

    void record_reclamation(size_t bytes) noexcept {
        _total_bytes_reclaimed.fetch_add(bytes, std::memory_order_relaxed);
    }

    size_t get_total_reclaimed() const noexcept {
        return _total_bytes_reclaimed.load(std::memory_order_relaxed);
    }

    uint64_t get_strategy_switch_count() const noexcept {
        return _strategy_switches.load(std::memory_order_relaxed);
    }

    void reset();
};

/* ========================================================================= */
/* 4. RESURRECTION HEURISTICS (REHAB ARENA CHURN REDUCTION)                  */
/* ========================================================================= */

class RedletResurrectionHeuristics {
private:
    std::atomic<uint32_t> _resurrection_quota{ 32 };
    std::atomic<uint64_t> _total_resurrections_approved{ 0 };
    std::atomic<uint64_t> _total_resurrections_rejected{ 0 };

public:
    RedletResurrectionHeuristics();

    // Computes optimal resurrection quota based on allocation velocity and landfill status
    uint32_t compute_quota(size_t young_alloc_rate_mbps, size_t landfill_used_bytes,
                           size_t landfill_total_bytes);

    bool should_admit_resurrection(uint32_t current_queue_size) noexcept;
    void record_resurrection(bool admitted) noexcept;

    uint32_t get_active_quota() const noexcept {
        return _resurrection_quota.load(std::memory_order_relaxed);
    }

    void query_counters(uint64_t* out_approved, uint64_t* out_rejected) const;
    void reset();
};

/* ========================================================================= */
/* 5. GLOBAL INSTANCES & C EXPORT APIS                                       */
/* ========================================================================= */

extern RedletCardTableBitmapEvolver g_redlet_bitmap_evolver;
extern RedletMemoryResetEvolver     g_redlet_memreset_evolver;
extern RedletRMRPHeuristics         g_redlet_rmrp_heuristics;
extern RedletResurrectionHeuristics g_redlet_resurrection_heuristics;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API int      SF_self_advance_evolve_bitmap_kernel(void);
MSIMD_API uint8_t  SF_self_advance_get_active_bitmap_kernel(void);
MSIMD_API void     SF_self_advance_adaptive_memzero(void* dest, size_t bytes, int on_p_core, int high_pressure);
MSIMD_API uint8_t  SF_self_advance_evaluate_rmrp(size_t used_bytes, size_t committed_bytes,
                                                 size_t total_reserved, double alloc_rate_mbps);
MSIMD_API uint32_t SF_self_advance_compute_resurrect_quota(size_t young_rate_mbps,
                                                           size_t landfill_used, size_t landfill_total);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_SELF_ADVANCE_REDLETSELFADVANCEHEURISTICS_HPP
