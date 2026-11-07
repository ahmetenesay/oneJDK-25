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

#ifndef SHARE_GC_REDJET_REDJETHARDWARE_HPP
#define SHARE_GC_REDJET_REDJETHARDWARE_HPP

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

#include "gc/redjet/runtime/redjetGearkit.hpp"

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

static inline void hs_get_cpuid(int info[4], int func, int subfunc = 0) {
#if defined(_MSC_VER) || defined(__clang__)
    __cpuidex(info, func, subfunc);
#elif defined(__GNUC__)
    __asm__ __volatile__(
        "cpuid"
        : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3])
        : "a"(func), "c"(subfunc));
#else
    info[0] = info[1] = info[2] = info[3] = 0;
#endif
}

/* ========================================================================= */
/* 1. ARCHITECTURAL & HARDWARE TOPOLOGY TYPES & ENUMS                        */
/* ========================================================================= */

#ifndef HS_CORE_TIER_DEFINED
#define HS_CORE_TIER_DEFINED
typedef enum HS_hotspot_core_tier_t {
    HS_CORE_TIER_ANY         = 0,
    HS_CORE_TIER_PERFORMANCE = 1,
    HS_CORE_TIER_EFFICIENCY  = 2
} HS_hotspot_core_tier_t;
#endif

typedef enum HS_hotspot_cpu_topology_class_t {
    HS_CPU_TOPOLOGY_HOMOGENEOUS   = 0,
    HS_CPU_TOPOLOGY_HYBRID_INTEL  = 1,
    HS_CPU_TOPOLOGY_HYBRID_ARM    = 2,
    HS_CPU_TOPOLOGY_HYBRID_APPLE  = 3
} HS_hotspot_cpu_topology_class_t;

typedef enum HS_hotspot_mem_architecture_t {
    HS_MEM_ARCH_UMA             = 0,
    HS_MEM_ARCH_NUMA_SYMMETRIC  = 1,
    HS_MEM_ARCH_NUMA_ASYMMETRIC = 2
} HS_hotspot_mem_architecture_t;

typedef enum HS_hotspot_smt_mode_t {
    HS_SMT_MODE_DISABLED_OR_ABSENT = 0,
    HS_SMT_MODE_2WAY               = 1,
    HS_SMT_MODE_4WAY_PLUS          = 2
} HS_hotspot_smt_mode_t;

typedef enum HS_hotspot_workload_affinity_policy_t {
    HS_AFFINITY_PHYSICAL_CORES_ONLY = 0,
    HS_AFFINITY_ALL_LOGICAL_THREADS = 1,
    HS_AFFINITY_ISOLATE_P_CORES     = 2
} HS_hotspot_workload_affinity_policy_t;

typedef enum HS_hotspot_sram_regime_t {
    HS_SRAM_REGIME_STANDARD_HIERARCHICAL = 0,
    HS_SRAM_REGIME_3D_VCACHE             = 1,
    HS_SRAM_REGIME_SCRATCHPAD_TCM        = 2,
    HS_SRAM_REGIME_WIDE_CACHELINE_128B   = 3
} HS_hotspot_sram_regime_t;

typedef enum HS_hotspot_execution_focus_t {
    HS_FOCUS_AUTO            = 0,
    HS_FOCUS_SCP_HOMO        = 1,
    HS_FOCUS_SCP_HYBRID      = 2,
    HS_FOCUS_MCP_HOMO        = 3,
    HS_FOCUS_MCP_HYBRID      = 4,
    HS_FOCUS_DUAL_HOMO       = 5,
    HS_FOCUS_DUAL_HYBRID     = 6,
    HS_FOCUS_SCP             = 10,
    HS_FOCUS_MCP             = 11,
    HS_FOCUS_HYBRID_DUAL     = 12,
    HS_FOCUS_DUAL            = 12
} HS_hotspot_execution_focus_t;

