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

#ifndef SHARE_GC_REDJET_REDJETENTANGLE_HPP
#define SHARE_GC_REDJET_REDJETENTANGLE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 0. TENSOR DATA STRUCTURE (SacFor_VM.cpp:3092-3110)                        */
/* ========================================================================= */
#ifndef HS_TENSOR_T_DEFINED
#define HS_TENSOR_T_DEFINED
typedef struct HS_hotspot_tensor_t {
    void*   data{ nullptr };
    size_t  byte_size{ 0 };
    uint8_t dtype{ 0 };
    int32_t ndim{ 0 };
    int32_t shape[8]{ 0 };
} HS_hotspot_tensor_t;
#endif

#ifndef HS_TAIL_CANARY
#define HS_TAIL_CANARY 0xDEADBEEFCAFEF00DULL
#endif

/* ========================================================================= */
/* 1. DAG NODE & TRACE CACHE ENUMS & TYPES                                   */
/* ========================================================================= */

enum hs_dag_op_category_t : uint8_t {
    HS_ENTANGLE_OP_COMPUTE     = 0, // Arithmetic / Vector / Tensor operation
    HS_ENTANGLE_OP_MEMORY      = 1, // Memory copy / DMA / SRAM prefetch transfer
    HS_ENTANGLE_OP_GC_FUSION   = 2, // Fused GC Sweep (Falline Snatch + EmptyBox + Canary)
    HS_ENTANGLE_OP_OBJ_LINK    = 3  // Lock-free Object Entanglement State Transition
};

enum hs_dag_node_status_t : uint8_t {
    HS_DAG_STATUS_PENDING      = 0,
    HS_DAG_STATUS_READY        = 1,
    HS_DAG_STATUS_EXECUTING    = 2,
    HS_DAG_STATUS_COMPLETED    = 3,
    HS_DAG_STATUS_FAILED       = 4
};

enum hs_object_entangle_state_e : uint8_t {
    HS_OBJ_ENTANGLE_UNBOUND   = 0,
    HS_OBJ_ENTANGLE_TENTATIVE = 1,
    HS_OBJ_ENTANGLE_LOCKED    = 2,
    HS_OBJ_ENTANGLE_FUSED     = 3,
    HS_OBJ_ENTANGLE_COMMITTED = 4,
    HS_OBJ_ENTANGLE_ABORTED   = 5
};

/* ========================================================================= */
/* 2. LOCK-FREE OBJECT ENTANGLEMENT & GC CELL FUSION STRUCTURES              */
/* ========================================================================= */

struct SACFOR_ALIGN_CL hs_object_entangle_handle_t {
    // 64-bit control word: [63..32: version_counter] [31..8: epoch] [7..0: state]
    alignas(64) std::atomic<uint64_t> control_word{ 0 };
    alignas(64) std::atomic<void*>    paired_object{ nullptr };
    alignas(64) std::atomic<uint64_t> entangle_tag{ 0 };

    static inline uint64_t make_cw(uint8_t state, uint32_t epoch, uint32_t version) noexcept {
        return (static_cast<uint64_t>(version) << 32) |
               (static_cast<uint64_t>(epoch & 0x00FFFFFFu) << 8) |
               static_cast<uint64_t>(state);
    }

    static inline uint8_t unpack_state(uint64_t cw) noexcept {
        return static_cast<uint8_t>(cw & 0xFFu);
    }

    static inline uint32_t unpack_epoch(uint64_t cw) noexcept {
        return static_cast<uint32_t>((cw >> 8) & 0x00FFFFFFu);
    }

    static inline uint32_t unpack_version(uint64_t cw) noexcept {
        return static_cast<uint32_t>(cw >> 32);
    }

    inline uint8_t get_state() const noexcept {
        return unpack_state(control_word.load(std::memory_order_acquire));
    }

    inline hs_object_entangle_state_e get_state_enum() const noexcept {
        return static_cast<hs_object_entangle_state_e>(get_state());
    }

    inline bool is_entangled() const noexcept {
        uint8_t s = get_state();
        return s == HS_OBJ_ENTANGLE_TENTATIVE || s == HS_OBJ_ENTANGLE_LOCKED ||
               s == HS_OBJ_ENTANGLE_FUSED || s == HS_OBJ_ENTANGLE_COMMITTED;
    }

    inline void* get_paired_object() const noexcept {
        return paired_object.load(std::memory_order_acquire);
    }

    inline uint64_t get_tag() const noexcept {
        return entangle_tag.load(std::memory_order_acquire);
    }

    RJ_API bool try_entangle(void* target, uint64_t tag, uint32_t epoch) noexcept;
    RJ_API bool commit_entanglement(void* expected_target) noexcept;
    RJ_API void break_entanglement() noexcept;
};

