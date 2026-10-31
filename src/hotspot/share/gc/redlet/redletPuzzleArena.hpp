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

#ifndef SHARE_GC_REDLET_REDLETPUZZLEARENA_HPP
#define SHARE_GC_REDLET_REDLETPUZZLEARENA_HPP

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

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletALU.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include "gc/redlet/redletRunway.hpp"

#ifndef SACFOR_F32
#define SACFOR_F32 SF_DTYPE_F32
#endif
#ifndef SACFOR_F64
#define SACFOR_F64 SF_DTYPE_F64
#endif
#ifndef SACFOR_I64
#define SACFOR_I64 SF_DTYPE_I64
#endif
#ifndef SACFOR_I32
#define SACFOR_I32 SF_DTYPE_I32
#endif

#ifdef STRICT
#pragma push_macro("STRICT")
#undef STRICT
#define SF_RESTORE_WIN32_STRICT_MACRO 1
#endif

/* ========================================================================= */
/* 1. DETERMINISM TIERS & EXECUTION MODES                                    */
/* ========================================================================= */

/**
 * \brief Two-tier determinism model for Yapbozcu / RedletPuzzleArena.
 *
 * - NUMERIC_IEEE_FP: Float/double/mathematical operations.
 *   If user specifies no target, default is Strict IEEE 754 (machine epsilon:
 *   FLT_EPSILON for float32, DBL_EPSILON for float64).
 *
 * - EXACT_ALGEBRAIC: String, memory copy, hash, bitmask, and GC bitmap/card-table scans.
 *   Tolerance is strictly ZERO (0-bit deviation, exact bitwise invariance, memcmp == 0).
 */
enum class sf_puzzler_determinism_tier_e : uint8_t {
    NUMERIC_IEEE_FP = 0,
    EXACT_ALGEBRAIC = 1
};

#ifndef SF_PUZZLE_EXEC_MODE_T_DEFINED
#define SF_PUZZLE_EXEC_MODE_T_DEFINED
typedef enum SF_custominiVm_puzzle_exec_mode_t {
    SF_PUZZLE_EXEC_SEQUENTIAL = 0,
    SF_PUZZLE_EXEC_PARALLEL   = 1,
    SF_PUZZLE_EXEC_ADAPTIVE   = 2,
    SF_PUZZLE_EXEC_STRICT     = 3
} SF_custominiVm_puzzle_exec_mode_t;
typedef SF_custominiVm_puzzle_exec_mode_t sfPuzzleExecMode;
#endif

enum class sf_puzzle_execution_mode_e : uint8_t {
    SEQUENTIAL = static_cast<uint8_t>(SF_PUZZLE_EXEC_SEQUENTIAL),
    PARALLEL   = static_cast<uint8_t>(SF_PUZZLE_EXEC_PARALLEL),
    ADAPTIVE   = static_cast<uint8_t>(SF_PUZZLE_EXEC_ADAPTIVE),
    STRICT     = static_cast<uint8_t>(SF_PUZZLE_EXEC_STRICT)
};

#ifdef SF_RESTORE_WIN32_STRICT_MACRO
#pragma pop_macro("STRICT")
#undef SF_RESTORE_WIN32_STRICT_MACRO
#endif

/* ========================================================================= */
/* 2. CANDIDATE & TELEMETRY MANIFEST TYPES                                   */
/* ========================================================================= */

#ifndef SF_ISA_CANDIDATE_T_DEFINED
#define SF_ISA_CANDIDATE_T_DEFINED
typedef struct SF_custominiVm_isa_candidate_t {
    sacculus_kernel_fn_t fn;
    const char*          isa_tag;
    const char*          variant_name;
} SF_custominiVm_isa_candidate_t;
typedef SF_custominiVm_isa_candidate_t sfIsaCandidate;
#endif

