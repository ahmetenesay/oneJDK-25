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

#include "gc/redjet/allocator/redjetSRAMCache.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#endif

/* ========================================================================= */
/* 1. SINGLETON INSTANCE & STATIC STATE                                      */
/* ========================================================================= */

static RedjetSRAMCache s_sram_cache_instance;

RedjetSRAMCache* RedjetSRAMCache::instance() {
    if (!s_sram_cache_instance.is_initialized()) {
        s_sram_cache_instance.initialize(nullptr);
    }
    return &s_sram_cache_instance;
}

/* ========================================================================= */
/* 2. SCRATCHPAD MEMORY MANAGEMENT (ZERO-WAIT-STATE OS PRE-COMMIT)           */
/* ========================================================================= */

void RedjetSRAMCache::allocate_scratchpad_memory() {
    if (_scratchpad_base) return;

#if defined(_WIN32)
    _scratchpad_base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, SCRATCHPAD_CAPACITY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    );
#elif defined(__linux__) || defined(__APPLE__)
    _scratchpad_base = static_cast<uint8_t*>(
        mmap(nullptr, SCRATCHPAD_CAPACITY, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
    );
    if (_scratchpad_base == MAP_FAILED) {
        _scratchpad_base = nullptr;
    }
#else
    _scratchpad_base = static_cast<uint8_t*>(malloc(SCRATCHPAD_CAPACITY));
#endif

    if (_scratchpad_base) {
        _has_scratchpad.store(true, std::memory_order_release);
        pretouch_scratchpad();
    } else {
        _has_scratchpad.store(false, std::memory_order_release);
    }
    _scratchpad_offset.store(0, std::memory_order_relaxed);
    _peak_scratchpad_offset.store(0, std::memory_order_relaxed);
}

void RedjetSRAMCache::release_scratchpad_memory() {
    if (!_scratchpad_base) return;

#if defined(_WIN32)
    VirtualFree(_scratchpad_base, 0, MEM_RELEASE);
#elif defined(__linux__) || defined(__APPLE__)
    munmap(_scratchpad_base, SCRATCHPAD_CAPACITY);
#else
    free(_scratchpad_base);
#endif

    _scratchpad_base = nullptr;
    _has_scratchpad.store(false, std::memory_order_release);
    _scratchpad_offset.store(0, std::memory_order_relaxed);
}

void RedjetSRAMCache::pretouch_scratchpad() {
    if (!_scratchpad_base) return;
    // Pre-fault each 4KB physical page to eliminate page faults during execution
    for (size_t off = 0; off < SCRATCHPAD_CAPACITY; off += 4096) {
        volatile uint8_t* p = _scratchpad_base + off;
        *p = 0;
    }
    flush_wc_buffers();
}

/* ========================================================================= */
/* 3. HARDWARE SRAM & CACHE HIERARCHY DETECTION                              */
/* ========================================================================= */