// Result structure for fused GC sweep on a cell line
struct hs_gc_fused_cell_result_t {
    void*    cell_address{ nullptr };
    uint32_t prev_fn_id{ 0 };
    size_t   cell_size{ 0 };
    uint32_t canary_hash{ 0 };
    bool     tombstone_written{ false };
    bool     canary_valid{ false };
};

RJ_API hs_gc_fused_cell_result_t entangle_gc_cell_fusion(
    void* cell_ptr,
    uint32_t prev_fn,
    size_t occupant_size,
    size_t aligned_size,
    void* forwarding_address,
    uint64_t sweep_no) noexcept;

/* ========================================================================= */
/* 3. DAG NODE STRUCTURE                                                     */
/* ========================================================================= */

struct hs_entangle_dag_node_t;
typedef void (*hs_entangle_node_kernel_fn)(void* ctx, hs_entangle_dag_node_t* node);

struct hs_entangle_dag_node_t {
    // Primary identification and data pointers (ABI preserved)
    uint32_t                    fn_id{ 0 };
    const void*                 primary_in{ nullptr };
    void*                       primary_out{ nullptr };
    size_t                      byte_size{ 0 };
    uint8_t                     dtype{ 0 };
    uint8_t                     is_fused_epilogue{ 0 };

    // Extended topology and scheduling attributes
    uint32_t                    node_id{ 0 };
    const void*                 secondary_in{ nullptr };
    uint8_t                     op_category{ HS_ENTANGLE_OP_COMPUTE };
    uint8_t                     status{ HS_DAG_STATUS_PENDING };

    // Predecessor and dependent tracking for parallel DAG traversal (Rich fan-in/fan-out)
    static constexpr uint32_t   MAX_PRED = 8;
    static constexpr uint32_t   MAX_DEP  = 16;
    uint32_t                    predecessor_count{ 0 };
    uint32_t                    predecessors[MAX_PRED]{ 0 };
    uint32_t                    dependent_count{ 0 };
    uint32_t                    dependents[MAX_DEP]{ 0 };
    std::atomic<int32_t>        unsatisfied_deps{ 0 };
    uint32_t                    topological_level{ 0 };

    // Object entanglement references (GC cells or tensor objects)
    void*                       target_obj_ref{ nullptr };
    void*                       paired_obj_ref{ nullptr };
    uint64_t                    entangle_tag{ 0 };
    hs_object_entangle_handle_t* entangle_handle{ nullptr };
    hs_object_entangle_handle_t  embedded_handle;

    // Intermediate scratchpad routing
    uint32_t                    scratchpad_offset{ 0 };
    uint32_t                    scratchpad_bytes{ 0 };
    bool                        routes_via_scratchpad{ false };

    // Custom execution delegate
    hs_entangle_node_kernel_fn  kernel_exec_cb{ nullptr };
    void*                       user_data{ nullptr };

    void reset() noexcept {
        fn_id = 0;
        primary_in = nullptr;
        primary_out = nullptr;
        byte_size = 0;
        dtype = 0;
        is_fused_epilogue = 0;
        node_id = 0;
        secondary_in = nullptr;
        op_category = HS_ENTANGLE_OP_COMPUTE;
        status = HS_DAG_STATUS_PENDING;
        predecessor_count = 0;
        for (uint32_t i = 0; i < MAX_PRED; ++i) predecessors[i] = 0;
        dependent_count = 0;
        for (uint32_t i = 0; i < MAX_DEP; ++i) dependents[i] = 0;
        unsatisfied_deps.store(0, std::memory_order_relaxed);
        topological_level = 0;
        target_obj_ref = nullptr;
        paired_obj_ref = nullptr;
        entangle_tag = 0;
        entangle_handle = nullptr;
        embedded_handle.break_entanglement();
        scratchpad_offset = 0;
        scratchpad_bytes = 0;
        routes_via_scratchpad = false;
        kernel_exec_cb = nullptr;
        user_data = nullptr;
    }
};

/* ========================================================================= */
/* 4. TRACE CACHE & TICKET-GUARDED READY RING BUFFER                         */
/* ========================================================================= */

typedef void (*hs_superkernel_fn_t)(void* scratchpad, const void** inputs, uint32_t num_inputs, void** outputs, uint32_t num_outputs);

struct hs_entangle_trace_cache_entry_t {
    uint64_t            trace_hash{ 0 };
    uint32_t            node_count{ 0 };
    uint32_t            fused_edges{ 0 };
    uint32_t            fn_ids[16]{ 0 };
    uint32_t            warmup_hits{ 0 };
    uint32_t            superkernel_fn_id{ 0 };
    uint8_t             is_compiled{ 0 };
    uint8_t             max_parallel_level{ 0 };
    uint16_t            total_bandwidth_saved_kb{ 0 };
    hs_superkernel_fn_t superkernel_exec_fn{ nullptr };
    uint64_t            last_access_tsc{ 0 };
    std::atomic<uint64_t> lifetime_invocations{ 0 };