struct sf_puzzle_sample_t {
    uint8_t  dtype{ SACFOR_F32 };
    size_t   byte_size{ 0 };
    int32_t  shape[8]{ 1, 0, 0, 0, 0, 0, 0, 0 };
    int32_t  ndim{ 1 };
    uint8_t* data{ nullptr };

    sf_puzzle_sample_t() {
        data = (uint8_t*)_mm_malloc(128 * 1024, 64);
        if (data) memset(data, 0, 128 * 1024);
    }
    ~sf_puzzle_sample_t() {
        if (data) _mm_free(data);
    }
    sf_puzzle_sample_t(const sf_puzzle_sample_t& o) {
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        data = (uint8_t*)_mm_malloc(128 * 1024, 64);
        if (o.data && data) memcpy(data, o.data, 128 * 1024);
    }
    sf_puzzle_sample_t(sf_puzzle_sample_t&& o) noexcept {
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        data = o.data;
        o.data = nullptr;
    }
    sf_puzzle_sample_t& operator=(const sf_puzzle_sample_t& o) {
        if (this == &o) return *this;
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        if (!data) data = (uint8_t*)_mm_malloc(128 * 1024, 64);
        if (o.data && data) memcpy(data, o.data, 128 * 1024);
        return *this;
    }
    sf_puzzle_sample_t& operator=(sf_puzzle_sample_t&& o) noexcept {
        if (this == &o) return *this;
        if (data) _mm_free(data);
        dtype = o.dtype; byte_size = o.byte_size; ndim = o.ndim;
        for (int i = 0; i < 8; ++i) shape[i] = o.shape[i];
        data = o.data;
        o.data = nullptr;
        return *this;
    }
};

struct sf_puzzle_batch_job_t {
    uint32_t                        fn_id{ 0 };
    std::vector<sf_puzzle_sample_t> samples;
    SF_custominiVm_tensor_t         target_precision_tensor{};
    bool                            has_target_precision{ false };
    float                           target_epsilon{ 1e-4f };
};

struct sf_puzzle_experiment_t {
    uint32_t              fn_id{ 0 };
    char                  name[32]{ 0 };
    sacculus_kernel_fn_t  candidate{ nullptr };
    char                  target_isa[16]{ 0 };
    float                 target_epsilon{ 1e-4f };
    SF_custominiVm_tensor_t target_tensor{};
    bool                  has_target_tensor{ false };
};

struct sf_candidate_eval_t {
    int                  idx{ -1 };
    sacculus_kernel_fn_t fn{ nullptr };
    char                 variant_name[32]{ 0 };
    char                 isa_tag[16]{ 0 };
    bool                 executed_ok{ false };
    uint32_t             fault_code{ 0 };
    uint64_t             cycles{ UINT64_MAX };
    float                max_abs_err{ 1e9f };
    float                mse{ 1e9f };
    bool                 precision_passed{ false };
    bool                 speed_passed{ false };
    double               score{ -1.0 };
    bool                 is_hybrid{ false };
};

struct sf_hybrid_compat_result_t {
    bool   compatible{ false };
    double mean_expected{ 0.0 };
    double mean_hybrid{ 0.0 };
    double dynamic_bound{ 1.0 };
    double observed_ratio{ 1.0 };
    char   reason[64]{ 0 };
};

struct sf_score_eval_t {
    bool   qualified{ false };
    double score{ -1.0 };
    double speedup_ratio{ 0.0 };
    double precision_headroom{ 0.0 };
    float  max_err{ 0.0f };
    float  mse{ 0.0f };
};

/* ========================================================================= */
/* 3. DETERMINISM HELPERS                                                    */
/* ========================================================================= */

MSIMD_API sf_puzzler_determinism_tier_e sf_puzzler_classify_tier(uint8_t dtype);
MSIMD_API float                         sf_puzzler_default_epsilon_for_dtype(uint8_t dtype);
MSIMD_API bool                          sf_puzzler_validate_target_precision(uint8_t dtype, float& inout_target_epsilon);
MSIMD_API void                          sf_accumulate_ieee754_error_f32(const float* cand_ptr, const float* ref_ptr, size_t n_elems,
                                                                        float& inout_max_err, float& inout_sum_sq, bool& inout_finite_ok) noexcept;