void RedjetSRAMCache::detect_hardware_caches(const hs_hardware_topology_t* topo) {
    std::memset(&_hierarchy, 0, sizeof(_hierarchy));
    _hierarchy.primary_cacheline_size = 64;

    // 1. Inspect processor brand string and vendor string via CPUID
    char vendor[16] = { 0 };
#if defined(_M_X64) || defined(__x86_64__)
    int cpuInfo[4] = { 0 };
    hs_get_cpuid(cpuInfo, 0, 0);
    std::memcpy(vendor + 0, &cpuInfo[1], 4);
    std::memcpy(vendor + 4, &cpuInfo[3], 4);
    std::memcpy(vendor + 8, &cpuInfo[2], 4);
    vendor[12] = '\0';

    hs_get_cpuid(cpuInfo, 0x80000000, 0);
    unsigned int max_ext = static_cast<unsigned int>(cpuInfo[0]);
    if (max_ext >= 0x80000004) {
        int brandInfo[12] = { 0 };
        hs_get_cpuid(&brandInfo[0], 0x80000002, 0);
        hs_get_cpuid(&brandInfo[4], 0x80000003, 0);
        hs_get_cpuid(&brandInfo[8], 0x80000004, 0);
        std::memcpy(_hierarchy.brand_string, brandInfo, 48);
        _hierarchy.brand_string[48] = '\0';
    }
#endif

    // 2. Query basic topology values
    if (topo) {
        _hierarchy.l1d.size_kb = topo->l1d.size_kb > 0 ? topo->l1d.size_kb : 32;
        _hierarchy.l1d.line_size = topo->l1d.line_size > 0 ? topo->l1d.line_size : 64;
        _hierarchy.l1d.associativity = topo->l1d.associativity;
        _hierarchy.l1d.type = 1;

        _hierarchy.l1i.size_kb = topo->l1i.size_kb > 0 ? topo->l1i.size_kb : 32;
        _hierarchy.l1i.line_size = topo->l1i.line_size > 0 ? topo->l1i.line_size : 64;
        _hierarchy.l1i.associativity = topo->l1i.associativity;
        _hierarchy.l1i.type = 2;

        _hierarchy.l2.size_kb = topo->l2.size_kb > 0 ? topo->l2.size_kb : 512;
        _hierarchy.l2.line_size = topo->l2.line_size > 0 ? topo->l2.line_size : 64;
        _hierarchy.l2.associativity = topo->l2.associativity;
        _hierarchy.l2.type = 3;

        _hierarchy.l3.size_kb = topo->l3.size_kb > 0 ? topo->l3.size_kb : 16384;
        _hierarchy.l3.line_size = topo->l3.line_size > 0 ? topo->l3.line_size : 64;
        _hierarchy.l3.associativity = topo->l3.associativity;
        _hierarchy.l3.type = 3;

        _hierarchy.primary_cacheline_size = topo->primary_cacheline_size > 0 ? topo->primary_cacheline_size : 64;
    } else {
        _hierarchy.l1d.size_kb = 32;
        _hierarchy.l1i.size_kb = 32;
        _hierarchy.l2.size_kb = 512;
        _hierarchy.l3.size_kb = 16384;
        _hierarchy.primary_cacheline_size = 64;
    }

    // 3. Detect AMD 3D V-Cache and Intel Big LLC (bLLC)
    _hierarchy.has_3d_vcache = detect_amd_3d_vcache(topo);
    _hierarchy.has_intel_bllc = detect_intel_bllc(topo);

    // If 3D V-Cache detected, ensure L3 size accurately reflects at least 96MB per 3D CCD
    if (_hierarchy.has_3d_vcache) {
        if (_hierarchy.l3.size_kb < 96 * 1024) {
            _hierarchy.l3.size_kb = 96 * 1024;
        }
        _hierarchy.vcache_l3_slice_kb = 96 * 1024;
        _hierarchy.standard_l3_slice_kb = 32 * 1024;

        // Check for asymmetric dual-CCD (e.g. Ryzen 9 7950X3D/9950X3D: 16 cores, CCD0 has 3D V-Cache, CCD1 standard)
        uint32_t phys_cores = topo ? topo->physical_cores : 8;
        uint32_t log_cores = topo ? topo->logical_cores : phys_cores;
        bool brand_asymm = (std::strstr(_hierarchy.brand_string, "7950X3D") != nullptr ||
                            std::strstr(_hierarchy.brand_string, "7900X3D") != nullptr ||
                            std::strstr(_hierarchy.brand_string, "9950X3D") != nullptr ||
                            std::strstr(_hierarchy.brand_string, "9900X3D") != nullptr);
        if (brand_asymm || phys_cores > 8) {
            _hierarchy.has_asymmetric_3d_vcache = true;
            uint32_t vcache_threads = log_cores / 2;
            _hierarchy.vcache_core_mask = (vcache_threads >= 64) ? ~0ULL : ((1ULL << vcache_threads) - 1);
            _hierarchy.frequency_core_mask = (log_cores >= 64 ? ~0ULL : ((1ULL << log_cores) - 1)) & ~_hierarchy.vcache_core_mask;
        } else {
            _hierarchy.has_asymmetric_3d_vcache = false;
            _hierarchy.vcache_core_mask = log_cores >= 64 ? ~0ULL : ((1ULL << log_cores) - 1);
            _hierarchy.frequency_core_mask = 0;
        }
    }

    // If Intel bLLC detected, ensure L3/bLLC size accurately reflects at least 144MB
    if (_hierarchy.has_intel_bllc) {
        if (_hierarchy.l3.size_kb < 144 * 1024) {
            _hierarchy.l3.size_kb = 144 * 1024;
        }
    }

    // 4. Classify SRAM Regime
    if (_hierarchy.primary_cacheline_size >= 128) {
        _hierarchy.is_wide_cacheline = true;
        _regime = HS_SRAM_REGIME_WIDE_CACHELINE_128B;
        std::snprintf(_hierarchy.sram_description, sizeof(_hierarchy.sram_description),
                      "Wide-Cacheline 128B Regime (L3: %u KB, Line: %u B)",
                      _hierarchy.l3.size_kb, _hierarchy.primary_cacheline_size);
    } else if (_hierarchy.has_intel_bllc) {
        _regime = HS_SRAM_REGIME_3D_VCACHE;
        uint32_t rep_kb = _hierarchy.l4.size_kb > 0 ? _hierarchy.l4.size_kb : _hierarchy.l3.size_kb;
        std::snprintf(_hierarchy.sram_description, sizeof(_hierarchy.sram_description),
                      "Intel Nova Lake / Big LLC (bLLC) Massive SRAM (%u KB bLLC Active)",
                      rep_kb);
    } else if (_hierarchy.has_3d_vcache) {
        _regime = HS_SRAM_REGIME_3D_VCACHE;
        if (_hierarchy.has_asymmetric_3d_vcache) {
            std::snprintf(_hierarchy.sram_description, sizeof(_hierarchy.sram_description),
                          "AMD Asymmetric 3D V-Cache (L3: %u KB, CCD0: 96MB V-Cache, CCD1: 32MB Freq)",
                          _hierarchy.l3.size_kb);
        } else {
            std::snprintf(_hierarchy.sram_description, sizeof(_hierarchy.sram_description),
                          "AMD 3D V-Cache Stacked SRAM (L3: %u KB, 3D V-Cache Active)",
                          _hierarchy.l3.size_kb);
        }
    } else {
        _regime = HS_SRAM_REGIME_STANDARD_HIERARCHICAL;
        std::snprintf(_hierarchy.sram_description, sizeof(_hierarchy.sram_description),
                      "Standard Hierarchical Cache (L1D: %u KB, L2: %u KB, L3: %u KB)",
                      _hierarchy.l1d.size_kb, _hierarchy.l2.size_kb, _hierarchy.l3.size_kb);
    }
}

