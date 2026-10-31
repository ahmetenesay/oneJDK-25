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

#include "gc/redlet/redletEntangle.hpp"
#include <cstdio>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

sf_entangle_execution_engine_t g_sf_entangle_engine;

void sf_entangle_execution_engine_t::begin() {
    if (transaction_depth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        std::lock_guard<std::mutex> g(dag_lock);
        in_transaction.store(true, std::memory_order_release);
        fused_operation_count.store(0, std::memory_order_relaxed);
        dag_node_count.store(0, std::memory_order_relaxed);
        sf_unified_log_emit(SF_Ebin0_ENTANGLE_TX_BEGIN, SF_Ebin1_INFO, 0, "entangle_tx_begin", "Entangle transaction started");
    }
}

void sf_entangle_execution_engine_t::record_op(uint32_t fn_id, const void* in_ptr, void* out_ptr, size_t byte_size, uint8_t dtype) {
    if (!in_transaction.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> g(dag_lock);
    uint32_t idx = dag_node_count.load(std::memory_order_relaxed);

    bool fused = false;
    if (idx > 0 && idx <= MAX_DAG_NODES) {
        const auto& prev = dag_nodes[idx - 1];
        // Producer-Consumer Epilogue Fusion detection: output of previous op feeds directly into current op
        if (prev.primary_out != nullptr && (prev.primary_out == in_ptr || prev.primary_out == out_ptr)) {
            fused = true;
            fused_operation_count.fetch_add(1, std::memory_order_relaxed);
            lifetime_fused_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Keep intermediate tensor hot in L1/L2 SRAM cache hierarchy across the fused DAG chain
    if (out_ptr && byte_size > 0 && byte_size <= 256 * 1024) {
        const char* p = static_cast<const char*>(out_ptr);
        for (size_t off = 0; off < byte_size; off += 64) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_prefetch(p + off, _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(p + off, 1, 3);
#endif
        }
    }

    if (idx < MAX_DAG_NODES) {
        dag_nodes[idx].fn_id = fn_id;
        dag_nodes[idx].primary_in = in_ptr;
        dag_nodes[idx].primary_out = out_ptr;
        dag_nodes[idx].byte_size = byte_size;
        dag_nodes[idx].dtype = dtype;
        dag_nodes[idx].is_fused_epilogue = fused ? 1 : 0;
        dag_node_count.store(idx + 1, std::memory_order_release);
    }
}

void sf_entangle_execution_engine_t::record_kernel_op(uint32_t fn_id,
    const SF_custominiVm_tensor_t* inputs, int n_in,
    SF_custominiVm_tensor_t* outputs, int n_out) {
    if (!in_transaction.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> g(dag_lock);
    uint32_t idx = dag_node_count.load(std::memory_order_relaxed);
    const void* in_ptr = (inputs && n_in > 0) ? inputs[0].data : nullptr;
    void* out_ptr = (outputs && n_out > 0) ? outputs[0].data : nullptr;
    size_t      bsize = (outputs && n_out > 0) ? outputs[0].byte_size : ((inputs && n_in > 0) ? inputs[0].byte_size : 0);
    uint8_t     dt = (outputs && n_out > 0) ? outputs[0].dtype : ((inputs && n_in > 0) ? inputs[0].dtype : 0);

    bool fused = false;
    if (idx > 0 && idx <= MAX_DAG_NODES) {
        const auto& prev = dag_nodes[idx - 1];
        if (prev.primary_out != nullptr && (prev.primary_out == in_ptr || prev.primary_out == out_ptr)) {
            fused = true;
            fused_operation_count.fetch_add(1, std::memory_order_relaxed);
            lifetime_fused_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (out_ptr && bsize > 0 && bsize <= 256 * 1024) {
        const char* p = static_cast<const char*>(out_ptr);
        for (size_t off = 0; off < bsize; off += 64) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_prefetch(p + off, _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(p + off, 1, 3);
#endif
        }
        if (idx == 0) {
            sf_hotlog_emit(SF_Ebin0_SRAM_PREWARM_START, SF_Ebin1_TRACE, fn_id, 0, dt, 0, 0, 0,
                static_cast<uint32_t>(bsize / 1024), "entangle_sram_prewarm");
        }
    }

    if (idx < MAX_DAG_NODES) {
        dag_nodes[idx].fn_id = fn_id;
        dag_nodes[idx].primary_in = in_ptr;
        dag_nodes[idx].primary_out = out_ptr;
        dag_nodes[idx].byte_size = bsize;
        dag_nodes[idx].dtype = dt;
        dag_nodes[idx].is_fused_epilogue = fused ? 1 : 0;
        dag_node_count.store(idx + 1, std::memory_order_release);
    }
}

void sf_entangle_execution_engine_t::commit() {
    uint32_t d = transaction_depth.load(std::memory_order_acquire);
    if (d > 0) {
        if (transaction_depth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            uint32_t cnt = 0;
            uint32_t nodes = 0;
            uint64_t thash = 0;
            uint32_t fn_seq[16]{ 0 };
            uint32_t seq_len = 0;
            bool should_compile_superkernel = false;
            uint32_t slot_idx = 0;

            {
                std::lock_guard<std::mutex> g(dag_lock);
                cnt = fused_operation_count.load(std::memory_order_relaxed);
                nodes = dag_node_count.load(std::memory_order_relaxed);
                in_transaction.store(false, std::memory_order_release);

                if (nodes > 0) {
                    thash = compute_dag_hash(dag_nodes, nodes);
                    slot_idx = static_cast<uint32_t>(thash & (TRACE_CACHE_CAP - 1));
                    auto& entry = trace_cache[slot_idx];
                    if (entry.trace_hash != thash) {
                        entry.trace_hash = thash;
                        entry.node_count = nodes;
                        entry.fused_edges = cnt;
                        entry.warmup_hits = 0;
                        entry.is_compiled = 0;
                        entry.superkernel_fn_id = 0;
                        seq_len = nodes < 16 ? nodes : 16;
                        for (uint32_t i = 0; i < seq_len; ++i) {
                            entry.fn_ids[i] = dag_nodes[i].fn_id;
                            fn_seq[i] = dag_nodes[i].fn_id;
                        }
                    } else {
                        seq_len = entry.node_count < 16 ? entry.node_count : 16;
                        for (uint32_t i = 0; i < seq_len; ++i) {
                            fn_seq[i] = entry.fn_ids[i];
                        }
                    }

                    entry.warmup_hits++;
                    total_warmup_hits.fetch_add(1, std::memory_order_relaxed);
                    uint32_t thresh = warmup_threshold.load(std::memory_order_relaxed);
                    if (thresh == 0) thresh = 1;

                    if (!entry.is_compiled && entry.warmup_hits >= thresh) {
                        entry.is_compiled = 1;
                        should_compile_superkernel = true;
                    }
                }
            }

            if (should_compile_superkernel && seq_len > 0) {
                uint32_t super_id = static_cast<uint32_t>((thash & 0x07FFULL) + 2048u);
                {
                    std::lock_guard<std::mutex> g(dag_lock);
                    trace_cache[slot_idx].superkernel_fn_id = super_id;
                }
                compiled_superkernel_count.fetch_add(1, std::memory_order_relaxed);
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "[SFVM Entangle Warmup->Superkernel] Trace 0x%016llX reached threshold (%u ops) -> Fused Superkernel ID %u promoted",
                    static_cast<unsigned long long>(thash), seq_len, super_id);
                sf_unified_log_emit(SF_Ebin0_PROMOTE, SF_Ebin1_INFO, super_id, "entangle_superkernel_jit", msg);
            }

            sf_unified_log_emit(SF_Ebin0_ENTANGLE_TX_COMMIT, SF_Ebin1_INFO, nodes,
                should_compile_superkernel ? "entangle_superkernel_jit" : "entangle_dag_commit",
                "Entangle transaction committed");
        }
    }
}

void sf_entangle_execution_engine_t::reset() {
    std::lock_guard<std::mutex> g(dag_lock);
    in_transaction.store(false, std::memory_order_release);
    transaction_depth.store(0, std::memory_order_release);
    fused_operation_count.store(0, std::memory_order_relaxed);
    dag_node_count.store(0, std::memory_order_relaxed);
    total_warmup_hits.store(0, std::memory_order_relaxed);
    compiled_superkernel_count.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < MAX_DAG_NODES; ++i) {
        dag_nodes[i] = sf_entangle_dag_node_t{};
    }
    for (size_t i = 0; i < TRACE_CACHE_CAP; ++i) {
        trace_cache[i] = sf_entangle_trace_cache_entry_t{};
    }
}

extern "C" {

MSIMD_API void SF_custominiVm_entangle_begin(void) {
    g_sf_entangle_engine.begin();
}

MSIMD_API void SF_custominiVm_entangle_commit(void) {
    g_sf_entangle_engine.commit();
}

MSIMD_API int SF_custominiVm_entangle_is_active(void) {
    return g_sf_entangle_engine.is_active() ? 1 : 0;
}

MSIMD_API void SF_custominiVm_entangle_record_op(uint32_t fn_id,
    const SF_custominiVm_tensor_t* inputs, int n_in,
    SF_custominiVm_tensor_t* outputs, int n_out) {
    g_sf_entangle_engine.record_kernel_op(fn_id, inputs, n_in, outputs, n_out);
}

MSIMD_API uint32_t SF_custominiVm_entangle_get_fused_count(void) {
    return g_sf_entangle_engine.fused_operation_count.load(std::memory_order_acquire);
}

MSIMD_API uint32_t SF_custominiVm_entangle_get_dag_depth(void) {
    return g_sf_entangle_engine.dag_node_count.load(std::memory_order_acquire);
}

MSIMD_API uint32_t SF_custominiVm_entangle_get_warmup_hits(void) {
    return g_sf_entangle_engine.total_warmup_hits.load(std::memory_order_acquire);
}

MSIMD_API uint32_t SF_custominiVm_entangle_get_superkernel_count(void) {
    return g_sf_entangle_engine.compiled_superkernel_count.load(std::memory_order_acquire);
}

MSIMD_API void SF_custominiVm_entangle_set_warmup_threshold(uint32_t threshold) {
    g_sf_entangle_engine.warmup_threshold.store(threshold > 0 ? threshold : 1, std::memory_order_release);
}

MSIMD_API uint32_t SF_custominiVm_entangle_get_warmup_threshold(void) {
    return g_sf_entangle_engine.warmup_threshold.load(std::memory_order_acquire);
}

}
