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

#ifndef SHARE_GC_REDLET_REDLETHARDWARE_HPP
#define SHARE_GC_REDLET_REDLETHARDWARE_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <vector>
#include <utility>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

#include "gc/redlet/redletGearkit.hpp"

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

#if defined(__linux__)
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#endif

/* ========================================================================= */
/* 1. ARCHITECTURAL & HARDWARE TOPOLOGY TYPES & ENUMS                        */
/* ========================================================================= */

#ifndef SF_CORE_TIER_DEFINED
#define SF_CORE_TIER_DEFINED
typedef enum SF_custominiVm_core_tier_t {
    SF_CORE_TIER_ANY         = 0,
    SF_CORE_TIER_PERFORMANCE = 1,
    SF_CORE_TIER_EFFICIENCY  = 2
} SF_custominiVm_core_tier_t;
#endif

typedef enum SF_custominiVm_cpu_topology_class_t {
    SF_CPU_TOPOLOGY_HOMOGENEOUS   = 0,
    SF_CPU_TOPOLOGY_HYBRID_INTEL  = 1,
    SF_CPU_TOPOLOGY_HYBRID_ARM    = 2,
    SF_CPU_TOPOLOGY_HYBRID_APPLE  = 3
} SF_custominiVm_cpu_topology_class_t;

typedef enum SF_custominiVm_mem_architecture_t {
    SF_MEM_ARCH_UMA             = 0,
    SF_MEM_ARCH_NUMA_SYMMETRIC  = 1,
    SF_MEM_ARCH_NUMA_ASYMMETRIC = 2
} SF_custominiVm_mem_architecture_t;

typedef enum SF_custominiVm_smt_mode_t {
    SF_SMT_MODE_DISABLED_OR_ABSENT = 0,
    SF_SMT_MODE_2WAY               = 1,
    SF_SMT_MODE_4WAY_PLUS          = 2
} SF_custominiVm_smt_mode_t;

typedef enum SF_custominiVm_workload_affinity_policy_t {
    SF_AFFINITY_PHYSICAL_CORES_ONLY = 0,
    SF_AFFINITY_ALL_LOGICAL_THREADS = 1,
    SF_AFFINITY_ISOLATE_P_CORES     = 2
} SF_custominiVm_workload_affinity_policy_t;

typedef enum SF_custominiVm_sram_regime_t {
    SF_SRAM_REGIME_STANDARD_HIERARCHICAL = 0,
    SF_SRAM_REGIME_3D_VCACHE             = 1,
    SF_SRAM_REGIME_SCRATCHPAD_TCM        = 2,
    SF_SRAM_REGIME_WIDE_CACHELINE_128B   = 3
} SF_custominiVm_sram_regime_t;

typedef enum SF_custominiVm_execution_focus_t {
    SF_FOCUS_AUTO            = 0,
    SF_FOCUS_SCP_HOMO        = 1,
    SF_FOCUS_SCP_HYBRID      = 2,
    SF_FOCUS_MCP_HOMO        = 3,
    SF_FOCUS_MCP_HYBRID      = 4,
    SF_FOCUS_DUAL_HOMO       = 5,
    SF_FOCUS_DUAL_HYBRID     = 6,
    SF_FOCUS_SCP             = 10,
    SF_FOCUS_MCP             = 11,
    SF_FOCUS_HYBRID_DUAL     = 12,
    SF_FOCUS_DUAL            = 12
} SF_custominiVm_execution_focus_t;

typedef enum SF_custominiVm_hardware_tier_t {
    SF_HW_TIER_DOWN        = 0,
    SF_HW_TIER_MID_DOWN   = 1,
    SF_HW_TIER_MID_MID    = 2,
    SF_HW_TIER_MID_UP     = 3,
    SF_HW_TIER_UP         = 4
} SF_custominiVm_hardware_tier_t;

typedef struct SF_custominiVm_cache_level_info_t {
    uint32_t size_kb;
    uint16_t line_size;
    uint8_t  associativity;
    uint8_t  type;
} SF_custominiVm_cache_level_info_t;