    void reset() noexcept {
        trace_hash = 0;
        node_count = 0;
        fused_edges = 0;
        for (int i = 0; i < 16; ++i) fn_ids[i] = 0;
        warmup_hits = 0;
        superkernel_fn_id = 0;
        is_compiled = 0;
        max_parallel_level = 0;
        total_bandwidth_saved_kb = 0;
        superkernel_exec_fn = nullptr;
        last_access_tsc = 0;
        lifetime_invocations.store(0, std::memory_order_relaxed);
    }
};

struct hs_lockfree_dag_queue_t {
    static constexpr size_t QUEUE_CAP = 128;
    alignas(64) hs_ticket_lock_t lock;
    uint32_t buffer[QUEUE_CAP]{ 0 };
    uint32_t head{ 0 };
    uint32_t tail{ 0 };
    uint32_t count{ 0 };

    inline void reset() noexcept {
        hs_ticket_guard_t g(lock);
        head = 0;
        tail = 0;
        count = 0;
        for (size_t i = 0; i < QUEUE_CAP; ++i) buffer[i] = 0;
    }

    inline bool push(uint32_t node_idx) noexcept {
        hs_ticket_guard_t g(lock);
        if (count >= QUEUE_CAP) return false;
        buffer[tail] = node_idx;
        tail = (tail + 1) % QUEUE_CAP;
        count++;
        return true;
    }

    inline bool pop(uint32_t& out_node_idx) noexcept {
        hs_ticket_guard_t g(lock);
        if (count == 0) return false;
        out_node_idx = buffer[head];
        head = (head + 1) % QUEUE_CAP;
        count--;
        return true;
    }

    inline bool is_empty() noexcept {
        hs_ticket_guard_t g(lock);
        return count == 0;
    }

    inline size_t size() noexcept {
        hs_ticket_guard_t g(lock);
        return count;
    }
};

/* ========================================================================= */
/* 5. ENTANGLE EXECUTION & KERNEL FUSION ENGINE (SacFor_VM.cpp:7238-7320)    */
/* ========================================================================= */

struct hs_entangle_execution_engine_t {
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
    std::atomic<uint32_t> parallel_execution_count{ 0 };
    std::atomic<uint32_t> gc_fusion_count{ 0 };
    std::atomic<uint32_t> object_entangle_count{ 0 };

    hs_ticket_lock_t                dag_lock;
    hs_entangle_dag_node_t          dag_nodes[MAX_DAG_NODES]{};
    hs_entangle_trace_cache_entry_t trace_cache[TRACE_CACHE_CAP]{};
    hs_lockfree_dag_queue_t         ready_queue;

    static inline uint64_t compute_dag_hash(const hs_entangle_dag_node_t* nodes, uint32_t count) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (uint32_t i = 0; i < count; ++i) {
            h ^= static_cast<uint64_t>(nodes[i].fn_id);
            h *= 1099511628211ULL;
            h ^= static_cast<uint64_t>(nodes[i].dtype) | (static_cast<uint64_t>(nodes[i].is_fused_epilogue) << 8);
            h *= 1099511628211ULL;
            h ^= static_cast<uint64_t>(nodes[i].op_category) << 16;
            h *= 1099511628211ULL;
        }
        return h == 0 ? 1ULL : h;
    }

    RJ_API void begin();
    RJ_API void commit();
    RJ_API void cancel();
    RJ_API bool is_active() const { return in_transaction.load(std::memory_order_acquire); }
    RJ_API void record_op(uint32_t fn_id, const void* in_ptr, void* out_ptr, size_t byte_size, uint8_t dtype);
    RJ_API void record_kernel_op(uint32_t fn_id,
                                    const HS_hotspot_tensor_t* inputs, int n_in,
                                    HS_hotspot_tensor_t* outputs, int n_out);
    RJ_API void record_gc_fusion(void* cell_ptr, uint32_t prev_fn, size_t occupant_size,
                                    size_t aligned_size, void* fwd_addr, uint64_t sweep_no);
    RJ_API void record_object_entangle(void* obj_a, void* obj_b, uint64_t tag,
                                          hs_object_entangle_handle_t* handle = nullptr);
    RJ_API void reset();

    // Parallel Traversal & DAG Analysis
    RJ_API uint32_t build_dag_dependencies();
    RJ_API uint32_t get_max_topological_level() const;
    RJ_API void execute_dag_sequential();
    RJ_API void execute_dag_parallel(uint32_t num_workers);
    RJ_API void execute_node(uint32_t node_idx);
    RJ_API void* get_scratchpad_slice(uint32_t worker_id, size_t needed_bytes);

    // Trace Cache & Superkernel Lookup
    RJ_API hs_entangle_trace_cache_entry_t* lookup_trace(uint64_t trace_hash);
};