MSIMD_API void                          sf_accumulate_ieee754_error_f64(const double* cand_ptr, const double* ref_ptr, size_t n_elems,
                                                                        float& inout_max_err, float& inout_sum_sq, bool& inout_finite_ok) noexcept;

/* ========================================================================= */
/* 4. REDLET PUZZLE ARENA (sacculus_puzzle_arena_t)                          */
/* ========================================================================= */

class RedletPuzzleArena {
private:
    sacculus_dispatch_table_t* _dispatch{ nullptr };
    sacculus_runway_t*         _runway{ nullptr };
    sacculus_hotlog_t*         _hotlog{ nullptr };
    sacculus_coldlog_t*        _coldlog{ nullptr };

    std::thread                _thread;
    std::atomic<bool>          _running{ false };
    std::atomic<uint8_t>       _execution_mode{ static_cast<uint8_t>(SF_PUZZLE_EXEC_ADAPTIVE) };
    std::atomic<size_t>        _stream_sample_count{ 5 };

    std::mutex                          _queue_lock;
    std::vector<sf_puzzle_experiment_t> _pending_experiments;
    std::vector<sf_puzzle_batch_job_t>  _pending_batch_jobs;
    std::vector<sf_candidate_eval_t>    _eval_history;

    static constexpr size_t STAGNANT_BUF_CAP = 128 * 1024;
    uint8_t* puzzle_heap_base{ nullptr };
    size_t   puzzle_heap_capacity{ 0 };
    std::atomic<size_t> puzzle_heap_used{ 0 };

    uint8_t* stagnant_in_payload{ nullptr };
    uint8_t* candidate_out_payload{ nullptr };
    uint8_t* current_best_out_payload{ nullptr };

    sf_cpu_isa_inventory_t     cached_isa_inventory;
    std::vector<std::string>   active_simd_pool;

public:
    MSIMD_API RedletPuzzleArena();
    MSIMD_API ~RedletPuzzleArena();

    MSIMD_API void init(sacculus_dispatch_table_t* dt = nullptr,
                        sacculus_runway_t* rw = nullptr,
                        sacculus_hotlog_t* hl = nullptr,
                        sacculus_coldlog_t* cl = nullptr,
                        size_t arena_bytes = 16ULL * 1024 * 1024);
    MSIMD_API void destroy();

    MSIMD_API void start();
    MSIMD_API void stop();

    bool is_running() const { return _running.load(std::memory_order_relaxed); }
    MSIMD_API void refresh_simd_pool();
    MSIMD_API std::vector<std::string> pool_simd_capabilities();

    MSIMD_API void set_mode(SF_custominiVm_puzzle_exec_mode_t mode);
    MSIMD_API SF_custominiVm_puzzle_exec_mode_t get_mode() const;

    MSIMD_API void set_sample_count(size_t count);
    MSIMD_API size_t get_sample_count() const;

    size_t heap_capacity() const noexcept { return puzzle_heap_capacity; }
    size_t heap_used() const noexcept { return puzzle_heap_used.load(std::memory_order_relaxed); }

    MSIMD_API void identify_stream_types(std::vector<uint8_t>& detected_dtypes);
    MSIMD_API bool sample_runway_stream(size_t sample_iterations, std::vector<sf_puzzle_sample_t>& aggregated_samples);

    MSIMD_API void submit_batch(uint32_t fn_id,
                                const SF_custominiVm_tensor_t* batch_samples,
                                int sample_count,
                                const SF_custominiVm_tensor_t* target_prec,
                                float target_epsilon);

