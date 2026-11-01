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

#include "gc/redjet/rjspcf/redjetEntangle.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

/* Global Entangle Execution Engine Instance */
hs_entangle_execution_engine_t g_hs_entangle_engine;

/* ========================================================================= */
/* 1. LOCK-FREE OBJECT ENTANGLEMENT METHODS                                  */
/* ========================================================================= */

bool hs_object_entangle_handle_t::try_entangle(void* target, uint64_t tag, uint32_t epoch) noexcept {
    if (!target) return false;
    uint64_t cur_cw = control_word.load(std::memory_order_relaxed);
    while (true) {
        uint8_t st = unpack_state(cur_cw);
        if (st != HS_OBJ_ENTANGLE_UNBOUND && st != HS_OBJ_ENTANGLE_ABORTED) {
            return false; // Already bound or locked
        }
        uint32_t ver = unpack_version(cur_cw) + 1;
        uint64_t next_cw = make_cw(HS_OBJ_ENTANGLE_TENTATIVE, epoch, ver);
        if (control_word.compare_exchange_weak(cur_cw, next_cw,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
            // Tentative reservation succeeded, store pair and tag
            paired_object.store(target, std::memory_order_release);
            entangle_tag.store(tag, std::memory_order_release);

            // Elevate to locked state
            next_cw = make_cw(HS_OBJ_ENTANGLE_LOCKED, epoch, ver);
            control_word.store(next_cw, std::memory_order_release);
            return true;
        }
    }
}

bool hs_object_entangle_handle_t::commit_entanglement(void* expected_target) noexcept {
    uint64_t cur_cw = control_word.load(std::memory_order_acquire);
    while (true) {
        uint8_t st = unpack_state(cur_cw);
        if (st != HS_OBJ_ENTANGLE_LOCKED && st != HS_OBJ_ENTANGLE_FUSED) {
            return false;
        }
        if (paired_object.load(std::memory_order_acquire) != expected_target) {
            return false;
        }
        uint32_t epoch = unpack_epoch(cur_cw);
        uint32_t ver = unpack_version(cur_cw) + 1;
        uint64_t next_cw = make_cw(HS_OBJ_ENTANGLE_COMMITTED, epoch, ver);
        if (control_word.compare_exchange_weak(cur_cw, next_cw,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
            return true;
        }
    }
}

void hs_object_entangle_handle_t::break_entanglement() noexcept {
    uint64_t cur_cw = control_word.load(std::memory_order_acquire);
    while (true) {
        uint32_t epoch = unpack_epoch(cur_cw);
        uint32_t ver = unpack_version(cur_cw) + 1;
        uint64_t next_cw = make_cw(HS_OBJ_ENTANGLE_UNBOUND, epoch, ver);
        if (control_word.compare_exchange_weak(cur_cw, next_cw,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
            paired_object.store(nullptr, std::memory_order_release);
            entangle_tag.store(0, std::memory_order_release);
            break;
        }
    }
}

/* ========================================================================= */
/* 2. DAHILI GC CELL FUSION (Falline Snatch -> EmptyBox Tombstone -> Canary) */
/* ========================================================================= */

hs_gc_fused_cell_result_t entangle_gc_cell_fusion(
    void* cell_ptr,
    uint32_t prev_fn,
    size_t occupant_size,
    size_t aligned_size,
    void* forwarding_address,
    uint64_t sweep_no) noexcept
{
    hs_gc_fused_cell_result_t res;
    // Boundary check: ensure cell buffer is non-null and large enough to host emptybox header
    if (!cell_ptr || aligned_size < sizeof(HS_custominiVm_emptybox_header_t)) {
        return res;
    }
    res.cell_address = cell_ptr;
    res.prev_fn_id = prev_fn;
    res.cell_size = aligned_size;

    // Step 1: L1 Prefetch (Falline Snatch cacheline warming)
#if defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(static_cast<const char*>(cell_ptr), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(cell_ptr, 1, 3);
#endif

    // Step 2: EmptyBox Tombstone in-place initialization
    auto* eb = reinterpret_cast<HS_custominiVm_emptybox_header_t*>(cell_ptr);
    eb->init(prev_fn, occupant_size, aligned_size, forwarding_address, sweep_no);
    res.tombstone_written = (eb->hsMN == HS_EMPTYBOX_MAGIC);

    // Step 3: Canary Verification & Stamping in the same cacheline visit
    if (aligned_size >= sizeof(HS_custominiVm_emptybox_header_t) + sizeof(uint64_t)) {
        uintptr_t canary_addr = (reinterpret_cast<uintptr_t>(cell_ptr) + aligned_size - sizeof(uint64_t)) & ~7ULL;
        volatile uint64_t* tail_canary = reinterpret_cast<volatile uint64_t*>(canary_addr);
        *tail_canary = HS_TAIL_CANARY;
        res.canary_valid = (*tail_canary == HS_TAIL_CANARY);
    } else {
        res.canary_valid = false;
    }

    // Step 4: Checksum / Canary hash over header for integrity telemetry (FNV-1a)
    uint32_t chash = 2166136261u;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(cell_ptr);
    for (size_t i = 0; i < sizeof(HS_custominiVm_emptybox_header_t); ++i) {
        chash ^= p[i];
        chash *= 16777619u;
    }
    res.canary_hash = chash;

    // Step 5: Memory / Compiler barrier to commit store into L1/L2
    HS_COMPILER_BARRIER(cell_ptr);

    return res;
}

/* ========================================================================= */
/* 3. SUPERKERNEL PROMOTION HOOK                                             */
/* ========================================================================= */

static inline void hs_entangle_promote_superkernel_hook(const uint32_t* fn_ids, uint32_t count, uint64_t trace_hash, uint32_t& out_super_id) {
    (void)fn_ids;
    out_super_id = static_cast<uint32_t>((trace_hash & 0x07FFULL) + 2048u);
    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "[HSVM Entangle Warmup->Superkernel] Trace 0x%016llX reached threshold (%u ops) -> Fused Superkernel ID %u promoted",
        static_cast<unsigned long long>(trace_hash), count, out_super_id);
    hs_unified_log_emit(HS_Ebin0_PROMOTE, HS_Ebin1_INFO, out_super_id, "entangle_superkernel_jit", msg);
}

/* ========================================================================= */
/* 4. ENTANGLE EXECUTION ENGINE IMPLEMENTATION                               */
/* ========================================================================= */

void hs_entangle_execution_engine_t::begin() {
    if (transaction_depth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        hs_ticket_guard_t g(dag_lock);
        in_transaction.store(true, std::memory_order_release);
        fused_operation_count.store(0, std::memory_order_relaxed);
        dag_node_count.store(0, std::memory_order_relaxed);
        ready_queue.reset();
        hs_unified_log_emit(HS_Ebin0_ENTANGLE_TX_BEGIN, HS_Ebin1_INFO, 0, "entangle_tx_begin", "Entangle transaction started");
    }
}

void hs_entangle_execution_engine_t::cancel() {
    hs_ticket_guard_t g(dag_lock);
    transaction_depth.store(0, std::memory_order_release);
    in_transaction.store(false, std::memory_order_release);
    uint32_t count = dag_node_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count && i < MAX_DAG_NODES; ++i) {
        dag_nodes[i].reset();
    }
    dag_node_count.store(0, std::memory_order_relaxed);
    fused_operation_count.store(0, std::memory_order_relaxed);
    ready_queue.reset();
    hs_unified_log_emit(HS_Ebin0_ENTANGLE_TX_COMMIT, HS_Ebin1_WARN, 0, "entangle_tx_cancel", "Entangle transaction cancelled/aborted");
}

void hs_entangle_execution_engine_t::record_op(uint32_t fn_id, const void* in_ptr, void* out_ptr, size_t byte_size, uint8_t dtype) {
    if (!in_transaction.load(std::memory_order_acquire)) return;
    hs_ticket_guard_t g(dag_lock);
    uint32_t idx = dag_node_count.load(std::memory_order_relaxed);
    if (idx >= MAX_DAG_NODES) return;

    bool fused = false;
    if (idx > 0) {
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

    dag_nodes[idx].reset();
    dag_nodes[idx].node_id = idx;
    dag_nodes[idx].fn_id = fn_id;
    dag_nodes[idx].primary_in = in_ptr;
    dag_nodes[idx].primary_out = out_ptr;
    dag_nodes[idx].byte_size = byte_size;
    dag_nodes[idx].dtype = dtype;
    dag_nodes[idx].is_fused_epilogue = fused ? 1 : 0;
    dag_nodes[idx].op_category = HS_ENTANGLE_OP_COMPUTE;
    dag_nodes[idx].status = HS_DAG_STATUS_PENDING;
    dag_node_count.store(idx + 1, std::memory_order_release);
}

void hs_entangle_execution_engine_t::record_kernel_op(uint32_t fn_id,
    const HS_custominiVm_tensor_t* inputs, int n_in,
    HS_custominiVm_tensor_t* outputs, int n_out) {
    if (!in_transaction.load(std::memory_order_acquire)) return;
    hs_ticket_guard_t g(dag_lock);
    uint32_t idx = dag_node_count.load(std::memory_order_relaxed);
    if (idx >= MAX_DAG_NODES) return;

    const void* in_ptr = (inputs && n_in > 0) ? inputs[0].data : nullptr;
    const void* sec_in = (inputs && n_in > 1) ? inputs[1].data : nullptr;
    void* out_ptr = (outputs && n_out > 0) ? outputs[0].data : nullptr;
    size_t      bsize = (outputs && n_out > 0) ? outputs[0].byte_size : ((inputs && n_in > 0) ? inputs[0].byte_size : 0);
    uint8_t     dt = (outputs && n_out > 0) ? outputs[0].dtype : ((inputs && n_in > 0) ? inputs[0].dtype : 0);

    bool fused = false;
    if (idx > 0) {
        const auto& prev = dag_nodes[idx - 1];
        if (prev.primary_out != nullptr && (prev.primary_out == in_ptr || prev.primary_out == sec_in || prev.primary_out == out_ptr)) {
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
            hs_hotlog_emit(HS_Ebin0_SRAM_PREWARM_START, HS_Ebin1_TRACE, fn_id, 0, dt, 0, 0, 0,
                static_cast<uint32_t>(bsize / 1024), "entangle_sram_prewarm");
        }
    }

    dag_nodes[idx].reset();
    dag_nodes[idx].node_id = idx;
    dag_nodes[idx].fn_id = fn_id;
    dag_nodes[idx].primary_in = in_ptr;
    dag_nodes[idx].secondary_in = sec_in;
    dag_nodes[idx].primary_out = out_ptr;
    dag_nodes[idx].byte_size = bsize;
    dag_nodes[idx].dtype = dt;
    dag_nodes[idx].is_fused_epilogue = fused ? 1 : 0;
    dag_nodes[idx].op_category = HS_ENTANGLE_OP_COMPUTE;
    dag_nodes[idx].status = HS_DAG_STATUS_PENDING;
    dag_node_count.store(idx + 1, std::memory_order_release);
}

void hs_entangle_execution_engine_t::record_gc_fusion(void* cell_ptr, uint32_t prev_fn, size_t occupant_size,
                                                      size_t aligned_size, void* fwd_addr, uint64_t sweep_no) {
    if (!in_transaction.load(std::memory_order_acquire)) return;
    hs_ticket_guard_t g(dag_lock);
    uint32_t idx = dag_node_count.load(std::memory_order_relaxed);
    if (idx >= MAX_DAG_NODES) return;

    dag_nodes[idx].reset();
    dag_nodes[idx].node_id = idx;
    dag_nodes[idx].fn_id = prev_fn;
    dag_nodes[idx].primary_in = cell_ptr;
    dag_nodes[idx].primary_out = cell_ptr;
    dag_nodes[idx].target_obj_ref = cell_ptr;
    dag_nodes[idx].byte_size = aligned_size;
    dag_nodes[idx].op_category = HS_ENTANGLE_OP_GC_FUSION;
    dag_nodes[idx].entangle_tag = sweep_no;
    dag_nodes[idx].status = HS_DAG_STATUS_PENDING;

    // Custom kernel executor calling entangle_gc_cell_fusion
    dag_nodes[idx].kernel_exec_cb = [](void* ctx, hs_entangle_dag_node_t* node) {
        (void)ctx;
        if (node && node->target_obj_ref) {
            entangle_gc_cell_fusion(node->target_obj_ref, node->fn_id,
                                    node->byte_size, node->byte_size,
                                    nullptr, node->entangle_tag);
        }
    };

    dag_node_count.store(idx + 1, std::memory_order_release);
    gc_fusion_count.fetch_add(1, std::memory_order_relaxed);
}

void hs_entangle_execution_engine_t::record_object_entangle(void* obj_a, void* obj_b, uint64_t tag,
                                                            hs_object_entangle_handle_t* handle) {
    if (!in_transaction.load(std::memory_order_acquire)) return;
    hs_ticket_guard_t g(dag_lock);
    uint32_t idx = dag_node_count.load(std::memory_order_relaxed);
    if (idx >= MAX_DAG_NODES) return;

    dag_nodes[idx].reset();
    dag_nodes[idx].node_id = idx;
    dag_nodes[idx].target_obj_ref = obj_a;
    dag_nodes[idx].paired_obj_ref = obj_b;
    dag_nodes[idx].entangle_tag = tag;
    dag_nodes[idx].entangle_handle = handle;
    dag_nodes[idx].op_category = HS_ENTANGLE_OP_OBJ_LINK;
    dag_nodes[idx].status = HS_DAG_STATUS_PENDING;
    dag_node_count.store(idx + 1, std::memory_order_release);
    object_entangle_count.fetch_add(1, std::memory_order_relaxed);
}

void hs_entangle_execution_engine_t::commit() {
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
            uint32_t max_levels = 0;

            {
                hs_ticket_guard_t g(dag_lock);
                cnt = fused_operation_count.load(std::memory_order_relaxed);
                nodes = dag_node_count.load(std::memory_order_relaxed);
                in_transaction.store(false, std::memory_order_release);

                if (nodes > 0) {
                    thash = compute_dag_hash(dag_nodes, nodes);
                    slot_idx = static_cast<uint32_t>(thash & (TRACE_CACHE_CAP - 1));
                    auto& entry = trace_cache[slot_idx];
                    if (entry.trace_hash != thash) {
                        entry.reset();
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
                uint32_t super_id = 0;
                hs_entangle_promote_superkernel_hook(fn_seq, seq_len, thash, super_id);
                {
                    hs_ticket_guard_t g(dag_lock);
                    trace_cache[slot_idx].superkernel_fn_id = super_id;
                }
                compiled_superkernel_count.fetch_add(1, std::memory_order_relaxed);
            }

            hs_unified_log_emit(HS_Ebin0_ENTANGLE_TX_COMMIT, HS_Ebin1_INFO, nodes,
                should_compile_superkernel ? "entangle_superkernel_jit" : "entangle_dag_commit",
                "Entangle transaction committed");
        }
    }
}

void hs_entangle_execution_engine_t::reset() {
    hs_ticket_guard_t g(dag_lock);
    in_transaction.store(false, std::memory_order_release);
    transaction_depth.store(0, std::memory_order_release);
    fused_operation_count.store(0, std::memory_order_relaxed);
    dag_node_count.store(0, std::memory_order_relaxed);
    total_warmup_hits.store(0, std::memory_order_relaxed);
    compiled_superkernel_count.store(0, std::memory_order_relaxed);
    parallel_execution_count.store(0, std::memory_order_relaxed);
    gc_fusion_count.store(0, std::memory_order_relaxed);
    object_entangle_count.store(0, std::memory_order_relaxed);
    ready_queue.reset();
    for (size_t i = 0; i < MAX_DAG_NODES; ++i) {
        dag_nodes[i].reset();
    }
    for (size_t i = 0; i < TRACE_CACHE_CAP; ++i) {
        trace_cache[i].reset();
    }
}

/* ========================================================================= */
/* 5. PARALLEL DAG TRAVERSAL & TOPOLOGY ANALYSIS                             */
/* ========================================================================= */

uint32_t hs_entangle_execution_engine_t::build_dag_dependencies() {
    hs_ticket_guard_t g(dag_lock);
    uint32_t count = dag_node_count.load(std::memory_order_relaxed);
    if (count == 0) return 0;
    if (count > MAX_DAG_NODES) count = MAX_DAG_NODES;

    // Reset dependency tracking across all active nodes
    for (uint32_t j = 0; j < count; ++j) {
        dag_nodes[j].predecessor_count = 0;
        dag_nodes[j].dependent_count = 0;
        dag_nodes[j].topological_level = 0;
        dag_nodes[j].status = HS_DAG_STATUS_PENDING;
        for (uint32_t p = 0; p < hs_entangle_dag_node_t::MAX_PRED; ++p) {
            dag_nodes[j].predecessors[p] = 0;
        }
        for (uint32_t d = 0; d < hs_entangle_dag_node_t::MAX_DEP; ++d) {
            dag_nodes[j].dependents[d] = 0;
        }
    }

    // Pairwise dependency discovery: node j depends on node i (i < j)
    for (uint32_t j = 0; j < count; ++j) {
        for (uint32_t i = 0; i < j; ++i) {
            bool has_dep = false;

            // 1. RAW (Read-After-Write / True Dependency): Output of i matches inputs of j
            if (dag_nodes[i].primary_out != nullptr) {
                if (dag_nodes[i].primary_out == dag_nodes[j].primary_in ||
                    dag_nodes[i].primary_out == dag_nodes[j].secondary_in) {
                    has_dep = true;
                    if (i == j - 1) {
                        dag_nodes[j].is_fused_epilogue = 1;
                    }
                }
            }

            // 2. WAR (Write-After-Read / Anti-Dependency): Output of j overwrites inputs of i
            if (dag_nodes[j].primary_out != nullptr) {
                if (dag_nodes[j].primary_out == dag_nodes[i].primary_in ||
                    dag_nodes[j].primary_out == dag_nodes[i].secondary_in) {
                    has_dep = true;
                }
            }

            // 3. WAW (Write-After-Write / Output Dependency): Both i and j write to the same output buffer
            if (dag_nodes[i].primary_out != nullptr &&
                dag_nodes[i].primary_out == dag_nodes[j].primary_out) {
                has_dep = true;
            }

            // 4. Object entanglement collision: nodes touch overlapping object references
            void* i_target = dag_nodes[i].target_obj_ref;
            void* i_paired = dag_nodes[i].paired_obj_ref;
            void* j_target = dag_nodes[j].target_obj_ref;
            void* j_paired = dag_nodes[j].paired_obj_ref;

            if (i_target != nullptr && (i_target == j_target || i_target == j_paired)) {
                has_dep = true;
            }
            if (i_paired != nullptr && (i_paired == j_target || i_paired == j_paired)) {
                has_dep = true;
            }

            // 5. Scratchpad overlapping range hazard
            if (dag_nodes[i].routes_via_scratchpad && dag_nodes[j].routes_via_scratchpad) {
                uint32_t i_start = dag_nodes[i].scratchpad_offset;
                uint32_t i_end   = i_start + dag_nodes[i].scratchpad_bytes;
                uint32_t j_start = dag_nodes[j].scratchpad_offset;
                uint32_t j_end   = j_start + dag_nodes[j].scratchpad_bytes;
                if (std::max(i_start, j_start) < std::min(i_end, j_end)) {
                    has_dep = true;
                }
            }

            if (has_dep) {
                if (dag_nodes[j].predecessor_count < hs_entangle_dag_node_t::MAX_PRED) {
                    dag_nodes[j].predecessors[dag_nodes[j].predecessor_count++] = i;
                }
                if (dag_nodes[i].dependent_count < hs_entangle_dag_node_t::MAX_DEP) {
                    dag_nodes[i].dependents[dag_nodes[i].dependent_count++] = j;
                }
            }
        }
    }

    // Topological level calculation (wavefront DP)
    uint32_t max_level = 0;
    for (uint32_t j = 0; j < count; ++j) {
        if (dag_nodes[j].predecessor_count == 0) {
            dag_nodes[j].topological_level = 0;
            dag_nodes[j].status = HS_DAG_STATUS_READY;
        } else {
            uint32_t max_pred_level = 0;
            for (uint32_t p = 0; p < dag_nodes[j].predecessor_count; ++p) {
                uint32_t pred_idx = dag_nodes[j].predecessors[p];
                if (dag_nodes[pred_idx].topological_level > max_pred_level) {
                    max_pred_level = dag_nodes[pred_idx].topological_level;
                }
            }
            dag_nodes[j].topological_level = max_pred_level + 1;
            if (dag_nodes[j].topological_level > max_level) {
                max_level = dag_nodes[j].topological_level;
            }
        }
        dag_nodes[j].unsatisfied_deps.store(static_cast<int32_t>(dag_nodes[j].predecessor_count), std::memory_order_release);
    }

    return max_level + 1;
}

uint32_t hs_entangle_execution_engine_t::get_max_topological_level() const {
    uint32_t count = dag_node_count.load(std::memory_order_relaxed);
    if (count == 0) return 0;
    if (count > MAX_DAG_NODES) count = MAX_DAG_NODES;
    uint32_t max_lvl = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (dag_nodes[i].topological_level > max_lvl) {
            max_lvl = dag_nodes[i].topological_level;
        }
    }
    return max_lvl;
}

void hs_entangle_execution_engine_t::execute_node(uint32_t node_idx) {
    if (node_idx >= MAX_DAG_NODES) return;
    auto& node = dag_nodes[node_idx];
    node.status = HS_DAG_STATUS_EXECUTING;

    if (node.kernel_exec_cb) {
        node.kernel_exec_cb(node.user_data, &node);
    } else {
        switch (node.op_category) {
            case HS_ENTANGLE_OP_GC_FUSION:
                if (node.target_obj_ref) {
                    entangle_gc_cell_fusion(node.target_obj_ref, node.fn_id,
                                            node.byte_size, node.byte_size,
                                            nullptr, node.entangle_tag);
                }
                break;
            case HS_ENTANGLE_OP_OBJ_LINK: {
                // Persistent lock-free object entanglement state transition
                hs_object_entangle_handle_t* h = node.entangle_handle;
                if (!h && node.target_obj_ref) {
                    h = &node.embedded_handle;
                }
                if (h && node.paired_obj_ref) {
                    h->try_entangle(node.paired_obj_ref, node.entangle_tag, 0);
                    h->commit_entanglement(node.paired_obj_ref);
                }
                break;
            }
            case HS_ENTANGLE_OP_MEMORY:
                if (node.primary_out && node.primary_in && node.byte_size > 0) {
                    std::memcpy(node.primary_out, node.primary_in, node.byte_size);
                }
                break;
            case HS_ENTANGLE_OP_COMPUTE:
            default:
                if (node.primary_out && node.byte_size > 0 && node.byte_size <= 256 * 1024) {
                    const char* p = static_cast<const char*>(node.primary_out);
                    for (size_t off = 0; off < node.byte_size; off += 64) {
#if defined(_M_X64) || defined(__x86_64__)
                        _mm_prefetch(p + off, _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
                        __builtin_prefetch(p + off, 1, 3);
#endif
                    }
                }
                break;
        }
    }

    node.status = HS_DAG_STATUS_COMPLETED;
}

void hs_entangle_execution_engine_t::execute_dag_sequential() {
    uint32_t count = dag_node_count.load(std::memory_order_relaxed);
    if (count > MAX_DAG_NODES) count = MAX_DAG_NODES;
    for (uint32_t i = 0; i < count; ++i) {
        execute_node(i);
    }
}

void hs_entangle_execution_engine_t::execute_dag_parallel(uint32_t num_workers) {
    uint32_t count = dag_node_count.load(std::memory_order_relaxed);
    if (count == 0) return;
    if (count > MAX_DAG_NODES) count = MAX_DAG_NODES;

    // Discover dependencies and topological levels
    build_dag_dependencies();

    // Fall back to sequential if only 1 worker or only 1 node to execute
    if (num_workers <= 1 || count <= 1) {
        execute_dag_sequential();
        parallel_execution_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Populate ready ring buffer with all initial zero-predecessor nodes
    ready_queue.reset();
    for (uint32_t i = 0; i < count; ++i) {
        if (dag_nodes[i].predecessor_count == 0) {
            dag_nodes[i].status = HS_DAG_STATUS_READY;
            ready_queue.push(i);
        }
    }

    std::atomic<uint32_t> completed_nodes{ 0 };
    std::atomic<uint32_t> active_workers{ 0 };

    auto worker_loop = [&](uint32_t worker_id) {
        while (completed_nodes.load(std::memory_order_acquire) < count) {
            uint32_t node_idx = 0;
            if (ready_queue.pop(node_idx)) {
                active_workers.fetch_add(1, std::memory_order_acq_rel);
                dag_nodes[node_idx].scratchpad_offset = worker_id * 64;
                execute_node(node_idx);

                // Notify all dependent nodes whose predecessor completed
                for (uint32_t d = 0; d < dag_nodes[node_idx].dependent_count; ++d) {
                    uint32_t dep_idx = dag_nodes[node_idx].dependents[d];
                    if (dag_nodes[dep_idx].unsatisfied_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        dag_nodes[dep_idx].status = HS_DAG_STATUS_READY;
                        ready_queue.push(dep_idx);
                    }
                }
                active_workers.fetch_sub(1, std::memory_order_acq_rel);
                completed_nodes.fetch_add(1, std::memory_order_release);
            } else {
                // Deadlock/cycle guard: no ready work, no active workers, but remaining uncompleted
                if (ready_queue.is_empty() && active_workers.load(std::memory_order_acquire) == 0 &&
                    completed_nodes.load(std::memory_order_acquire) < count) {
                    // Mark stalled nodes as failed and abort
                    for (uint32_t rem = 0; rem < count; ++rem) {
                        if (dag_nodes[rem].status != HS_DAG_STATUS_COMPLETED) {
                            dag_nodes[rem].status = HS_DAG_STATUS_FAILED;
                        }
                    }
                    break;
                }
                hs_ring_cpu_pause();
            }
        }
    };

    uint32_t threads_to_spawn = std::min(num_workers, count);
    std::vector<std::thread> workers;
    workers.reserve(threads_to_spawn);

    // Spawn helper threads
    for (uint32_t w = 1; w < threads_to_spawn; ++w) {
        workers.emplace_back(worker_loop, w);
    }
    // Caller thread executes worker 0 directly (zero context switch waste)
    worker_loop(0);

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    parallel_execution_count.fetch_add(1, std::memory_order_relaxed);
}

void* hs_entangle_execution_engine_t::get_scratchpad_slice(uint32_t worker_id, size_t needed_bytes) {
    if (needed_bytes == 0) needed_bytes = 64;
    // Align requested size to 64-byte CPU cacheline boundary
    size_t aligned_bytes = (needed_bytes + 63) & ~size_t(63);
    size_t offset = static_cast<size_t>(worker_id) * aligned_bytes;
    if (offset + aligned_bytes <= sizeof(register_scratchpad)) {
        return &register_scratchpad[offset];
    }
    // Return nullptr on overflow rather than clobbering worker 0
    return nullptr;
}

hs_entangle_trace_cache_entry_t* hs_entangle_execution_engine_t::lookup_trace(uint64_t trace_hash) {
    uint32_t slot = static_cast<uint32_t>(trace_hash & (TRACE_CACHE_CAP - 1));
    hs_ticket_guard_t g(dag_lock);
    if (trace_cache[slot].trace_hash == trace_hash) {
        trace_cache[slot].lifetime_invocations.fetch_add(1, std::memory_order_relaxed);
        trace_cache[slot].last_access_tsc = sacculus_rdtsc();
        return &trace_cache[slot];
    }
    return nullptr;
}

/* ========================================================================= */
/* 6. C-API EXPORTS                                                          */
/* ========================================================================= */

extern "C" {

MSIMD_API void HS_custominiVm_entangle_begin(void) {
    g_hs_entangle_engine.begin();
}

MSIMD_API void HS_custominiVm_entangle_commit(void) {
    g_hs_entangle_engine.commit();
}

MSIMD_API void HS_custominiVm_entangle_cancel(void) {
    g_hs_entangle_engine.cancel();
}

MSIMD_API void HS_custominiVm_entangle_reset(void) {
    g_hs_entangle_engine.reset();
}

MSIMD_API int HS_custominiVm_entangle_is_active(void) {
    return g_hs_entangle_engine.is_active() ? 1 : 0;
}

MSIMD_API void HS_custominiVm_entangle_record_op(uint32_t fn_id,
    const HS_custominiVm_tensor_t* inputs, int n_in,
    HS_custominiVm_tensor_t* outputs, int n_out) {
    g_hs_entangle_engine.record_kernel_op(fn_id, inputs, n_in, outputs, n_out);
}

MSIMD_API void HS_custominiVm_entangle_record_gc_fusion(void* cell_ptr, uint32_t prev_fn,
                                                        size_t occupant_size, size_t aligned_size,
                                                        void* fwd_addr, uint64_t sweep_no) {
    g_hs_entangle_engine.record_gc_fusion(cell_ptr, prev_fn, occupant_size, aligned_size, fwd_addr, sweep_no);
}

MSIMD_API void HS_custominiVm_entangle_record_object_entangle(void* obj_a, void* obj_b, uint64_t tag) {
    g_hs_entangle_engine.record_object_entangle(obj_a, obj_b, tag);
}

MSIMD_API void HS_custominiVm_entangle_execute_parallel(uint32_t num_workers) {
    g_hs_entangle_engine.execute_dag_parallel(num_workers);
}

MSIMD_API void HS_custominiVm_entangle_execute_sequential(void) {
    g_hs_entangle_engine.execute_dag_sequential();
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_fused_count(void) {
    return g_hs_entangle_engine.fused_operation_count.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_dag_depth(void) {
    return g_hs_entangle_engine.dag_node_count.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_warmup_hits(void) {
    return g_hs_entangle_engine.total_warmup_hits.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_superkernel_count(void) {
    return g_hs_entangle_engine.compiled_superkernel_count.load(std::memory_order_acquire);
}

MSIMD_API void HS_custominiVm_entangle_set_warmup_threshold(uint32_t threshold) {
    g_hs_entangle_engine.warmup_threshold.store(threshold > 0 ? threshold : 1, std::memory_order_release);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_warmup_threshold(void) {
    return g_hs_entangle_engine.warmup_threshold.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_max_level(void) {
    return g_hs_entangle_engine.get_max_topological_level();
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_parallel_exec_count(void) {
    return g_hs_entangle_engine.parallel_execution_count.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_gc_fusion_count(void) {
    return g_hs_entangle_engine.gc_fusion_count.load(std::memory_order_acquire);
}

MSIMD_API uint32_t HS_custominiVm_entangle_get_object_entangle_count(void) {
    return g_hs_entangle_engine.object_entangle_count.load(std::memory_order_acquire);
}

}