extern hs_entangle_execution_engine_t g_hs_entangle_engine;

/* ========================================================================= */
/* 6. RAII SCOPES (RedjetEntangleScope & RedjetObjectEntangleScope)          */
/* ========================================================================= */

class RedjetEntangleScope {
public:
    explicit RedjetEntangleScope(hs_entangle_execution_engine_t& engine = g_hs_entangle_engine)
        : _engine(engine), _active(true) {
        _engine.begin();
    }
    ~RedjetEntangleScope() {
        if (_active) {
            _engine.commit();
        }
    }
    void cancel() noexcept {
        if (_active) {
            _engine.cancel();
            _active = false;
        }
    }
    void commit() noexcept {
        if (_active) {
            _engine.commit();
            _active = false;
        }
    }
    RedjetEntangleScope(const RedjetEntangleScope&) = delete;
    RedjetEntangleScope& operator=(const RedjetEntangleScope&) = delete;

private:
    hs_entangle_execution_engine_t& _engine;
    bool                            _active{ false };
};

class RedjetObjectEntangleScope {
public:
    RedjetObjectEntangleScope(hs_object_entangle_handle_t& handle, void* target, uint64_t tag, uint32_t epoch = 0)
        : _handle(handle), _target(target), _success(false), _committed(false) {
        _success = _handle.try_entangle(target, tag, epoch);
    }
    ~RedjetObjectEntangleScope() {
        if (_success && !_committed) {
            _handle.commit_entanglement(_target);
        }
    }
    bool is_locked() const noexcept { return _success; }
    void abort() noexcept {
        if (_success && !_committed) {
            _handle.break_entanglement();
            _committed = true;
        }
    }
    bool commit() noexcept {
        if (_success && !_committed) {
            bool ok = _handle.commit_entanglement(_target);
            _committed = true;
            return ok;
        }
        return false;
    }
    RedjetObjectEntangleScope(const RedjetObjectEntangleScope&) = delete;
    RedjetObjectEntangleScope& operator=(const RedjetObjectEntangleScope&) = delete;

private:
    hs_object_entangle_handle_t& _handle;
    void*                        _target;
    bool                         _success;
    bool                         _committed;
};

/* ========================================================================= */
/* 7. C-API EXPORTS FOR HOTSPOT JVM RUNTIME & REDJET GC                      */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void     HS_hotspot_entangle_begin(void);
RJ_API void     HS_hotspot_entangle_commit(void);
RJ_API void     HS_hotspot_entangle_cancel(void);
RJ_API void     HS_hotspot_entangle_reset(void);
RJ_API int      HS_hotspot_entangle_is_active(void);
RJ_API void     HS_hotspot_entangle_record_op(uint32_t fn_id,
                                                     const HS_hotspot_tensor_t* inputs, int n_in,
                                                     HS_hotspot_tensor_t* outputs, int n_out);
RJ_API void     HS_hotspot_entangle_record_gc_fusion(void* cell_ptr, uint32_t prev_fn,
                                                            size_t occupant_size, size_t aligned_size,
                                                            void* fwd_addr, uint64_t sweep_no);
RJ_API void     HS_hotspot_entangle_record_object_entangle(void* obj_a, void* obj_b, uint64_t tag);
RJ_API void     HS_hotspot_entangle_execute_parallel(uint32_t num_workers);
RJ_API void     HS_hotspot_entangle_execute_sequential(void);
RJ_API uint32_t HS_hotspot_entangle_get_fused_count(void);
RJ_API uint32_t HS_hotspot_entangle_get_dag_depth(void);
RJ_API uint32_t HS_hotspot_entangle_get_warmup_hits(void);
RJ_API uint32_t HS_hotspot_entangle_get_superkernel_count(void);
RJ_API void     HS_hotspot_entangle_set_warmup_threshold(uint32_t threshold);
RJ_API uint32_t HS_hotspot_entangle_get_warmup_threshold(void);
RJ_API uint32_t HS_hotspot_entangle_get_max_level(void);
RJ_API uint32_t HS_hotspot_entangle_get_parallel_exec_count(void);
RJ_API uint32_t HS_hotspot_entangle_get_gc_fusion_count(void);
RJ_API uint32_t HS_hotspot_entangle_get_object_entangle_count(void);

#ifdef __cplusplus
}
#endif

using RedjetEntangle = hs_entangle_execution_engine_t;

#endif // SHARE_GC_REDJET_REDJETENTANGLE_HPP