typedef enum HS_hotspot_hardware_tier_t {
    HS_HW_TIER_DOWN        = 0,
    HS_HW_TIER_MID_DOWN   = 1,
    HS_HW_TIER_MID_MID    = 2,
    HS_HW_TIER_MID_UP     = 3,
    HS_HW_TIER_UP         = 4
} HS_hotspot_hardware_tier_t;

typedef struct HS_hotspot_cache_level_info_t {
    uint32_t size_kb;
    uint16_t line_size;
    uint8_t  associativity;
    uint8_t  type;
} HS_hotspot_cache_level_info_t;

typedef struct HS_hotspot_hardware_topology_t {
    HS_hotspot_cache_level_info_t l1i;
    HS_hotspot_cache_level_info_t l1d;
    HS_hotspot_cache_level_info_t l2;
    HS_hotspot_cache_level_info_t l3;
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
} HS_hotspot_hardware_topology_t;
typedef HS_hotspot_hardware_topology_t hs_hardware_topology_t;

#ifndef HS_PMU_STAT_DEFINED
#define HS_PMU_STAT_DEFINED
typedef struct HS_hotspot_pmu_stat_t {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t l1d_cache_misses;
    uint64_t llc_cache_misses;
    double   ipc;
    double   freq_downclock_ratio;
    uint8_t  hw_pmu_active;
    uint8_t  _pad[7];
} HS_hotspot_pmu_stat_t;
typedef HS_hotspot_pmu_stat_t hsPmuStat;
#endif

enum class hs_uarch_ipc_tier_t : uint8_t {
    ULTRA_IPC      = 0,
    HIGH_IPC       = 1,
    MODERATE_IPC   = 2,
    EFFICIENCY_IPC = 3
};

struct hs_tablegen_uarch_entry_t {
    const char*            profile_tag;
    const char*            uarch_name;
    const char*            family_name;
    const char*            vendor_code;
    uint32_t               family;
    uint32_t               model_min;
    uint32_t               model_max;
    bool                   is_hybrid;
    hs_uarch_ipc_tier_t    ipc_tier;
    double                 rel_ipc_weight;
    uint32_t               max_vec_bits;
};

/* ========================================================================= */
/* 2. SUB-ENGINES DEFINITION                                                 */
/* ========================================================================= */

/* ------------------------------------------------------------------------- */
/* 2.1 Hybrid vs Homogeneous CPU Execution Engine                            */
/* ------------------------------------------------------------------------- */
struct hs_hybrid_homo_engine_t {
    HS_hotspot_cpu_topology_class_t topo_class{ HS_CPU_TOPOLOGY_HOMOGENEOUS };
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

    RJ_API void init(const hs_hardware_topology_t* topo);
    RJ_API bool bind_current_thread(HS_hotspot_core_tier_t tier);
    RJ_API bool set_thread_qos(HS_hotspot_core_tier_t tier);
    RJ_API void partition_asymmetric_work(size_t total_elements,
                                            size_t num_p, size_t num_e,
                                            std::vector<std::pair<size_t, size_t>>& out_p_ranges,
                                            std::vector<std::pair<size_t, size_t>>& out_e_ranges,
                                            size_t align_elements = 16) const;
    RJ_API uint32_t get_optimal_worker_count(bool is_heavy_compute) const;
    RJ_API bool should_steer_to_pcore(bool is_heavy_simd, size_t vector_bytes = 64) const;
    RJ_API HS_hotspot_core_tier_t classify_core(uint32_t core_id) const;
    RJ_API bool bind_gc_sweeper_to_ecore(uint32_t thread_idx = (uint32_t)-1);
    RJ_API bool bind_mutator_to_pcore(uint32_t thread_idx = (uint32_t)-1);
    RJ_API bool bind_mutator_to_all_pcores();
    RJ_API bool is_dual_hybrid_applicable() const;
};