typedef struct SF_custominiVm_hardware_topology_t {
    SF_custominiVm_cache_level_info_t l1i;
    SF_custominiVm_cache_level_info_t l1d;
    SF_custominiVm_cache_level_info_t l2;
    SF_custominiVm_cache_level_info_t l3;
    uint16_t primary_cacheline_size;
    uint8_t  _pad0[2];

    uint64_t total_phys_ram_bytes;
    uint64_t avail_phys_ram_bytes;
    uint32_t page_size;
    uint32_t huge_page_size;
    uint32_t huge_pages_supported;

    uint8_t  numa_nodes;
    uint8_t  memory_channels;
    uint16_t bus_width_bits;
    uint32_t estimated_bandwidth_gbps;
    uint32_t is_uma;

    uint32_t physical_cores;
    uint32_t logical_cores;
    uint32_t p_cores;
    uint32_t e_cores;
    uint32_t is_heterogeneous;

    char     motherboard_model[128];
    uint32_t dram_mhz;
    uint32_t is_xmp_expo_active;
    uint32_t cpu_cur_freq_mhz;
    uint32_t cpu_max_freq_mhz;

    uint32_t is_upclocker_active;
    char     summary[1024];
} SF_custominiVm_hardware_topology_t;
typedef SF_custominiVm_hardware_topology_t sf_hardware_topology_t;

#ifndef SF_PMU_STAT_DEFINED
#define SF_PMU_STAT_DEFINED
typedef struct SF_custominiVm_pmu_stat_t {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t l1d_cache_misses;
    uint64_t llc_cache_misses;
    double   ipc;
    double   freq_downclock_ratio;
    uint8_t  hw_pmu_active;
    uint8_t  _pad[7];
} SF_custominiVm_pmu_stat_t;
typedef SF_custominiVm_pmu_stat_t sfPmuStat;
#endif

enum class sf_uarch_ipc_tier_t : uint8_t {
    ULTRA_IPC      = 0,
    HIGH_IPC       = 1,
    MODERATE_IPC   = 2,
    EFFICIENCY_IPC = 3
};

struct sf_tablegen_uarch_entry_t {
    const char*            profile_tag;
    const char*            uarch_name;
    const char*            family_name;
    const char*            vendor_code;
    uint32_t               family;
    uint32_t               model_min;
    uint32_t               model_max;
    bool                   is_hybrid;
    sf_uarch_ipc_tier_t    ipc_tier;
    double                 rel_ipc_weight;
    uint32_t               max_vec_bits;
};

/* ========================================================================= */
/* 2. SUB-ENGINES DEFINITION                                                 */
/* ========================================================================= */

/* ------------------------------------------------------------------------- */
/* 2.1 Hybrid vs Homogeneous CPU Execution Engine                            */
/* ------------------------------------------------------------------------- */
struct sf_hybrid_homo_engine_t {
    SF_custominiVm_cpu_topology_class_t topo_class{ SF_CPU_TOPOLOGY_HOMOGENEOUS };
    uint32_t p_core_count{ 0 };
    uint32_t e_core_count{ 0 };
    uint32_t lp_e_core_count{ 0 };
    uint64_t p_core_mask{ 0 };
    uint64_t e_core_mask{ 0 };
    uint64_t lp_e_core_mask{ 0 };
    uint64_t all_core_mask{ 0 };
    bool     is_hybrid{ false };

    double   r_ipc{ 2.60 };
    double   freq_ratio{ 1.40 };
    double   throughput_ratio{ 3.64 };

    MSIMD_API void init(const sf_hardware_topology_t* topo);
    MSIMD_API bool bind_current_thread(SF_custominiVm_core_tier_t tier);
    MSIMD_API bool set_thread_qos(SF_custominiVm_core_tier_t tier);
    MSIMD_API void partition_asymmetric_work(size_t total_elements,
                                            size_t num_p, size_t num_e,
                                            std::vector<std::pair<size_t, size_t>>& out_p_ranges,
                                            std::vector<std::pair<size_t, size_t>>& out_e_ranges,
                                            size_t align_elements = 16) const;
    MSIMD_API uint32_t get_optimal_worker_count(bool is_heavy_compute) const;
    MSIMD_API bool should_steer_to_pcore(bool is_heavy_simd, size_t vector_bytes = 64) const;
    MSIMD_API SF_custominiVm_core_tier_t classify_core(uint32_t core_id) const;
};

