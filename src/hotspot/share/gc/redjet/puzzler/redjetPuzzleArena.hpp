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

#ifndef SHARE_GC_REDJET_REDJETPUZZLEARENA_HPP
#define SHARE_GC_REDJET_REDJETPUZZLEARENA_HPP

#include <cstdint>
#include <cstddef>
#include <cfloat>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#if defined(_MSC_VER)
#include <intrin.h>
#include <malloc.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/simd/redjetALU.hpp"
#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "mirc/mirc.hpp"

class VAC_Table_Snapshot;

#ifndef SACFOR_F32
#define SACFOR_F32 HS_DTYPE_F32
#endif
#ifndef SACFOR_F64
#define SACFOR_F64 HS_DTYPE_F64
#endif
#ifndef SACFOR_I64
#define SACFOR_I64 HS_DTYPE_I64
#endif
#ifndef SACFOR_I32
#define SACFOR_I32 HS_DTYPE_I32
#endif

static inline void* hs_puzzle_aligned_alloc(size_t sz, size_t align = 64) noexcept {
#if defined(_MSC_VER) || defined(__MINGW32__)
    return _aligned_malloc(sz, align);
#elif defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || (defined(__cplusplus) && __cplusplus >= 201703L)
    size_t padded = (sz + align - 1) & ~(align - 1);
    return std::aligned_alloc(align, padded > 0 ? padded : align);
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, sz) != 0) return nullptr;
    return ptr;
#else
    return malloc(sz);
#endif
}