/* ------------------------------------------------------------------------- */
/* 2.2 NUMA vs UMA Memory Architecture Engine                                */
/* ------------------------------------------------------------------------- */
struct hs_numa_uma_engine_t {
    HS_hotspot_mem_architecture_t mem_arch{ HS_MEM_ARCH_UMA };
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

    RJ_API void init(const hs_hardware_topology_t* topo);
    RJ_API void* alloc_node(size_t bytes, uint8_t preferred_node = 0);
    RJ_API void free_node(void* ptr, size_t bytes, uint8_t node = 0);
    RJ_API uint8_t get_current_thread_node();
    RJ_API void record_access(uint8_t accessing_node, uint8_t target_node, size_t bytes = 64);
    RJ_API float get_remote_heat_ratio(uint8_t node) const;
    RJ_API bool should_migrate(uint8_t host_node, uint8_t& out_dominant_node, float threshold = 0.70f) const;
    RJ_API void distribute_numa_work(size_t total_elements,
                                        std::vector<std::pair<size_t, size_t>>& out_node_slices) const;
};

/* ------------------------------------------------------------------------- */
/* 2.3 SMT (Hyper-Threading) vs Non-HT CPU Execution Engine                  */
/* ------------------------------------------------------------------------- */
struct hs_smt_ht_engine_t {
    HS_hotspot_smt_mode_t smt_mode{ HS_SMT_MODE_DISABLED_OR_ABSENT };
    uint32_t physical_cores{ 1 };
    uint32_t logical_cores{ 1 };
    uint32_t threads_per_core{ 1 };
    bool     is_ht_active{ false };

    uint64_t primary_threads_mask{ 0 };
    uint64_t sibling_threads_mask{ 0 };
    uint8_t  sibling_map[64];

    struct hs_thread_budget_t {
        uint32_t gc_threads;
        uint32_t puzzle_threads;
        uint32_t vm_threads;
        uint32_t total_threads;
    };

    RJ_API void init(const hs_hardware_topology_t* topo);
    RJ_API uint32_t get_optimal_concurrency(HS_hotspot_workload_affinity_policy_t policy) const;
    RJ_API HS_hotspot_hardware_tier_t detect_hardware_tier() const;
    RJ_API const char* get_hardware_tier_name() const;
    RJ_API hs_thread_budget_t calculate_thread_budget() const;
    RJ_API bool apply_affinity_policy(HS_hotspot_workload_affinity_policy_t policy);
    RJ_API bool is_primary_core(uint32_t core_id) const;
    RJ_API uint8_t get_sibling_core(uint8_t core_id) const;
    RJ_API double estimate_port_contention_penalty(bool is_fma_vector) const;

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
struct hs_sram_cache_engine_t {
    HS_hotspot_sram_regime_t sram_regime{ HS_SRAM_REGIME_STANDARD_HIERARCHICAL };
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

    RJ_API void init(const hs_hardware_topology_t* topo);
    RJ_API void cleanup();
    RJ_API void* scratchpad_alloc(size_t bytes, size_t alignment = 64);
    RJ_API void scratchpad_reset();
    RJ_API size_t pad_to_cacheline(size_t size) const;
    RJ_API void* align_to_cacheline(void* ptr) const;
    RJ_API bool should_stream_store(size_t tensor_bytes) const;
    RJ_API void compute_blis_tiles(size_t elem_size,
                                      size_t total_M, size_t total_N, size_t total_K,
                                      int& M_C, int& N_C, int& K_C,
                                      int& M_R, int& N_R) const;
    RJ_API void get_optimal_tile_sizes(size_t elem_size, int& tile_M, int& tile_N, int& tile_K) const;
    RJ_API size_t get_cache_color_stride(uint32_t ways = 8) const;
    RJ_API static void flush_wc_buffers();
    RJ_API size_t scratchpad_used_bytes() const;
    RJ_API size_t scratchpad_remaining_bytes() const;
    RJ_API size_t scratchpad_peak_bytes() const;
};

/* ------------------------------------------------------------------------- */
/* 2.5 Dynamic Upclocker Engine (SacFor_VM.cpp:10082-10133)                  */
/* ------------------------------------------------------------------------- */
struct hs_upclocker_t {
    std::atomic<bool> is_boosted{ false };

