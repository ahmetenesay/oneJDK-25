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

#ifndef SHARE_GC_REDLET_REDLETENTANGLE_HPP
#define SHARE_GC_REDLET_REDLETENTANGLE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redlet/redletGearkit.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 0. TENSOR DATA STRUCTURE (SacFor_VM.cpp:3092-3110)                        */
#ifndef SF_TENSOR_T_DEFINED
#define SF_TENSOR_T_DEFINED
typedef struct SF_custominiVm_tensor_t {
    void*   data{ nullptr };
    size_t  byte_size{ 0 };
    uint8_t dtype{ 0 };
    int32_t ndim{ 0 };
    int32_t shape[8]{ 0 };
} SF_custominiVm_tensor_t;
#endif

/* ========================================================================= */
/* 1. DAG NODE & TRACE CACHE STRUCTURES                                      */
/* ========================================================================= */

struct sf_entangle_dag_node_t {
    uint32_t    fn_id{ 0 };
    const void* primary_in{ nullptr };
    void*       primary_out{ nullptr };
    size_t      byte_size{ 0 };
    uint8_t     dtype{ 0 };
    uint8_t     is_fused_epilogue{ 0 };
};

struct sf_entangle_trace_cache_entry_t {
    uint64_t trace_hash{ 0 };
    uint32_t node_count{ 0 };
    uint32_t fused_edges{ 0 };
    uint32_t fn_ids[16]{ 0 };
    uint32_t warmup_hits{ 0 };
    uint32_t superkernel_fn_id{ 0 };
    uint8_t  is_compiled{ 0 };
    uint8_t  _pad[3]{ 0 };
};

/* ========================================================================= */
/* 2. ENTANGLE EXECUTION & KERNEL FUSION ENGINE (SacFor_VM.cpp:7238-7320)    */
/* ========================================================================= */

struct sf_entangle_execution_engine_t {
    static constexpr size_t MAX_DAG_NODES   = 64;
    static constexpr size_t TRACE_CACHE_CAP = 64;

    alignas(64) uint8_t   register_scratchpad[512] = { 0 };
    std::atomic<bool>     in_transaction{ false };
    std::atomic<uint32_t> transaction_depth{ 0 };
    std::atomic<uint32_t> fused_operation_count{ 0 };
    std::atomic<uint32_t> dag_node_count{ 0 };
    std::atomic<uint64_t> lifetime_fused_ops{ 0 };
    std::atomic<uint32_t> warmup_threshold{ 4 };
    std::atomic<uint32_t> total_warmup_hits{ 0 };
    std::atomic<uint32_t> compiled_superkernel_count{ 0 };

    std::mutex                      dag_lock;
    sf_entangle_dag_node_t          dag_nodes[MAX_DAG_NODES]{};
    sf_entangle_trace_cache_entry_t trace_cache[TRACE_CACHE_CAP]{};

    static inline uint64_t compute_dag_hash(const sf_entangle_dag_node_t* nodes, uint32_t count) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (uint32_t i = 0; i < count; ++i) {
            h ^= static_cast<uint64_t>(nodes[i].fn_id);
            h *= 1099511628211ULL;
            h ^= static_cast<uint64_t>(nodes[i].dtype) | (static_cast<uint64_t>(nodes[i].is_fused_epilogue) << 8);
            h *= 1099511628211ULL;
        }
        return h == 0 ? 1ULL : h;
    }

    MSIMD_API void begin();
    MSIMD_API void commit();
    MSIMD_API bool is_active() const { return in_transaction.load(std::memory_order_acquire); }
    MSIMD_API void record_op(uint32_t fn_id, const void* in_ptr, void* out_ptr, size_t byte_size, uint8_t dtype);
    MSIMD_API void record_kernel_op(uint32_t fn_id,
                                    const SF_custominiVm_tensor_t* inputs, int n_in,
                                    SF_custominiVm_tensor_t* outputs, int n_out);
    MSIMD_API void reset();
};

extern sf_entangle_execution_engine_t g_sf_entangle_engine;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API void     SF_custominiVm_entangle_begin(void);
MSIMD_API void     SF_custominiVm_entangle_commit(void);
MSIMD_API int      SF_custominiVm_entangle_is_active(void);
MSIMD_API void     SF_custominiVm_entangle_record_op(uint32_t fn_id,
                                                     const SF_custominiVm_tensor_t* inputs, int n_in,
                                                     SF_custominiVm_tensor_t* outputs, int n_out);
MSIMD_API uint32_t SF_custominiVm_entangle_get_fused_count(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_dag_depth(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_warmup_hits(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_superkernel_count(void);
MSIMD_API void     SF_custominiVm_entangle_set_warmup_threshold(uint32_t threshold);
MSIMD_API uint32_t SF_custominiVm_entangle_get_warmup_threshold(void);

#ifdef __cplusplus
}
#endif

using RedletEntangle = sf_entangle_execution_engine_t;

#endif // SHARE_GC_REDLET_REDLETENTANGLE_HPP
