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

#ifndef SHARE_GC_REDJET_REDJETSRAMCACHE_HPP
#define SHARE_GC_REDJET_REDJETSRAMCACHE_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <vector>
#include <utility>
#include <algorithm>
#include <mutex>

#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/redjetHardware.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#if defined(_MSC_VER) || defined(__clang__)
#include <intrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* ========================================================================= */
/* 1. HARDWARE CACHE & SRAM STRUCTURES                                       */
/* ========================================================================= */

struct RedjetCacheLevel {
    uint32_t size_kb{ 0 };
    uint16_t line_size{ 64 };
    uint8_t  associativity{ 0 };
    uint8_t  type{ 0 }; // 1: Data, 2: Instruction, 3: Unified
    uint32_t num_sets{ 0 };
    bool     is_shared{ false };
};

struct RedjetCacheHierarchy {
    RedjetCacheLevel l1i;
    RedjetCacheLevel l1d;
    RedjetCacheLevel l2;
    RedjetCacheLevel l3;
    RedjetCacheLevel l4; // Intel Big LLC (bLLC) / Adamantine stacked SRAM

    uint16_t primary_cacheline_size{ 64 };
    bool     has_3d_vcache{ false };   // AMD 3D V-Cache (96MB - 192MB LLC stacked SRAM)
    bool     has_intel_bllc{ false };  // Intel Nova Lake / Clearwater Forest Big LLC (144MB - 288MB bLLC)
    bool     is_wide_cacheline{ false }; // Apple Silicon / Power (128-byte cache lines)

    // AMD Asymmetric Dual-CCD 3D V-Cache Topology (e.g. Ryzen 9 7950X3D / 9950X3D)
    bool     has_asymmetric_3d_vcache{ false }; // CCD0 has 3D V-Cache, CCD1 standard frequency
    uint64_t vcache_core_mask{ 0 };             // Logical thread affinity mask for 3D V-Cache CCD cores
    uint64_t frequency_core_mask{ 0 };          // Logical thread affinity mask for High-Frequency CCD cores
    uint32_t vcache_l3_slice_kb{ 0 };           // Stacked L3 SRAM slice size (e.g. 96MB)
    uint32_t standard_l3_slice_kb{ 0 };         // Standard L3 slice size (e.g. 32MB)

    char     brand_string[64]{ 0 };
    char     sram_description[128]{ 0 };
};

struct RedjetScratchpadMetrics {
    size_t   total_capacity{ 0 };
    size_t   used_bytes{ 0 };
    size_t   remaining_bytes{ 0 };
    size_t   peak_bytes{ 0 };
    uint64_t total_allocations{ 0 };
    uint64_t reset_count{ 0 };
    uint64_t failed_allocations{ 0 };
};

/* ========================================================================= */
/* 2. REDJET SRAM & CACHE ARCHITECTURE ENGINE                                */
/* ========================================================================= */

class RedjetSRAMCache {
public:
    // Architectural constants
    static constexpr size_t SCRATCHPAD_CAPACITY   = 8ULL * 1024 * 1024;  // 8 MB On-chip Zero-Latency Scratchpad
    static constexpr size_t BLLC_STREAM_THRESHOLD = 72ULL * 1024 * 1024; // 72 MB Non-temporal streaming threshold for Intel bLLC
    static constexpr size_t X3D_STREAM_THRESHOLD  = 48ULL * 1024 * 1024; // 48 MB Non-temporal streaming threshold for AMD 3D V-Cache
    static constexpr size_t DEFAULT_ALIGNMENT     = 64;                  // 64-byte primary cache line alignment

private:
    mutable std::mutex                  _init_mutex;
    RedjetCacheHierarchy                _hierarchy{};
    HS_custominiVm_sram_regime_t        _regime{ HS_SRAM_REGIME_STANDARD_HIERARCHICAL };

    uint8_t*                            _scratchpad_base{ nullptr };
    std::atomic<size_t>                 _scratchpad_offset{ 0 };
    std::atomic<size_t>                 _peak_scratchpad_offset{ 0 };
    std::atomic<uint64_t>               _total_allocations{ 0 };
    std::atomic<uint64_t>               _reset_count{ 0 };
    std::atomic<uint64_t>               _failed_allocations{ 0 };

    std::atomic<bool>                   _has_scratchpad{ false };
    std::atomic<bool>                   _is_initialized{ false };

    // Internal initialization helpers
    void allocate_scratchpad_memory();
    void release_scratchpad_memory();
    void pretouch_scratchpad();

public:
    RedjetSRAMCache() = default;
    ~RedjetSRAMCache() { cleanup(); }

    // Singleton instance access
    MSIMD_API static RedjetSRAMCache* instance();

    // Initialization & Lifecycle
    MSIMD_API void initialize(const hs_hardware_topology_t* topo = nullptr);
    MSIMD_API void cleanup();
    MSIMD_API bool is_initialized() const { return _is_initialized.load(std::memory_order_acquire); }

    // Hardware SRAM & Cache Detection
    MSIMD_API void detect_hardware_caches(const hs_hardware_topology_t* topo);
    MSIMD_API bool detect_amd_3d_vcache(const hs_hardware_topology_t* topo);
    MSIMD_API bool detect_intel_bllc(const hs_hardware_topology_t* topo);

    // Scratchpad Bump Allocator (Zero-wait-state, thread-safe, lock-free)
    MSIMD_API void* scratchpad_alloc(size_t bytes, size_t alignment = DEFAULT_ALIGNMENT);
    MSIMD_API void  scratchpad_reset();
    MSIMD_API RedjetScratchpadMetrics get_scratchpad_metrics() const;