    RJ_API static void pin_max_frequency_warmup(uint32_t duration_us = 50);
    RJ_API void enable(bool boost);
};

/* ------------------------------------------------------------------------- */
/* 2.6 Execution Focus Engine (TableGen Profile Matching)                    */
/* ------------------------------------------------------------------------- */
struct hs_execution_focus_engine_t {
    std::atomic<HS_hotspot_execution_focus_t> configured_focus{ HS_FOCUS_AUTO };
    std::atomic<HS_hotspot_execution_focus_t> active_focus{ HS_FOCUS_AUTO };
    const hs_tablegen_uarch_entry_t*              matched_uarch{ nullptr };
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

    hs_hybrid_homo_engine_t* hybrid_eng_ref{ nullptr };

    RJ_API void init(const hs_hardware_topology_t* topo,
                        hs_hybrid_homo_engine_t* hybrid_eng,
                        const hs_sram_cache_engine_t* sram_eng);
    RJ_API HS_hotspot_execution_focus_t detect_focus(const hs_hardware_topology_t* topo,
                                                            const hs_hybrid_homo_engine_t* hybrid_eng) const;
    RJ_API void set_focus(HS_hotspot_execution_focus_t focus);
    RJ_API void apply_state(HS_hotspot_execution_focus_t resolved);

    bool is_dual_hybrid() const { return active_focus.load(std::memory_order_relaxed) == HS_FOCUS_DUAL_HYBRID; }
    bool is_scp_hybrid() const { return active_focus.load(std::memory_order_relaxed) == HS_FOCUS_SCP_HYBRID; }
    bool is_mcp_hybrid() const { return active_focus.load(std::memory_order_relaxed) == HS_FOCUS_MCP_HYBRID; }
    bool is_dual_homo() const { return active_focus.load(std::memory_order_relaxed) == HS_FOCUS_DUAL_HOMO; }
    bool is_scp_homo() const { return active_focus.load(std::memory_order_relaxed) == HS_FOCUS_SCP_HOMO; }
    bool is_mcp_homo() const { return active_focus.load(std::memory_order_relaxed) == HS_FOCUS_MCP_HOMO; }
    HS_hotspot_execution_focus_t get_focus_mode() const { return active_focus.load(std::memory_order_relaxed); }
};

/* ------------------------------------------------------------------------- */
/* 2.7 Hardware Performance Counter (PMU) Telemetry Engine                   */
/* ------------------------------------------------------------------------- */
struct hs_pmu_session_t {
    uint64_t start_tsc{ 0 };
    uint64_t start_ns{ 0 };
#if defined(__linux__)
    int fd_cycles{ -1 };
    int fd_instr{ -1 };
    int fd_cache_miss{ -1 };
#endif

    RJ_API void begin() noexcept;
    RJ_API void end(HS_hotspot_pmu_stat_t* out, size_t bytes_processed = 0) noexcept;
};

/* ------------------------------------------------------------------------- */
/* 2.8 Unified Hardware Engine Nexus                                         */
/* ------------------------------------------------------------------------- */
struct hs_hardware_engine_nexus_t {
    hs_hybrid_homo_engine_t     hybrid;
    hs_numa_uma_engine_t        numa;
    hs_smt_ht_engine_t          smt;
    hs_sram_cache_engine_t      sram;
    hs_upclocker_t              upclocker;
    hs_execution_focus_engine_t focus;
    hs_hardware_topology_t      topology;
    std::atomic<bool>           initialized{ false };