    MSIMD_API bool acquire_evaluation_samples(uint32_t fn_id,
                                              std::vector<sf_puzzle_sample_t>& samples,
                                              SF_custominiVm_tensor_t& target_prec,
                                              bool& has_target_prec,
                                              float& target_epsilon,
                                              sf_puzzler_determinism_tier_e& out_tier);

    MSIMD_API sf_hybrid_compat_result_t evaluate_hybrid_compatibility(sacculus_kernel_fn_t fn_a,
                                                                    sacculus_kernel_fn_t fn_b,
                                                                    const SF_custominiVm_tensor_t* in_t,
                                                                    SF_custominiVm_tensor_t* out_t);

    MSIMD_API sf_score_eval_t evaluate_score(uint64_t cand_cycles,
                                             uint64_t cur_cycles,
                                             float max_abs_err,
                                             float mse,
                                             float target_epsilon,
                                             const SF_custominiVm_pmu_stat_t* pmu = nullptr);

    MSIMD_API void redirect_stream(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t new_cycles, const char* name, double score);

    MSIMD_API void submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name = nullptr,
                          const char* target_isa = "AVX2", float target_epsilon = 1e-4f,
                          const SF_custominiVm_tensor_t* target_tensor = nullptr);

    MSIMD_API void run_puzzle_suite_parallel(uint32_t fn_id, const SF_custominiVm_isa_candidate_t* candidates, int n_candidates,
                                            float target_epsilon, const SF_custominiVm_tensor_t* target_tensor);

    MSIMD_API void submit_suite(uint32_t fn_id, const SF_custominiVm_isa_candidate_t* candidates, int n_candidates,
                                float target_epsilon = 1e-4f, const SF_custominiVm_tensor_t* target_tensor = nullptr);

    MSIMD_API void run_puzzle_experiment_with_samples(const sf_puzzle_experiment_t& exp,
                                                     const std::vector<sf_puzzle_sample_t>& samples,
                                                     const SF_custominiVm_tensor_t& target_prec,
                                                     bool has_target_prec,
                                                     float target_eps,
                                                     sf_puzzler_determinism_tier_e tier);

    MSIMD_API void run_puzzle_experiment(const sf_puzzle_experiment_t& exp);
    MSIMD_API void run_puzzle_batch_job(const sf_puzzle_batch_job_t& job);

    MSIMD_API void evaluate_candidate(sf_candidate_eval_t& candidate);
    MSIMD_API double score_variant(uint64_t baseline_cycles, uint64_t candidate_cycles, float max_err);
    MSIMD_API size_t history_count() const;
};

using sacculus_puzzle_arena_t = RedletPuzzleArena;

/* ========================================================================= */
/* 5. CANONICAL C APIS                                                       */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API RedletPuzzleArena* sf_get_default_puzzle_arena(void);
MSIMD_API void SF_custominiVm_puzzle_submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name,
                                            const char* target_isa, float target_epsilon,
                                            const SF_custominiVm_tensor_t* target_tensor);
MSIMD_API void SF_custominiVm_puzzle_submit_batch(uint32_t fn_id, const SF_custominiVm_tensor_t* batch_samples,
                                                  int sample_count, const SF_custominiVm_tensor_t* target_prec,
                                                  float target_epsilon);
MSIMD_API void SF_custominiVm_puzzle_submit_suite(uint32_t fn_id, const SF_custominiVm_isa_candidate_t* candidates,
                                                  int n_candidates, float target_epsilon,
                                                  const SF_custominiVm_tensor_t* target_tensor);
MSIMD_API void SF_custominiVm_puzzle_set_mode(SF_custominiVm_puzzle_exec_mode_t mode);
MSIMD_API SF_custominiVm_puzzle_exec_mode_t SF_custominiVm_puzzle_get_mode(void);
MSIMD_API void SF_custominiVm_puzzle_set_sample_count(size_t count);
MSIMD_API size_t SF_custominiVm_puzzle_get_sample_count(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETPUZZLEARENA_HPP