static inline void hs_puzzle_aligned_free(void* ptr) noexcept {
#if defined(_MSC_VER) || defined(__MINGW32__)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

#ifdef STRICT
#pragma push_macro("STRICT")
#undef STRICT
#define HS_RESTORE_WIN32_STRICT_MACRO 1
#endif

/* ========================================================================= */
/* 1. DETERMINISM TIERS & EXECUTION MODES                                    */
/* ========================================================================= */

/**
 * \brief Two-tier determinism model for Yapbozcu / RedjetPuzzleArena.
 *
 * - NUMERIC_IEEE_FP: Float/double/mathematical operations.
 *   If user specifies no target, default is Strict IEEE 754 (machine epsilon:
 *   FLT_EPSILON for float32, DBL_EPSILON for float64).
 *   Error guard: max_abs_err <= epsilon AND MSE <= epsilon^2.
 *   Finite guard: strictly no NaN or Inf produced.
 *
 * - EXACT_ALGEBRAIC: String, memory copy, hash, bitmask, and GC bitmap/card-table scans.
 *   Tolerance is strictly ZERO (0-bit deviation, exact bitwise invariance, memcmp == 0).
 */
enum class hs_puzzler_determinism_tier_e : uint8_t {
    NUMERIC_IEEE_FP = 0,
    EXACT_ALGEBRAIC = 1
};

#ifndef HS_PUZZLE_EXEC_MODE_T_DEFINED
#define HS_PUZZLE_EXEC_MODE_T_DEFINED
typedef enum HS_hotspot_puzzle_exec_mode_t {
    HS_PUZZLE_EXEC_SEQUENTIAL = 0,
    HS_PUZZLE_EXEC_PARALLEL   = 1,
    HS_PUZZLE_EXEC_ADAPTIVE   = 2,
    HS_PUZZLE_EXEC_STRICT     = 3
} HS_hotspot_puzzle_exec_mode_t;
typedef HS_hotspot_puzzle_exec_mode_t hsPuzzleExecMode;
#endif

enum class hs_puzzle_execution_mode_e : uint8_t {
    SEQUENTIAL = static_cast<uint8_t>(HS_PUZZLE_EXEC_SEQUENTIAL),
    PARALLEL   = static_cast<uint8_t>(HS_PUZZLE_EXEC_PARALLEL),
    ADAPTIVE   = static_cast<uint8_t>(HS_PUZZLE_EXEC_ADAPTIVE),
    STRICT     = static_cast<uint8_t>(HS_PUZZLE_EXEC_STRICT)
};

#ifdef HS_RESTORE_WIN32_STRICT_MACRO
#pragma pop_macro("STRICT")
#undef HS_RESTORE_WIN32_STRICT_MACRO
#endif

enum class hs_sample_source_e : uint8_t {
    RUNWAY_MOTHERLINE = 0,
    TLAB_MUTATOR      = 1,
    SYNTHETIC_VECTOR  = 2,
    GC_BITMAP         = 3,
    GC_FALLINE        = 4
};

enum class hs_puzzler_bench_id_e : uint32_t {
    BENCH_MEMZERO        = 1,
    BENCH_MEMCPY         = 2,
    BENCH_BITMAP_SCAN    = 3,
    BENCH_VECTOR_FMA_F32 = 4,
    BENCH_MATRIX_DOT_F64 = 5
};

/* ========================================================================= */
/* 2. CANDIDATE & TELEMETRY MANIFEST TYPES                                   */
/* ========================================================================= */

#ifndef HS_ISA_CANDIDATE_T_DEFINED
#define HS_ISA_CANDIDATE_T_DEFINED
typedef struct HS_hotspot_isa_candidate_t {
    sacculus_kernel_fn_t fn;
    const char*          isa_tag;
    const char*          variant_name;
} HS_hotspot_isa_candidate_t;
typedef HS_hotspot_isa_candidate_t hsIsaCandidate;
#endif

struct hs_puzzle_sample_t {
    uint8_t            dtype{ SACFOR_F32 };
    size_t             byte_size{ 0 };
    int32_t            shape[8]{ 1, 0, 0, 0, 0, 0, 0, 0 };
    int32_t            ndim{ 1 };
    uint8_t*           data{ nullptr };
    hs_sample_source_e source{ hs_sample_source_e::RUNWAY_MOTHERLINE };
    uint64_t           sample_timestamp_tsc{ 0 };
    bool               is_from_tlab{ false };

    hs_puzzle_sample_t() {
        data = (uint8_t*)hs_puzzle_aligned_alloc(128 * 1024, 64);
        if (data) memset(data, 0, 128 * 1024);
    }
    ~hs_puzzle_sample_t() {
        if (data) hs_puzzle_aligned_free(data);
    }
    hs_puzzle_sample_t(const hs_puzzle_sample_t& o) {
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        source = o.source; sample_timestamp_tsc = o.sample_timestamp_tsc;
        is_from_tlab = o.is_from_tlab;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        data = (uint8_t*)hs_puzzle_aligned_alloc(128 * 1024, 64);
        if (o.data && data) memcpy(data, o.data, 128 * 1024);
    }
    hs_puzzle_sample_t(hs_puzzle_sample_t&& o) noexcept {
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        source = o.source; sample_timestamp_tsc = o.sample_timestamp_tsc;
        is_from_tlab = o.is_from_tlab;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        data = o.data;
        o.data = nullptr;
    }
    hs_puzzle_sample_t& operator=(const hs_puzzle_sample_t& o) {
        if (this == &o) return *this;
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        source = o.source; sample_timestamp_tsc = o.sample_timestamp_tsc;
        is_from_tlab = o.is_from_tlab;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        if (!data) data = (uint8_t*)hs_puzzle_aligned_alloc(128 * 1024, 64);
        if (o.data && data) memcpy(data, o.data, 128 * 1024);
        return *this;
    }
    hs_puzzle_sample_t& operator=(hs_puzzle_sample_t&& o) noexcept {
        if (this == &o) return *this;
        if (data) hs_puzzle_aligned_free(data);
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        source = o.source; sample_timestamp_tsc = o.sample_timestamp_tsc;
        is_from_tlab = o.is_from_tlab;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        data = o.data;
        o.data = nullptr;
        return *this;
    }

    static hs_puzzle_sample_t create_from_runway(const void* src, size_t sz, uint8_t dt = SACFOR_F32) {
        hs_puzzle_sample_t s;
        s.dtype = dt;
        s.source = hs_sample_source_e::RUNWAY_MOTHERLINE;
        s.is_from_tlab = false;
        s.sample_timestamp_tsc = sacculus_rdtsc();
        size_t copy_sz = sz > (128 * 1024) ? (128 * 1024) : sz;
        if (src && s.data) {
            memcpy(s.data, src, copy_sz);
            s.byte_size = copy_sz;
            s.ndim = 2;
            size_t elem_sz = (dt == SACFOR_F64 || dt == SACFOR_I64) ? 8 : 4;
            s.shape[0] = 1;
            s.shape[1] = static_cast<int32_t>(copy_sz / (elem_sz > 0 ? elem_sz : 1));
        }
        return s;
    }

    static hs_puzzle_sample_t create_from_tlab(const void* src, size_t sz, uint8_t dt = SACFOR_F32) {
        hs_puzzle_sample_t s;
        s.dtype = dt;
        s.source = hs_sample_source_e::TLAB_MUTATOR;
        s.is_from_tlab = true;
        s.sample_timestamp_tsc = sacculus_rdtsc();
        size_t copy_sz = sz > (128 * 1024) ? (128 * 1024) : sz;
        if (src && s.data) {
            memcpy(s.data, src, copy_sz);
            s.byte_size = copy_sz;
            s.ndim = 2;
            size_t elem_sz = (dt == SACFOR_F64 || dt == SACFOR_I64) ? 8 : 4;
            s.shape[0] = 1;
            s.shape[1] = static_cast<int32_t>(copy_sz / (elem_sz > 0 ? elem_sz : 1));
        }
        return s;
    }
};

struct hs_puzzle_batch_job_t {
    uint32_t                        fn_id{ 0 };
    std::vector<hs_puzzle_sample_t> samples;
    HS_hotspot_tensor_t         target_precision_tensor{};
    bool                            has_target_precision{ false };
    float                           target_epsilon{ 1e-4f };
};

struct hs_puzzle_experiment_t {
    uint32_t                fn_id{ 0 };
    char                    name[32]{ 0 };
    sacculus_kernel_fn_t    candidate{ nullptr };
    char                    target_isa[16]{ 0 };
    float                   target_epsilon{ 1e-4f };
    HS_hotspot_tensor_t target_tensor{};
    bool                    has_target_tensor{ false };
};

struct hs_candidate_eval_t {
    int                         idx{ -1 };
    sacculus_kernel_fn_t        fn{ nullptr };
    char                        variant_name[32]{ 0 };
    char                        isa_tag[16]{ 0 };
    bool                        executed_ok{ false };
    uint32_t                    fault_code{ 0 };
    uint64_t                    cycles{ UINT64_MAX };
    float                       max_abs_err{ 1e9f };
    float                       mse{ 1e9f };
    bool                        precision_passed{ false };
    bool                        speed_passed{ false };
    double                      score{ -1.0 };
    bool                        is_hybrid{ false };
    double                      l1_l2_cache_miss_penalty{ 0.0 };
    double                      ipc_score{ 0.0 };
    double                      downclock_penalty{ 1.0 };
    uint64_t                    instructions_executed{ 0 };
    uint64_t                    cache_misses{ 0 };
    HS_hotspot_pmu_stat_t   pmu_stats{};
};

struct hs_hybrid_compat_result_t {
    bool   compatible{ false };
    double mean_expected{ 0.0 };
    double mean_hybrid{ 0.0 };
    double dynamic_bound{ 1.0 };
    double observed_ratio{ 1.0 };
    char   reason[64]{ 0 };
    double avx512_downclock_penalty{ 0.0 };
    double p_core_ipc{ 0.0 };
    double e_core_ipc{ 0.0 };
    double p_core_cycles{ 0.0 };
    double e_core_cycles{ 0.0 };
    bool   avx512_throttle_detected{ false };
    double asymmetric_ratio{ 1.0 };
};

struct hs_score_eval_t {
    bool   qualified{ false };
    double score{ -1.0 };
    double speedup_ratio{ 0.0 };
    double precision_headroom{ 0.0 };
    float  max_err{ 0.0f };
    float  mse{ 0.0f };
    double pmu_ipc{ 0.0 };
    double cache_penalty{ 0.0 };
    double downclock_factor{ 1.0 };
};

struct hs_puzzler_bench_result_t {
    uint32_t                       bench_id{ 0 };
    char                           bench_name[32]{ 0 };
    char                           winning_isa[16]{ 0 };
    uint64_t                       baseline_cycles{ 0 };
    uint64_t                       winning_cycles{ 0 };
    double                         speedup{ 1.0 };
    double                         ipc{ 1.0 };
    float                          max_abs_err{ 0.0f };
    float                          mse{ 0.0f };
    bool                           verified_ok{ false };
    hs_puzzler_determinism_tier_e tier{ hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC };
};

struct hs_puzzle_arena_telemetry_t {
    uint64_t total_experiments{ 0 };
    uint64_t total_candidates_evaluated{ 0 };
    uint64_t total_hotswaps_performed{ 0 };
    uint64_t total_precision_failures{ 0 };
    uint64_t total_speed_failures{ 0 };
    uint64_t total_fault_isolations{ 0 };
    uint64_t total_downclock_detections{ 0 };
    uint64_t total_pausepoint_gates_armed{ 0 };
    uint64_t last_winner_cycles{ 0 };
    char     last_winner_name[32]{ 0 };
};

/* ========================================================================= */
/* 3. DETERMINISM HELPERS                                                    */
/* ========================================================================= */

RJ_API hs_puzzler_determinism_tier_e hs_puzzler_classify_tier(uint8_t dtype);
RJ_API float                         hs_puzzler_default_epsilon_for_dtype(uint8_t dtype);
RJ_API bool                          hs_puzzler_validate_target_precision(uint8_t dtype, float& inout_target_epsilon);
RJ_API void                          hs_accumulate_ieee754_error_f32(const float* cand_ptr, const float* ref_ptr, size_t n_elems,
                                                                        float& inout_max_err, float& inout_sum_sq, bool& inout_finite_ok) noexcept;
RJ_API void                          hs_accumulate_ieee754_error_f64(const double* cand_ptr, const double* ref_ptr, size_t n_elems,
                                                                        float& inout_max_err, float& inout_sum_sq, bool& inout_finite_ok) noexcept;

/* ========================================================================= */
/* 4. REDJET PUZZLE ARENA (sacculus_puzzle_arena_t)                          */
/* ========================================================================= */

class RedjetPausepointGateGuard {
private:
    bool _armed{ false };
public:
    RedjetPausepointGateGuard() {
        HS_hotspot_set_pausepoint_gate(1);
        _armed = true;
    }
    ~RedjetPausepointGateGuard() {
        disarm();
    }
    void disarm() noexcept {
        if (_armed) {
            HS_hotspot_set_pausepoint_gate(0);
            _armed = false;
        }
    }
    RedjetPausepointGateGuard(const RedjetPausepointGateGuard&) = delete;
    RedjetPausepointGateGuard& operator=(const RedjetPausepointGateGuard&) = delete;
};

class RedjetPuzzleArena {
private:
    sacculus_dispatch_table_t* _dispatch{ nullptr };
    sacculus_runway_t*         _runway{ nullptr };
    sacculus_hotlog_t*         _hotlog{ nullptr };
    sacculus_coldlog_t*        _coldlog{ nullptr };

    std::thread                _thread;
    std::atomic<bool>          _running{ false };
    std::atomic<uint8_t>       _execution_mode{ static_cast<uint8_t>(HS_PUZZLE_EXEC_ADAPTIVE) };
    std::atomic<size_t>        _stream_sample_count{ 5 };

    std::mutex                          _queue_lock;
    std::vector<hs_puzzle_experiment_t> _pending_experiments;
    std::vector<hs_puzzle_batch_job_t>  _pending_batch_jobs;
    std::vector<hs_candidate_eval_t>    _eval_history;

    static constexpr size_t STAGNANT_BUF_CAP = 128 * 1024;
    uint8_t* puzzle_heap_base{ nullptr };
    size_t   puzzle_heap_capacity{ 0 };
    std::atomic<size_t> puzzle_heap_used{ 0 };

    uint8_t* stagnant_in_payload{ nullptr };
    uint8_t* candidate_out_payload{ nullptr };
    uint8_t* current_best_out_payload{ nullptr };
    bool     _owns_payload_fallback{ false };

    hs_cpu_isa_inventory_t     cached_isa_inventory;
    std::vector<std::string>   active_simd_pool;

    // Telemetry atomic accumulators
    std::atomic<uint64_t> _telemetry_experiments{ 0 };
    std::atomic<uint64_t> _telemetry_candidates_eval{ 0 };
    std::atomic<uint64_t> _telemetry_hotswaps{ 0 };
    std::atomic<uint64_t> _telemetry_prec_fails{ 0 };
    std::atomic<uint64_t> _telemetry_speed_fails{ 0 };
    std::atomic<uint64_t> _telemetry_fault_isolations{ 0 };
    std::atomic<uint64_t> _telemetry_downclock_detections{ 0 };
    std::atomic<uint64_t> _telemetry_pausepoint_gates{ 0 };
    std::atomic<uint64_t> _telemetry_last_winner_cycles{ 0 };
    char                  _telemetry_last_winner_name[32]{ 0 };
    mutable std::mutex    _telemetry_lock;

public:
    RJ_API RedjetPuzzleArena();
    RJ_API ~RedjetPuzzleArena();

    RJ_API void ensure_payload_buffers();
    RJ_API static void record_tlab_snapshot(const void* src, size_t sz, uint8_t dt = SACFOR_F32);
    RJ_API static bool get_tlab_snapshot(hs_puzzle_sample_t& out_smp);

    RJ_API void init(sacculus_dispatch_table_t* dt = nullptr,
                        sacculus_runway_t* rw = nullptr,
                        sacculus_hotlog_t* hl = nullptr,
                        sacculus_coldlog_t* cl = nullptr,
                        size_t arena_bytes = 16ULL * 1024 * 1024);
    RJ_API void destroy();

    RJ_API void start();
    RJ_API void stop();

    bool is_running() const { return _running.load(std::memory_order_relaxed); }
    RJ_API void refresh_simd_pool();
    RJ_API std::vector<std::string> pool_simd_capabilities();

    RJ_API void set_mode(HS_hotspot_puzzle_exec_mode_t mode);
    RJ_API HS_hotspot_puzzle_exec_mode_t get_mode() const;

    RJ_API void set_sample_count(size_t count);
    RJ_API size_t get_sample_count() const;

    size_t heap_capacity() const noexcept { return puzzle_heap_capacity; }
    size_t heap_used() const noexcept { return puzzle_heap_used.load(std::memory_order_relaxed); }

    RJ_API void identify_stream_types(std::vector<uint8_t>& detected_dtypes);
    RJ_API bool sample_runway_stream(size_t sample_iterations, std::vector<hs_puzzle_sample_t>& aggregated_samples);
    RJ_API bool sample_tlab_stream(size_t sample_iterations, std::vector<hs_puzzle_sample_t>& aggregated_samples);

    RJ_API void submit_batch(uint32_t fn_id,
                                const HS_hotspot_tensor_t* batch_samples,
                                int sample_count,
                                const HS_hotspot_tensor_t* target_prec,
                                float target_epsilon);

    RJ_API bool acquire_evaluation_samples(uint32_t fn_id,
                                              std::vector<hs_puzzle_sample_t>& samples,
                                              HS_hotspot_tensor_t& target_prec,
                                              bool& has_target_prec,
                                              float& target_epsilon,
                                              hs_puzzler_determinism_tier_e& out_tier);

    RJ_API hs_hybrid_compat_result_t evaluate_hybrid_compatibility(sacculus_kernel_fn_t fn_a,
                                                                    sacculus_kernel_fn_t fn_b,
                                                                    const HS_hotspot_tensor_t* in_t,
                                                                    HS_hotspot_tensor_t* out_t);

    RJ_API hs_score_eval_t evaluate_score(uint64_t cand_cycles,
                                             uint64_t cur_cycles,
                                             float max_abs_err,
                                             float mse,
                                             float target_epsilon,
                                             const HS_hotspot_pmu_stat_t* pmu = nullptr);

    RJ_API void redirect_stream(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t new_cycles, const char* name, double score);

    RJ_API void submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name = nullptr,
                          const char* target_isa = "AVX2", float target_epsilon = 1e-4f,
                          const HS_hotspot_tensor_t* target_tensor = nullptr);

    RJ_API void run_puzzle_suite_parallel(uint32_t fn_id, const HS_hotspot_isa_candidate_t* candidates, int n_candidates,
                                            float target_epsilon, const HS_hotspot_tensor_t* target_tensor);

    RJ_API void submit_suite(uint32_t fn_id, const HS_hotspot_isa_candidate_t* candidates, int n_candidates,
                                float target_epsilon = 1e-4f, const HS_hotspot_tensor_t* target_tensor = nullptr);

    RJ_API void run_puzzle_experiment_with_samples(const hs_puzzle_experiment_t& exp,
                                                     const std::vector<hs_puzzle_sample_t>& samples,
                                                     const HS_hotspot_tensor_t& target_prec,
                                                     bool has_target_prec,
                                                     float target_eps,
                                                     hs_puzzler_determinism_tier_e tier);

    RJ_API void run_puzzle_experiment(const hs_puzzle_experiment_t& exp);
    RJ_API void run_puzzle_batch_job(const hs_puzzle_batch_job_t& job);

    RJ_API void evaluate_candidate(hs_candidate_eval_t& candidate);
    RJ_API double score_variant(uint64_t baseline_cycles, uint64_t candidate_cycles, float max_err);
    RJ_API size_t history_count() const;
    RJ_API std::vector<hs_candidate_eval_t> get_candidate_history() const;

    // Self-tuning micro-benchmark suite
    RJ_API hs_puzzler_bench_result_t run_self_tuning_benchmark(uint32_t bench_id);
    RJ_API void run_all_self_tuning_benchmarks();

    // Telemetry reporting & inspection
    RJ_API hs_puzzle_arena_telemetry_t get_telemetry_snapshot() const;
    RJ_API void reset_telemetry();
    RJ_API void print_telemetry_report(FILE* out = stdout) const;

    // Phase 4: Puzzler Çift Girdili Yarışma Motoru (VAC_Table + MIRC_Lvl1_Table)
    RJ_API bool submit_duel(const VAC_Table_Snapshot* vac_tab,
                               MIRC_Lvl1_Table* lvl1_tab,
                               mirc_hotswap_delta_t* out_delta = nullptr);

    RJ_API bool apply_hotswap_delta_2points(const mirc_hotswap_delta_t& delta,
                                               HS_hotspot_live_header_t* target_obj = nullptr);
};