bool RedjetSRAMCache::detect_amd_3d_vcache(const hs_hardware_topology_t* topo) {
    const char* force_env = getenv("REDJET_FORCE_AMD_X3D");
    if (force_env && (std::strcmp(force_env, "1") == 0 || std::strcmp(force_env, "true") == 0)) {
        return true;
    }

    // 1. Check brand string for "X3D" or "3D V-Cache"
    if (std::strstr(_hierarchy.brand_string, "X3D") != nullptr ||
        std::strstr(_hierarchy.brand_string, "3D V-Cache") != nullptr ||
        std::strstr(_hierarchy.brand_string, "x3d") != nullptr) {
        return true;
    }

    // 2. Query AMD Extended Cache Topology CPUID leaf 0x8000001D
#if defined(_M_X64) || defined(__x86_64__)
    int cpuInfo[4] = { 0 };
    hs_get_cpuid(cpuInfo, 0, 0);
    char vendor[16] = { 0 };
    std::memcpy(vendor + 0, &cpuInfo[1], 4);
    std::memcpy(vendor + 4, &cpuInfo[3], 4);
    std::memcpy(vendor + 8, &cpuInfo[2], 4);
    bool is_amd_vendor = (std::strstr(vendor, "AuthenticAMD") != nullptr ||
                          std::strstr(_hierarchy.brand_string, "AMD") != nullptr ||
                          std::strstr(_hierarchy.brand_string, "Ryzen") != nullptr ||
                          std::strstr(_hierarchy.brand_string, "EPYC") != nullptr);

    if (is_amd_vendor) {
        hs_get_cpuid(cpuInfo, 0x80000000, 0);
        unsigned int max_ext = static_cast<unsigned int>(cpuInfo[0]);
        if (max_ext >= 0x8000001D) {
            for (int sub = 0; sub < 8; ++sub) {
                hs_get_cpuid(cpuInfo, 0x8000001D, sub);
                int cache_type = cpuInfo[0] & 0x1F;
                if (cache_type == 0) break;
                int level = (cpuInfo[0] >> 5) & 0x7;
                if (level == 3) {
                    uint32_t line_size = (cpuInfo[1] & 0xFFF) + 1;
                    uint32_t partitions = ((cpuInfo[1] >> 12) & 0x3FF) + 1;
                    uint32_t ways = ((cpuInfo[1] >> 22) & 0x3FF) + 1;
                    uint32_t sets = cpuInfo[2] + 1;
                    uint64_t l3_bytes = static_cast<uint64_t>(line_size) * partitions * ways * sets;
                    uint32_t l3_kb = static_cast<uint32_t>(l3_bytes / 1024);
                    // AMD 3D V-Cache CCDs feature 64MB or 96MB of stacked L3 SRAM (per CCD)
                    // Any single CCD slice >= 48MB indicates 3D V-Cache
                    if (l3_kb >= 48 * 1024) {
                        _hierarchy.l3.size_kb = l3_kb;
                        return true;
                    }
                }
            }
        }
    }
#endif

    // 3. Fallback to topology L3 threshold for AMD CPUs (>= 48MB)
    if (topo && topo->l3.size_kb >= 48 * 1024) {
#if defined(_M_X64) || defined(__x86_64__)
        if (is_amd_vendor) {
            return true;
        }
#endif
    }

    return false;
}