/* ------------------------------------------------------------------------- */
/* 2.2 NUMA vs UMA Memory Architecture Engine                                */
/* ------------------------------------------------------------------------- */
struct sf_numa_uma_engine_t {
    SF_custominiVm_mem_architecture_t mem_arch{ SF_MEM_ARCH_UMA };
    uint8_t  node_count{ 1 };
    uint32_t interconnect_bandwidth_gbps{ 0 };
    bool     is_uma{ true };

    uint8_t  slit_distance[8][8];

    struct NumaNodeDomain {
        std::atomic<size_t>   allocated_bytes{ 0 };
        std::atomic<uint64_t> access_count{ 0 };
        std::atomic<uint64_t> local_reads{ 0 };
        std::atomic<uint64_t> remote_reads{ 0 };
        uint64_t              processor_mask{ 0 };
        uint8_t               node_id{ 0 };
    } nodes[8];

    std::atomic<uint64_t> access_matrix[8][8];

    MSIMD_API void init(const sf_hardware_topology_t* topo);
    MSIMD_API void* alloc_node(size_t bytes, uint8_t preferred_node = 0);
    MSIMD_API void free_node(void* ptr, size_t bytes, uint8_t node = 0);
    MSIMD_API uint8_t get_current_thread_node();
    MSIMD_API void record_access(uint8_t accessing_node, uint8_t target_node, size_t bytes = 64);
    MSIMD_API float get_remote_heat_ratio(uint8_t node) const;
    MSIMD_API bool should_migrate(uint8_t host_node, uint8_t& out_dominant_node, float threshold = 0.70f) const;
    MSIMD_API void distribute_numa_work(size_t total_elements,
                                        std::vector<std::pair<size_t, size_t>>& out_node_slices) const;
};

/* ------------------------------------------------------------------------- */
/* 2.3 SMT (Hyper-Threading) vs Non-HT CPU Execution Engine                  */
/* ------------------------------------------------------------------------- */
struct sf_smt_ht_engine_t {
    SF_custominiVm_smt_mode_t smt_mode{ SF_SMT_MODE_DISABLED_OR_ABSENT };
    uint32_t physical_cores{ 1 };
    uint32_t logical_cores{ 1 };
    uint32_t threads_per_core{ 1 };
    bool     is_ht_active{ false };

    uint64_t primary_threads_mask{ 0 };
    uint64_t sibling_threads_mask{ 0 };
    uint8_t  sibling_map[64];

    struct sf_thread_budget_t {
        uint32_t gc_threads;
        uint32_t puzzle_threads;
        uint32_t vm_threads;
        uint32_t total_threads;
    };

    MSIMD_API void init(const sf_hardware_topology_t* topo);
    MSIMD_API uint32_t get_optimal_concurrency(SF_custominiVm_workload_affinity_policy_t policy) const;
    MSIMD_API SF_custominiVm_hardware_tier_t detect_hardware_tier() const;
    MSIMD_API const char* get_hardware_tier_name() const;
    MSIMD_API sf_thread_budget_t calculate_thread_budget() const;
    MSIMD_API bool apply_affinity_policy(SF_custominiVm_workload_affinity_policy_t policy);
    MSIMD_API bool is_primary_core(uint32_t core_id) const;
    MSIMD_API uint8_t get_sibling_core(uint8_t core_id) const;
    MSIMD_API double estimate_port_contention_penalty(bool is_fma_vector) const;