    // Cache line geometry & alignment utilities
    MSIMD_API size_t pad_to_cacheline(size_t size) const;
    MSIMD_API void*  align_to_cacheline(void* ptr) const;
    MSIMD_API size_t get_cache_color_stride(uint32_t ways = 8) const;

    // Non-temporal Streaming Store heuristics
    MSIMD_API bool should_stream_store(size_t tensor_bytes) const;
    MSIMD_API void stream_store(void* dst, const void* src, size_t bytes) const;

    // Analytical BLIS Cache Blocking Dimension Solver
    MSIMD_API void compute_blis_tiles(size_t elem_size,
                                      size_t total_M, size_t total_N, size_t total_K,
                                      int& M_C, int& N_C, int& K_C,
                                      int& M_R, int& N_R) const;
    MSIMD_API void get_optimal_tile_sizes(size_t elem_size, int& tile_M, int& tile_N, int& tile_K) const;

    // Cacheline Invalidation, Volatility, & DRAM Commit
    MSIMD_API void commit_sram_to_dram(void* ptr, size_t bytes) const;
    MSIMD_API void broadcast_volatility(void* ptr, size_t bytes) const;

    // Query Accessors
    bool has_3d_vcache() const { return _hierarchy.has_3d_vcache; }
    bool has_intel_bllc() const { return _hierarchy.has_intel_bllc; }
    bool has_massive_llc() const { return _hierarchy.has_3d_vcache || _hierarchy.has_intel_bllc; }
    bool has_scratchpad_sram() const { return _has_scratchpad.load(std::memory_order_acquire); }
    HS_custominiVm_sram_regime_t regime() const { return _regime; }
    const RedjetCacheHierarchy& hierarchy() const { return _hierarchy; }

    // AMD Asymmetric Dual-CCD 3D V-Cache Helpers
    bool     has_asymmetric_3d_vcache() const { return _hierarchy.has_asymmetric_3d_vcache; }
    uint64_t vcache_core_mask() const { return _hierarchy.vcache_core_mask; }
    uint64_t frequency_core_mask() const { return _hierarchy.frequency_core_mask; }
    MSIMD_API bool is_core_on_vcache_ccd(uint32_t core_id) const;
    MSIMD_API bool bind_current_thread_to_vcache_ccd() const;
    MSIMD_API bool bind_current_thread_to_frequency_ccd() const;

    uint8_t* scratchpad_base() const { return _scratchpad_base; }
    size_t   scratchpad_capacity() const { return SCRATCHPAD_CAPACITY; }
    size_t   scratchpad_used() const { return _scratchpad_offset.load(std::memory_order_relaxed); }
    size_t   scratchpad_remaining() const {
        size_t used = _scratchpad_offset.load(std::memory_order_relaxed);
        return (used < SCRATCHPAD_CAPACITY) ? (SCRATCHPAD_CAPACITY - used) : 0;
    }
    size_t   scratchpad_peak() const { return _peak_scratchpad_offset.load(std::memory_order_relaxed); }

    const char* get_sram_description() const { return _hierarchy.sram_description; }
    const char* get_brand_string() const { return _hierarchy.brand_string; }

    // Static Memory & Cache Control Primitives
    static inline void flush_wc_buffers() {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_sfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
#else
        std::atomic_thread_fence(std::memory_order_release);
#endif
        hs_hotlog_emit(HS_Ebin0_STREAMING_STORE_FLUSH, HS_Ebin1_TRACE, 0, 0, 0, 0, 0, 0, 0, "wc_buffers_flushed");
    }

    static inline void prefetch_t0(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_prefetch((const char*)ptr, _MM_HINT_T0);
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __builtin_prefetch(ptr, 0, 3);
#endif
#else
        (void)ptr;
#endif
    }

    static inline void prefetch_t1(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_prefetch((const char*)ptr, _MM_HINT_T1);
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __builtin_prefetch(ptr, 0, 2);
#endif
#else
        (void)ptr;
#endif
    }

    static inline void prefetch_t2(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_prefetch((const char*)ptr, _MM_HINT_T2);
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __builtin_prefetch(ptr, 0, 1);
#endif
#else
        (void)ptr;
#endif
    }

    static inline void prefetch_nta(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_prefetch((const char*)ptr, _MM_HINT_NTA);
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __builtin_prefetch(ptr, 0, 0);
#endif
#else
        (void)ptr;
#endif
    }

    static inline void clflush(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_clflush(ptr);
#else
        (void)ptr;
#endif
    }

    static inline void clflushopt(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
#if defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_clflushopt(const_cast<void*>(ptr));
#elif defined(_MSC_VER)
        _mm_clflushopt(const_cast<void*>(ptr));
#else
        _mm_clflush(const_cast<void*>(ptr));
#endif
#else
        (void)ptr;
#endif
    }

    static inline void clwb(const void* ptr) {
#if defined(_M_X64) || defined(__x86_64__)
#if defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_clwb(const_cast<void*>(ptr));
#elif defined(_MSC_VER)
        _mm_clwb(const_cast<void*>(ptr));
#else
        _mm_clflush(const_cast<void*>(ptr));
#endif
#else
        (void)ptr;
#endif
    }

    static inline void sfence() {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_sfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
#else
        std::atomic_thread_fence(std::memory_order_release);
#endif
    }

    static inline void lfence() {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_lfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __asm__ __volatile__("dmb ishld" ::: "memory");
#endif
#else
        std::atomic_thread_fence(std::memory_order_acquire);
#endif
    }

    static inline void mfence() {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_mfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
        __asm__ __volatile__("dmb ish" ::: "memory");
#endif
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    }

    // Engine Self-Diagnostics
    MSIMD_API bool run_diagnostics();
};

#endif // SHARE_GC_REDJET_REDJETSRAMCACHE_HPP