using sacculus_puzzle_arena_t = RedjetPuzzleArena;

/* ========================================================================= */
/* 5. CANONICAL C APIS                                                       */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

RJ_API RedjetPuzzleArena* hs_get_default_puzzle_arena(void);
RJ_API void HS_hotspot_puzzle_submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name,
                                            const char* target_isa, float target_epsilon,
                                            const HS_hotspot_tensor_t* target_tensor);
RJ_API void HS_hotspot_puzzle_submit_batch(uint32_t fn_id, const HS_hotspot_tensor_t* batch_samples,
                                                  int sample_count, const HS_hotspot_tensor_t* target_prec,
                                                  float target_epsilon);
RJ_API void HS_hotspot_puzzle_submit_suite(uint32_t fn_id, const HS_hotspot_isa_candidate_t* candidates,
                                                  int n_candidates, float target_epsilon,
                                                  const HS_hotspot_tensor_t* target_tensor);
RJ_API void HS_hotspot_puzzle_set_mode(HS_hotspot_puzzle_exec_mode_t mode);
RJ_API HS_hotspot_puzzle_exec_mode_t HS_hotspot_puzzle_get_mode(void);
RJ_API void HS_hotspot_puzzle_set_sample_count(size_t count);
RJ_API size_t HS_hotspot_puzzle_get_sample_count(void);

RJ_API int  HS_hotspot_puzzle_sample_tlab(size_t iterations);
RJ_API void HS_hotspot_puzzle_run_benchmark(uint32_t bench_id, hs_puzzler_bench_result_t* out_result);
RJ_API void HS_hotspot_puzzle_get_telemetry(hs_puzzle_arena_telemetry_t* out_telemetry);
RJ_API void HS_hotspot_puzzle_reset_telemetry(void);
RJ_API void HS_hotspot_puzzle_print_telemetry(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_REDJETPUZZLEARENA_HPP