    static inline void smt_pause_backoff(uint32_t iteration) {
#if defined(_M_X64) || defined(__x86_64__)
        if (iteration < 4) {
            _mm_pause();
        } else if (iteration < 16) {
            _mm_pause(); _mm_pause(); _mm_pause(); _mm_pause();
        } else if (iteration < 64) {
            for (int k = 0; k < 16; ++k) _mm_pause();
        } else {
#if defined(_WIN32)
            SwitchToThread();
#else
            std::this_thread::yield();
#endif
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        if (iteration < 16) {
            __asm__ __volatile__("isb" ::: "memory");
        } else {
            std::this_thread::yield();
        }
#else
        std::this_thread::yield();
#endif
    }
};

/* ------------------------------------------------------------------------- */
/* 2.4 Diverse SRAM & Cache Architectures Engine                             */
/* ------------------------------------------------------------------------- */
struct sf_sram_cache_engine_t {
    SF_custominiVm_sram_regime_t sram_regime{ SF_SRAM_REGIME_STANDARD_HIERARCHICAL };
    uint32_t l1d_kb{ 32 };
    uint32_t l1i_kb{ 32 };
    uint32_t l2_kb{ 512 };
    uint32_t l3_kb{ 16384 };
    uint16_t cacheline_bytes{ 64 };
    bool     has_3d_vcache{ false };
    bool     has_intel_bllc{ false };
    bool     has_scratchpad_sram{ false };

    static constexpr size_t SCRATCHPAD_CAPACITY = 8ULL * 1024 * 1024;
    uint8_t* scratchpad_base{ nullptr };
    std::atomic<size_t> scratchpad_offset{ 0 };

    MSIMD_API void init(const sf_hardware_topology_t* topo);
    MSIMD_API void cleanup();
    MSIMD_API void* scratchpad_alloc(size_t bytes, size_t alignment = 64);
    MSIMD_API void scratchpad_reset();
    MSIMD_API size_t pad_to_cacheline(size_t size) const;
    MSIMD_API void* align_to_cacheline(void* ptr) const;
    MSIMD_API bool should_stream_store(size_t tensor_bytes) const;
    MSIMD_API void compute_blis_tiles(size_t elem_size,
                                      size_t total_M, size_t total_N, size_t total_K,
                                      int& M_C, int& N_C, int& K_C,
                                      int& M_R, int& N_R) const;
    MSIMD_API void get_optimal_tile_sizes(size_t elem_size, int& tile_M, int& tile_N, int& tile_K) const;
    MSIMD_API size_t get_cache_color_stride(uint32_t ways = 8) const;
    MSIMD_API static void flush_wc_buffers();
};

/* ------------------------------------------------------------------------- */
/* 2.5 Dynamic Upclocker Engine (SacFor_VM.cpp:10082-10133)                  */
/* ------------------------------------------------------------------------- */
struct sf_upclocker_t {
    std::atomic<bool> is_boosted{ false };

    MSIMD_API static void pin_max_frequency_warmup(uint32_t duration_us = 50);
    MSIMD_API void enable(bool boost);
};

/* ------------------------------------------------------------------------- */
/* 2.6 Execution Focus Engine (TableGen Profile Matching)                    */
/* ------------------------------------------------------------------------- */
struct sf_execution_focus_engine_t {
    std::atomic<SF_custominiVm_execution_focus_t> configured_focus{ SF_FOCUS_AUTO };
    std::atomic<SF_custominiVm_execution_focus_t> active_focus{ SF_FOCUS_AUTO };
    const sf_tablegen_uarch_entry_t*              matched_uarch{ nullptr };
    char                                          uarch_summary[128]{ 0 };

    double p_weight{ 1.0 };
    double e_weight{ 1.0 };

    bool hw_is_hybrid{ false };
    bool is_hybrid{ false };
    bool is_homogeneous{ true };
    bool is_scp{ false };
    bool is_mcp{ false };
    bool is_dual{ false };

    uint32_t p_cores{ 0 };
    uint32_t e_cores{ 0 };
    uint32_t total_cores{ 0 };

    sf_hybrid_homo_engine_t* hybrid_eng_ref{ nullptr };