bool RedjetSRAMCache::detect_intel_bllc(const hs_hardware_topology_t* topo) {
    const char* force_bllc = getenv("REDJET_FORCE_INTEL_BLLC");
    if (force_bllc && (std::strcmp(force_bllc, "1") == 0 || std::strcmp(force_bllc, "true") == 0)) {
        return true;
    }

    // Verify vendor is Intel before claiming Intel bLLC
#if defined(_M_X64) || defined(__x86_64__)
    int cpuInfo[4] = { 0 };
    hs_get_cpuid(cpuInfo, 0, 0);
    char vendor[16] = { 0 };
    std::memcpy(vendor + 0, &cpuInfo[1], 4);
    std::memcpy(vendor + 4, &cpuInfo[3], 4);
    std::memcpy(vendor + 8, &cpuInfo[2], 4);
    bool is_intel_vendor = (std::strstr(vendor, "GenuineIntel") != nullptr ||
                            std::strstr(_hierarchy.brand_string, "Intel") != nullptr);
    if (!is_intel_vendor && std::strstr(_hierarchy.brand_string, "AMD") != nullptr) {
        return false;
    }

    int max_leaf = cpuInfo[0];
    if (max_leaf >= 4) {
        for (int sub = 0; sub < 16; ++sub) {
            hs_get_cpuid(cpuInfo, 4, sub);
            int cache_type = cpuInfo[0] & 0x1F;
            if (cache_type == 0) break;
            int level = (cpuInfo[0] >> 5) & 0x7;
            uint32_t line_size = (cpuInfo[1] & 0xFFF) + 1;
            uint32_t partitions = ((cpuInfo[1] >> 12) & 0x3FF) + 1;
            uint32_t ways = ((cpuInfo[1] >> 22) & 0x3FF) + 1;
            uint32_t sets = cpuInfo[2] + 1;
            uint64_t cache_bytes = static_cast<uint64_t>(line_size) * partitions * ways * sets;
            uint32_t cache_kb = static_cast<uint32_t>(cache_bytes / 1024);

            if (level == 4 && cache_kb >= 64 * 1024) {
                _hierarchy.l4.size_kb = cache_kb;
                _hierarchy.l4.line_size = static_cast<uint16_t>(line_size);
                _hierarchy.l4.associativity = static_cast<uint8_t>(ways);
                _hierarchy.l4.type = 3;
                return true;
            }
            if (level == 3 && cache_kb >= 130 * 1024) {
                _hierarchy.l3.size_kb = cache_kb;
                return true;
            }
        }
    }
#endif

    // Topology L3 threshold check for Intel
    if (topo && topo->l3.size_kb >= 130 * 1024) {
#if defined(_M_X64) || defined(__x86_64__)
        if (is_intel_vendor) {
            return true;
        }
#else
        return true;
#endif
    }

    return false;
}