    RJ_API void init(const hs_hardware_topology_t* topo = nullptr);
    RJ_API void cleanup();
};

/* ------------------------------------------------------------------------- */
/* 2.9 CPU ISA Inventory & Dynamic Discovery                                 */
/* ------------------------------------------------------------------------- */
struct hs_cpu_isa_inventory_t {
    std::vector<std::string> discovered_isas;
    hs_hardware_topology_t  topology{};
    char description[2048] = { 0 };

    bool has_isa(const char* token) const;
    void add_isa(const char* token);
    std::string resolve_fallback(const char* requested_isa) const;
    void parse_cpu_string(const char* text);
    void build_description();
    static hs_cpu_isa_inventory_t discover(void);
};

/* ========================================================================= */
/* 3. SRAM CACHELINE & VOLATILITY MEMORY CONTROL APIS                        */
/* ========================================================================= */

RJ_API void hs_sram_commit_to_dram(void* ptr, size_t bytes);
RJ_API void hs_sram_broadcast_volatility(void* ptr, size_t bytes);
RJ_API void hs_dram_stream_store(void* dst, const void* src, size_t bytes);
RJ_API hs_hardware_topology_t hs_detect_hardware_topology(const char* isa_desc = nullptr);

/* ========================================================================= */
/* 4. GLOBAL ACCESSORS & PUBLIC C APIS (RJ_API)                           */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

RJ_API hs_hardware_engine_nexus_t* hs_get_hardware_nexus(void);
RJ_API void hs_init_hardware_nexus(const hs_hardware_topology_t* topo);
RJ_API bool hs_bind_current_thread(HS_hotspot_core_tier_t tier);
RJ_API bool hs_is_hybrid_cpu(void);
RJ_API bool hs_is_focus_scp(void);
RJ_API uint32_t hs_get_p_core_count(void);
RJ_API uint32_t hs_get_e_core_count(void);
RJ_API uint32_t hs_get_sram_l1d_kb(void);
RJ_API uint32_t hs_get_sram_l2_kb(void);
RJ_API uint32_t hs_get_sram_l3_kb(void);
RJ_API uint8_t  hs_get_numa_node_count(void);
RJ_API bool     hs_is_uma_system(void);
RJ_API void     hs_upclocker_set_boost(int enable);
RJ_API int      hs_upclocker_is_boosted(void);

// Canonical C API Surface
RJ_API const HS_hotspot_hardware_topology_t* HS_hotspot_hardware_topology(void);
RJ_API int HS_hotspot_cpu_numa_nodes(void);
RJ_API void HS_hotspot_upclocker_set_boost(int enable);
RJ_API int HS_hotspot_upclocker_is_boosted(void);
RJ_API void HS_hotspot_upclocker_warmup(uint32_t duration_us);
RJ_API int HS_hotspot_is_hybrid_cpu(void);
RJ_API int HS_hotspot_is_numa(void);
RJ_API HS_hotspot_smt_mode_t HS_hotspot_smt_mode(void);
RJ_API HS_hotspot_sram_regime_t HS_hotspot_sram_regime(void);
RJ_API int HS_hotspot_bind_calling_thread(HS_hotspot_core_tier_t tier);
RJ_API void* HS_hotspot_sram_scratchpad_alloc(size_t bytes, size_t alignment);
RJ_API void HS_hotspot_sram_scratchpad_reset(void);
RJ_API void* HS_hotspot_numa_alloc(size_t bytes, uint8_t node);
RJ_API void HS_hotspot_numa_free(void* ptr, size_t bytes, uint8_t node);
RJ_API HS_hotspot_execution_focus_t HS_hotspot_get_execution_focus(void);
RJ_API void HS_hotspot_set_execution_focus(HS_hotspot_execution_focus_t focus);
RJ_API const char* HS_hotspot_get_execution_focus_name(HS_hotspot_execution_focus_t focus);
RJ_API HS_hotspot_execution_focus_t HS_hotspot_detect_cpu_execution_focus(void);
RJ_API const char* HS_hotspot_get_focus_uarch_name(void);
RJ_API void HS_hotspot_get_focus_chunk_weights(double* out_p_weight, double* out_e_weight);
RJ_API int HS_hotspot_is_focus_scp(void);
RJ_API int HS_hotspot_is_focus_mcp(void);
RJ_API int HS_hotspot_is_focus_dual(void);
RJ_API HS_hotspot_hardware_tier_t HS_hotspot_query_hardware_tier(void);
RJ_API const char* HS_hotspot_query_hardware_tier_name(void);
RJ_API void HS_hotspot_query_thread_budget(uint32_t* out_gc, uint32_t* out_puzzle, uint32_t* out_vm);
RJ_API void HS_hotspot_sram_commit_to_dram(void* ptr, size_t bytes);
RJ_API void HS_hotspot_sram_broadcast_volatility(void* ptr, size_t bytes);
RJ_API void HS_hotspot_dram_stream_store(void* dst, const void* src, size_t bytes);
RJ_API void HS_hotspot_pmu_sample_begin(void);
RJ_API void HS_hotspot_pmu_sample_end(HS_hotspot_pmu_stat_t* out_stat, size_t bytes_processed);

RJ_API int HS_hotspot_bind_gc_sweeper_to_ecore(uint32_t worker_id);
RJ_API int HS_hotspot_bind_mutator_to_pcore(void);
RJ_API int HS_hotspot_bind_mutator_to_specific_pcore(uint32_t thread_idx);
RJ_API int HS_hotspot_has_3d_vcache(void);
RJ_API int HS_hotspot_has_intel_bllc(void);
RJ_API int HS_hotspot_has_asymmetric_3d_vcache(void);
RJ_API int HS_hotspot_bind_calling_thread_to_vcache_ccd(void);
RJ_API int HS_hotspot_bind_calling_thread_to_frequency_ccd(void);
RJ_API size_t HS_hotspot_sram_scratchpad_used_bytes(void);
RJ_API size_t HS_hotspot_sram_scratchpad_remaining_bytes(void);
RJ_API size_t HS_hotspot_sram_scratchpad_peak_bytes(void);
RJ_API size_t HS_hotspot_sram_scratchpad_capacity(void);

RJ_API void* hs_numa_alloc(size_t size, uint32_t numa_node);
RJ_API void  hs_numa_free(void* ptr, size_t size);
RJ_API void* msimd_numa_alloc(size_t size, uint32_t numa_node);
RJ_API void  msimd_numa_free(void* ptr, size_t size);
RJ_API int   hs_thread_pin_to_numa_node(uint32_t numa_node);
RJ_API int   msimd_thread_pin_to_numa_node(uint32_t numa_node);
RJ_API int   HS_hotspot_thread_pin_to_numa_node(uint32_t numa_node);

#ifdef __cplusplus
}
#endif

// Forward declaration / inclusion of RedjetSRAMCache
class RedjetSRAMCache;

// High-level Redjet aliases
using RedjetHardwareNexus        = hs_hardware_engine_nexus_t;
using RedjetHybridEngine          = hs_hybrid_homo_engine_t;
using RedjetNumaEngine            = hs_numa_uma_engine_t;
using RedjetSMTEngine             = hs_smt_ht_engine_t;
using RedjetSRAMCacheEngine       = hs_sram_cache_engine_t;
using RedjetSRAM                  = RedjetSRAMCache;
using RedjetUpclocker             = hs_upclocker_t;
using RedjetPMUSession            = hs_pmu_session_t;
using RedjetExecutionFocusEngine  = hs_execution_focus_engine_t;
using RedjetCPUISAInventory       = hs_cpu_isa_inventory_t;

#include "gc/redjet/allocator/redjetSRAMCache.hpp"

#endif // SHARE_GC_REDJET_REDJETHARDWARE_HPP