    MSIMD_API void init(const sf_hardware_topology_t* topo,
                        sf_hybrid_homo_engine_t* hybrid_eng,
                        const sf_sram_cache_engine_t* sram_eng);
    MSIMD_API SF_custominiVm_execution_focus_t detect_focus(const sf_hardware_topology_t* topo,
                                                            const sf_hybrid_homo_engine_t* hybrid_eng) const;
    MSIMD_API void set_focus(SF_custominiVm_execution_focus_t focus);
    MSIMD_API void apply_state(SF_custominiVm_execution_focus_t resolved);
};

/* ------------------------------------------------------------------------- */
/* 2.7 Hardware Performance Counter (PMU) Telemetry Engine                   */
/* ------------------------------------------------------------------------- */
struct sf_pmu_session_t {
    uint64_t start_tsc{ 0 };
    uint64_t start_ns{ 0 };
#if defined(__linux__)
    int fd_cycles{ -1 };
    int fd_instr{ -1 };
    int fd_cache_miss{ -1 };
#endif

    MSIMD_API void begin() noexcept;
    MSIMD_API void end(SF_custominiVm_pmu_stat_t* out, size_t bytes_processed = 0) noexcept;
};

/* ------------------------------------------------------------------------- */
/* 2.8 Unified Hardware Engine Nexus                                         */
/* ------------------------------------------------------------------------- */
struct sf_hardware_engine_nexus_t {
    sf_hybrid_homo_engine_t     hybrid;
    sf_numa_uma_engine_t        numa;
    sf_smt_ht_engine_t          smt;
    sf_sram_cache_engine_t      sram;
    sf_upclocker_t              upclocker;
    sf_execution_focus_engine_t focus;
    sf_hardware_topology_t      topology;
    std::atomic<bool>           initialized{ false };

    MSIMD_API void init(const sf_hardware_topology_t* topo = nullptr);
    MSIMD_API void cleanup();
};

/* ------------------------------------------------------------------------- */
/* 2.9 CPU ISA Inventory & Dynamic Discovery                                 */
/* ------------------------------------------------------------------------- */
struct sf_cpu_isa_inventory_t {
    std::vector<std::string> discovered_isas;
    sf_hardware_topology_t  topology{};
    char description[2048] = { 0 };