/* ========================================================================= */
/* 4. INITIALIZATION & LIFECYCLE                                             */
/* ========================================================================= */

void RedjetSRAMCache::initialize(const hs_hardware_topology_t* topo) {
    if (_is_initialized.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(_init_mutex);
    if (_is_initialized.load(std::memory_order_relaxed)) return;

    detect_hardware_caches(topo);
    allocate_scratchpad_memory();
    _is_initialized.store(true, std::memory_order_release);
}

void RedjetSRAMCache::cleanup() {
    std::lock_guard<std::mutex> lock(_init_mutex);
    release_scratchpad_memory();
    _is_initialized.store(false, std::memory_order_release);
}

/* ========================================================================= */
/* 5. SCRATCHPAD BUMP ALLOCATOR (ZERO-WAIT-STATE ON-CHIP SRAM)               */
/* ========================================================================= */

void* RedjetSRAMCache::scratchpad_alloc(size_t bytes, size_t alignment) {
    if (!_scratchpad_base || bytes == 0 || bytes > SCRATCHPAD_CAPACITY) {
        if (bytes > 0) _failed_allocations.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    if (alignment == 0) alignment = DEFAULT_ALIGNMENT;
    // Enforce power-of-2 alignment to guarantee bitwise mask validity
    if ((alignment & (alignment - 1)) != 0) {
        size_t p = 1;
        while (p < alignment) p <<= 1;
        alignment = p;
    }
    if (alignment > SCRATCHPAD_CAPACITY) {
        _failed_allocations.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    size_t cur = _scratchpad_offset.load(std::memory_order_relaxed);
    while (true) {
        if (cur > (SIZE_MAX - alignment)) {
            _failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        size_t aligned = (cur + alignment - 1) & ~(alignment - 1);
        if (aligned > (SIZE_MAX - bytes)) {
            _failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        size_t next = aligned + bytes;
        if (next > SCRATCHPAD_CAPACITY) {
            _failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (_scratchpad_offset.compare_exchange_weak(cur, next,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            _total_allocations.fetch_add(1, std::memory_order_relaxed);
            size_t peak = _peak_scratchpad_offset.load(std::memory_order_relaxed);
            while (next > peak &&
                   !_peak_scratchpad_offset.compare_exchange_weak(peak, next, std::memory_order_relaxed)) {}

            hs_hotlog_emit(HS_Ebin0_SCRATCHPAD_ALLOC, HS_Ebin1_TRACE, 0, static_cast<uint16_t>(alignment),
                           0, 0, 0, 0, static_cast<uint32_t>(bytes / 1024), "sram_scratch_alloc");
            return _scratchpad_base + aligned;
        }
    }
}

void RedjetSRAMCache::scratchpad_reset() {
    _scratchpad_offset.store(0, std::memory_order_release);
    _reset_count.fetch_add(1, std::memory_order_relaxed);
}

RedjetScratchpadMetrics RedjetSRAMCache::get_scratchpad_metrics() const {
    RedjetScratchpadMetrics m{};
    m.total_capacity = SCRATCHPAD_CAPACITY;
    m.used_bytes = _scratchpad_offset.load(std::memory_order_relaxed);
    m.remaining_bytes = (m.used_bytes < SCRATCHPAD_CAPACITY) ? (SCRATCHPAD_CAPACITY - m.used_bytes) : 0;
    m.peak_bytes = _peak_scratchpad_offset.load(std::memory_order_relaxed);
    m.total_allocations = _total_allocations.load(std::memory_order_relaxed);
    m.reset_count = _reset_count.load(std::memory_order_relaxed);
    m.failed_allocations = _failed_allocations.load(std::memory_order_relaxed);
    return m;
}

/* ========================================================================= */
/* 6. GEOMETRY & CACHELINE ALIGNMENT                                         */
/* ========================================================================= */

size_t RedjetSRAMCache::pad_to_cacheline(size_t size) const {
    size_t cl = _hierarchy.primary_cacheline_size > 0 ? _hierarchy.primary_cacheline_size : 64;
    if (size > (size_t)-1 - cl) return size;
    return (size + cl - 1) & ~(cl - 1);
}

void* RedjetSRAMCache::align_to_cacheline(void* ptr) const {
    size_t cl = _hierarchy.primary_cacheline_size > 0 ? _hierarchy.primary_cacheline_size : 64;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    return reinterpret_cast<void*>((addr + cl - 1) & ~(cl - 1));
}

size_t RedjetSRAMCache::get_cache_color_stride(uint32_t ways) const {
    if (ways == 0) ways = 8;
    uint32_t l1d_kb = _hierarchy.l1d.size_kb > 0 ? _hierarchy.l1d.size_kb : 32;
    return (static_cast<size_t>(l1d_kb) * 1024ULL) / ways;
}

/* ========================================================================= */
/* 7. NON-TEMPORAL STREAMING STORE HEURISTICS                                */
/* ========================================================================= */

bool RedjetSRAMCache::should_stream_store(size_t tensor_bytes) const {
    size_t sram_threshold;
    if (_hierarchy.has_intel_bllc) {
        sram_threshold = BLLC_STREAM_THRESHOLD; // 72 MB
    } else if (_hierarchy.has_3d_vcache) {
        sram_threshold = X3D_STREAM_THRESHOLD;  // 48 MB
    } else {
        size_t l3_bytes = (_hierarchy.l3.size_kb > 0 ? _hierarchy.l3.size_kb : 16384) * 1024ULL;
        sram_threshold = l3_bytes / 2;
    }
    return tensor_bytes > sram_threshold;
}

void RedjetSRAMCache::stream_store(void* dst, const void* src, size_t bytes) const {
    hs_dram_stream_store(dst, src, bytes);
}

/* ========================================================================= */
/* 8. ANALYTICAL BLIS CACHE BLOCKING DIMENSION SOLVER                         */
/* ========================================================================= */

void RedjetSRAMCache::compute_blis_tiles(size_t elem_size,
                                         size_t total_M, size_t total_N, size_t total_K,
                                         int& M_C, int& N_C, int& K_C,
                                         int& M_R, int& N_R) const {
    if (elem_size == 0) elem_size = sizeof(float);

    // Register micro-tile dimensions
#if defined(_M_X64) || defined(__x86_64__)
    int cinfo[4] = { 0 };
    hs_get_cpuid(cinfo, 0, 0);
    if (cinfo[0] >= 7) {
        hs_get_cpuid(cinfo, 7, 0);
        if (cinfo[1] & (1 << 16)) { // AVX-512F
            M_R = 16;
            N_R = 8;
        } else if (cinfo[1] & (1 << 5)) { // AVX2
            M_R = 8;
            N_R = 6;
        } else {
            M_R = 4;
            N_R = 4;
        }
    } else {
        M_R = 4;
        N_R = 4;
    }
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    M_R = 8;
    N_R = 4;
#else
    M_R = 4;
    N_R = 4;
#endif

    // 1. K_C block size: Pack panel A (M_C x K_C) in L1D cache (50% target)
    size_t l1d_bytes = (_hierarchy.l1d.size_kb > 0 ? _hierarchy.l1d.size_kb : 32) * 1024ULL;
    size_t l1d_target = l1d_bytes / 2;
    int kc_calc = static_cast<int>(l1d_target / (static_cast<size_t>(M_R) * elem_size));
    if (kc_calc < 64)  kc_calc = 64;
    if (kc_calc > 512) kc_calc = 512;
    kc_calc = (kc_calc / 4) * 4;
    if (kc_calc == 0) kc_calc = 4;
    K_C = static_cast<int>((std::min)(static_cast<size_t>(kc_calc),
                                      total_K > 0 ? total_K : static_cast<size_t>(kc_calc)));

    // 2. M_C block size: Block of A (M_C x K_C) fits in L2 cache (60% target)
    size_t l2_bytes = (_hierarchy.l2.size_kb > 0 ? _hierarchy.l2.size_kb : 512) * 1024ULL;
    size_t l2_target = (l2_bytes * 6) / 10;
    int mc_calc = static_cast<int>(l2_target / (static_cast<size_t>(K_C) * elem_size));
    if (mc_calc < 128)  mc_calc = 128;
    if (mc_calc > 2048) mc_calc = 2048;
    mc_calc = (mc_calc / M_R) * M_R;
    if (mc_calc == 0) mc_calc = M_R;

    if (total_M > 0 && total_M < static_cast<size_t>(mc_calc)) {
        // Enforce multiple of M_R when total_M >= M_R
        M_C = (total_M >= static_cast<size_t>(M_R)) ? static_cast<int>((total_M / M_R) * M_R) : static_cast<int>(total_M);
    } else {
        M_C = mc_calc;
    }

    // 3. N_C macro-block size: Macro-tile of B (K_C x N_C) fits in L3 / 3D V-Cache / bLLC
    if (_hierarchy.has_intel_bllc) {
        N_C = 16384;
    } else if (_hierarchy.has_3d_vcache) {
        N_C = 8192;
    } else {
        size_t l3_bytes = (_hierarchy.l3.size_kb > 0 ? _hierarchy.l3.size_kb : 16384) * 1024ULL;
        size_t l3_target = l3_bytes / 3;
        int nc_calc = static_cast<int>(l3_target / (static_cast<size_t>(K_C) * elem_size));
        if (nc_calc < 256)  nc_calc = 256;
        if (nc_calc > 4096) nc_calc = 4096;
        N_C = nc_calc;
    }
    N_C = (N_C / N_R) * N_R;
    if (N_C == 0) N_C = N_R;

    if (total_N > 0 && total_N < static_cast<size_t>(N_C)) {
        // Enforce multiple of N_R when total_N >= N_R
        N_C = (total_N >= static_cast<size_t>(N_R)) ? static_cast<int>((total_N / N_R) * N_R) : static_cast<int>(total_N);
    }
}

void RedjetSRAMCache::get_optimal_tile_sizes(size_t elem_size, int& tile_M, int& tile_N, int& tile_K) const {
    int mr = 8, nr = 6;
    compute_blis_tiles(elem_size, 0, 0, 0, tile_M, tile_N, tile_K, mr, nr);
}

/* ========================================================================= */
/* 9. AMD ASYMMETRIC DUAL-CCD 3D V-CACHE ROUTING                             */
/* ========================================================================= */

bool RedjetSRAMCache::is_core_on_vcache_ccd(uint32_t core_id) const {
    if (!_hierarchy.has_3d_vcache) return false;
    if (!_hierarchy.has_asymmetric_3d_vcache) return true; // Single CCD X3D: all cores have V-Cache
    if (core_id >= 64) return false;
    return (_hierarchy.vcache_core_mask & (1ULL << core_id)) != 0;
}

bool RedjetSRAMCache::bind_current_thread_to_vcache_ccd() const {
    if (!_hierarchy.has_3d_vcache || _hierarchy.vcache_core_mask == 0) return false;

#if defined(_WIN32)
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)_hierarchy.vcache_core_mask);
    return prev != 0;
#elif defined(__linux__) || defined(__ANDROID__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 64; ++i) {
        if (_hierarchy.vcache_core_mask & (1ULL << i)) CPU_SET(i, &cpuset);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    return true;
#endif
}

bool RedjetSRAMCache::bind_current_thread_to_frequency_ccd() const {
    if (!_hierarchy.has_asymmetric_3d_vcache || _hierarchy.frequency_core_mask == 0) return false;

#if defined(_WIN32)
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)_hierarchy.frequency_core_mask);
    return prev != 0;
#elif defined(__linux__) || defined(__ANDROID__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 64; ++i) {
        if (_hierarchy.frequency_core_mask & (1ULL << i)) CPU_SET(i, &cpuset);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    return true;
#endif
}

/* ========================================================================= */
/* 10. VOLATILITY & DRAM COMMITMENT                                          */
/* ========================================================================= */

void RedjetSRAMCache::commit_sram_to_dram(void* ptr, size_t bytes) const {
    hs_sram_commit_to_dram(ptr, bytes);
}

void RedjetSRAMCache::broadcast_volatility(void* ptr, size_t bytes) const {
    hs_sram_broadcast_volatility(ptr, bytes);
}

/* ========================================================================= */
/* 11. DIAGNOSTICS & VERIFICATION SUITE                                      */
/* ========================================================================= */

bool RedjetSRAMCache::run_diagnostics() {
    if (!is_initialized()) {
        initialize(nullptr);
    }

    // 1. Scratchpad bump allocation test
    void* p1 = scratchpad_alloc(1024, 64);
    if (!p1) return false;
    if ((reinterpret_cast<uintptr_t>(p1) & 63) != 0) return false;

    // Verify memory write & read
    std::memset(p1, 0x5A, 1024);
    const uint8_t* byte_p = static_cast<const uint8_t*>(p1);
    for (size_t i = 0; i < 1024; ++i) {
        if (byte_p[i] != 0x5A) return false;
    }

    void* p2 = scratchpad_alloc(2048, 64);
    if (!p2) return false;
    if ((reinterpret_cast<uintptr_t>(p2) & 63) != 0) return false;
    if (p2 <= p1) return false;

    // Test non-power-of-two alignment round-up (48 -> 64)
    void* p3 = scratchpad_alloc(512, 48);
    if (!p3) return false;
    if ((reinterpret_cast<uintptr_t>(p3) & 63) != 0) return false; // Must be 64-byte aligned

    // 2. Metrics check
    RedjetScratchpadMetrics metrics = get_scratchpad_metrics();
    if (metrics.total_allocations < 3) return false;
    if (metrics.used_bytes < 3584) return false;

    // 3. Reset test
    scratchpad_reset();
    metrics = get_scratchpad_metrics();
    if (metrics.used_bytes != 0) return false;
    if (metrics.reset_count < 1) return false;

    // 4. BLIS tile calculation test (Large matrix)
    int M_C = 0, N_C = 0, K_C = 0, M_R = 0, N_R = 0;
    compute_blis_tiles(sizeof(float), 2048, 2048, 2048, M_C, N_C, K_C, M_R, N_R);
    if (M_C <= 0 || N_C <= 0 || K_C <= 0 || M_R <= 0 || N_R <= 0) return false;
    if (M_C % M_R != 0 || N_C % N_R != 0) return false;

    // BLIS tile calculation test (Small matrix with non-aligned dimensions 100x100x100)
    int sm_MC = 0, sm_NC = 0, sm_KC = 0, sm_MR = 0, sm_NR = 0;
    compute_blis_tiles(sizeof(float), 100, 100, 100, sm_MC, sm_NC, sm_KC, sm_MR, sm_NR);
    if (sm_MC <= 0 || sm_NC <= 0 || sm_KC <= 0 || sm_MR <= 0 || sm_NR <= 0) return false;
    if (sm_MC % sm_MR != 0 || sm_NC % sm_NR != 0) return false;

    // 5. Streaming store threshold check
    bool stream_small = should_stream_store(1024 * 1024); // 1 MB
    if (stream_small) return false; // 1 MB should never stream store

    return true;
}