    bool has_isa(const char* token) const;
    void add_isa(const char* token);
    std::string resolve_fallback(const char* requested_isa) const;
    void parse_cpu_string(const char* text);
    void build_description();
    static sf_cpu_isa_inventory_t discover(void);
};

/* ========================================================================= */
/* 3. SRAM CACHELINE & VOLATILITY MEMORY CONTROL APIS                        */
/* ========================================================================= */

MSIMD_API void sf_sram_commit_to_dram(void* ptr, size_t bytes);
MSIMD_API void sf_sram_broadcast_volatility(void* ptr, size_t bytes);
MSIMD_API void sf_dram_stream_store(void* dst, const void* src, size_t bytes);
MSIMD_API sf_hardware_topology_t sf_detect_hardware_topology(const char* isa_desc = nullptr);

/* ========================================================================= */
/* 4. GLOBAL ACCESSORS & PUBLIC C APIS (MSIMD_API)                           */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API sf_hardware_engine_nexus_t* sf_get_hardware_nexus(void);
MSIMD_API void sf_init_hardware_nexus(const sf_hardware_topology_t* topo);
MSIMD_API bool sf_bind_current_thread(SF_custominiVm_core_tier_t tier);
MSIMD_API bool sf_is_hybrid_cpu(void);
MSIMD_API bool sf_is_focus_scp(void);
MSIMD_API uint32_t sf_get_p_core_count(void);
MSIMD_API uint32_t sf_get_e_core_count(void);
MSIMD_API uint32_t sf_get_sram_l1d_kb(void);
MSIMD_API uint32_t sf_get_sram_l2_kb(void);
MSIMD_API uint32_t sf_get_sram_l3_kb(void);
MSIMD_API uint8_t  sf_get_numa_node_count(void);
MSIMD_API bool     sf_is_uma_system(void);
MSIMD_API void     sf_upclocker_set_boost(int enable);
MSIMD_API int      sf_upclocker_is_boosted(void);

// Canonical C API Surface
MSIMD_API const SF_custominiVm_hardware_topology_t* SF_custominiVm_hardware_topology(void);
MSIMD_API int SF_custominiVm_cpu_numa_nodes(void);
MSIMD_API void SF_custominiVm_upclocker_set_boost(int enable);
MSIMD_API int SF_custominiVm_upclocker_is_boosted(void);
MSIMD_API void SF_custominiVm_upclocker_warmup(uint32_t duration_us);
MSIMD_API int SF_custominiVm_is_hybrid_cpu(void);
MSIMD_API int SF_custominiVm_is_numa(void);
MSIMD_API SF_custominiVm_smt_mode_t SF_custominiVm_smt_mode(void);
MSIMD_API SF_custominiVm_sram_regime_t SF_custominiVm_sram_regime(void);
MSIMD_API int SF_custominiVm_bind_calling_thread(SF_custominiVm_core_tier_t tier);
MSIMD_API void* SF_custominiVm_sram_scratchpad_alloc(size_t bytes, size_t alignment);
MSIMD_API void SF_custominiVm_sram_scratchpad_reset(void);
MSIMD_API void* SF_custominiVm_numa_alloc(size_t bytes, uint8_t node);
MSIMD_API void SF_custominiVm_numa_free(void* ptr, size_t bytes, uint8_t node);
MSIMD_API SF_custominiVm_execution_focus_t SF_custominiVm_get_execution_focus(void);
MSIMD_API void SF_custominiVm_set_execution_focus(SF_custominiVm_execution_focus_t focus);
MSIMD_API const char* SF_custominiVm_get_execution_focus_name(SF_custominiVm_execution_focus_t focus);
MSIMD_API SF_custominiVm_execution_focus_t SF_custominiVm_detect_cpu_execution_focus(void);
MSIMD_API const char* SF_custominiVm_get_focus_uarch_name(void);
MSIMD_API void SF_custominiVm_get_focus_chunk_weights(double* out_p_weight, double* out_e_weight);
MSIMD_API int SF_custominiVm_is_focus_scp(void);
MSIMD_API int SF_custominiVm_is_focus_mcp(void);
MSIMD_API int SF_custominiVm_is_focus_dual(void);
MSIMD_API SF_custominiVm_hardware_tier_t SF_custominiVm_query_hardware_tier(void);
MSIMD_API const char* SF_custominiVm_query_hardware_tier_name(void);
MSIMD_API void SF_custominiVm_query_thread_budget(uint32_t* out_gc, uint32_t* out_puzzle, uint32_t* out_vm);
MSIMD_API void SF_custominiVm_sram_commit_to_dram(void* ptr, size_t bytes);
MSIMD_API void SF_custominiVm_sram_broadcast_volatility(void* ptr, size_t bytes);
MSIMD_API void SF_custominiVm_dram_stream_store(void* dst, const void* src, size_t bytes);
MSIMD_API void SF_custominiVm_pmu_sample_begin(void);
MSIMD_API void SF_custominiVm_pmu_sample_end(SF_custominiVm_pmu_stat_t* out_stat, size_t bytes_processed);

MSIMD_API void* sf_numa_alloc(size_t size, uint32_t numa_node);
MSIMD_API void  sf_numa_free(void* ptr, size_t size);
MSIMD_API void* msimd_numa_alloc(size_t size, uint32_t numa_node);
MSIMD_API void  msimd_numa_free(void* ptr, size_t size);
MSIMD_API int   sf_thread_pin_to_numa_node(uint32_t numa_node);
MSIMD_API int   msimd_thread_pin_to_numa_node(uint32_t numa_node);
MSIMD_API int   SF_custominiVm_thread_pin_to_numa_node(uint32_t numa_node);

#ifdef __cplusplus
}
#endif

// High-level Redlet aliases
using RedletHardwareNexus        = sf_hardware_engine_nexus_t;
using RedletHybridEngine          = sf_hybrid_homo_engine_t;
using RedletNumaEngine            = sf_numa_uma_engine_t;
using RedletSMTEngine             = sf_smt_ht_engine_t;
using RedletSRAMCacheEngine       = sf_sram_cache_engine_t;
using RedletUpclocker             = sf_upclocker_t;
using RedletPMUSession            = sf_pmu_session_t;
using RedletExecutionFocusEngine  = sf_execution_focus_engine_t;
using RedletCPUISAInventory       = sf_cpu_isa_inventory_t;

#endif // SHARE_GC_REDLET_REDLETHARDWARE_HPP
