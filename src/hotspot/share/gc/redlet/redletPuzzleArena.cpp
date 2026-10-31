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

#include "gc/redlet/redletPuzzleArena.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include "gc/redlet/redletRunway.hpp"
#include "gc/redlet/redletRingLog.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#endif

static RedletPuzzleArena g_default_redlet_puzzle_arena;

/* ========================================================================= */
/* 1. TWO-TIER DETERMINISM CLASSIFICATION & VALIDATION HELPERS               */
/* ========================================================================= */

MSIMD_API sf_puzzler_determinism_tier_e sf_puzzler_classify_tier(uint8_t dtype) {
    if (dtype == SACFOR_F32 || dtype == SACFOR_F64 ||
        dtype == SF_DTYPE_F16 || dtype == SF_DTYPE_F8 ||
        dtype == SF_DTYPE_F128 || dtype == SF_DTYPE_F256) {
        return sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
    }
    return sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC;
}

MSIMD_API float sf_puzzler_default_epsilon_for_dtype(uint8_t dtype) {
    if (dtype == SACFOR_F64 || dtype == SF_DTYPE_F128 || dtype == SF_DTYPE_F256) {
        return static_cast<float>(DBL_EPSILON);
    }
    if (dtype == SACFOR_F32) {
        return FLT_EPSILON;
    }
    if (dtype == SF_DTYPE_F16) {
        return 9.77e-4f;
    }
    if (dtype == SF_DTYPE_F8) {
        return 0.0625f;
    }
    return 0.0f; // EXACT_ALGEBRAIC: strictly 0-bit deviation tolerance
}

MSIMD_API bool sf_puzzler_validate_target_precision(uint8_t dtype, float& inout_target_epsilon) {
    sf_puzzler_determinism_tier_e tier = sf_puzzler_classify_tier(dtype);
    if (tier == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) {
        // Precision Target semantic support will only operate on float/double code.
        // Non-FP code target must be rejected/disallowed or strictly 0.0 tolerance.
        if (inout_target_epsilon > 0.0f) {
            inout_target_epsilon = 0.0f;
            return false; // Rejected: target precision semantics disallowed on non-FP operations
        }
        inout_target_epsilon = 0.0f;
        return true;
    } else {
        // NUMERIC_IEEE_FP: default to strict machine epsilon if unspecified or non-positive
        if (inout_target_epsilon <= 0.0f) {
            inout_target_epsilon = sf_puzzler_default_epsilon_for_dtype(dtype);
        }
        return true;
    }
}

/* ========================================================================= */
/* 2. SAFE FAULT-ISOLATED KERNEL INVOCATION                                  */
/* ========================================================================= */

#if defined(_WIN32)
SF_DISABLE_OPT_BEGIN
static bool sf_safe_kernel_call_impl(sacculus_kernel_fn_t fn,
                                     const SF_custominiVm_tensor_t* in_t, int nin,
                                     SF_custominiVm_tensor_t* out_t, int nout,
                                     void* ctx, uint32_t* fault_code) {
    (void)ctx;
    __try {
        fn(in_t, nin, out_t, nout);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (fault_code) *fault_code = static_cast<uint32_t>(GetExceptionCode());
        return false;
    }
}
static inline bool sf_safe_kernel_call(sacculus_kernel_fn_t fn,
                                       const SF_custominiVm_tensor_t* in_t, int nin,
                                       SF_custominiVm_tensor_t* out_t, int nout,
                                       void* ctx, uint32_t* fault_code = nullptr) {
    if (!fn) return false;
    return sf_safe_kernel_call_impl(fn, in_t, nin, out_t, nout, ctx, fault_code);
}
SF_DISABLE_OPT_END
#else
static thread_local sigjmp_buf       sf_kernel_jmpbuf;
static thread_local volatile sig_atomic_t sf_kernel_in_safe_call = 0;

static void sf_kernel_signal_handler(int sig) {
    if (sf_kernel_in_safe_call) {
        siglongjmp(sf_kernel_jmpbuf, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static std::mutex g_sf_sigaction_mtx;
static uint32_t   g_sf_sigaction_active_calls = 0;
static struct sigaction g_sf_old_ill{}, g_sf_old_segv{}, g_sf_old_bus{};

static bool sf_safe_kernel_call(sacculus_kernel_fn_t fn,
                                const SF_custominiVm_tensor_t* in_t, int nin,
                                SF_custominiVm_tensor_t* out_t, int nout,
                                void* ctx, uint32_t* fault_code = nullptr) {
    (void)ctx;
    if (!fn) return false;
    {
        std::lock_guard<std::mutex> lk(g_sf_sigaction_mtx);
        if (g_sf_sigaction_active_calls++ == 0) {
            struct sigaction sa{};
            sa.sa_handler = sf_kernel_signal_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_NODEFER;
            sigaction(SIGILL, &sa, &g_sf_old_ill);
            sigaction(SIGSEGV, &sa, &g_sf_old_segv);
            sigaction(SIGBUS, &sa, &g_sf_old_bus);
        }
    }

    bool success = false;
    sf_kernel_in_safe_call = 1;
    if (sigsetjmp(sf_kernel_jmpbuf, 1) == 0) {
        fn(in_t, nin, out_t, nout);
        success = true;
    } else {
        if (fault_code) *fault_code = 0xDEADBEEF;
        success = false;
    }
    sf_kernel_in_safe_call = 0;

    {
        std::lock_guard<std::mutex> lk(g_sf_sigaction_mtx);
        if (--g_sf_sigaction_active_calls == 0) {
            sigaction(SIGILL, &g_sf_old_ill, nullptr);
            sigaction(SIGSEGV, &g_sf_old_segv, nullptr);
            sigaction(SIGBUS, &g_sf_old_bus, nullptr);
        }
    }
    return success;
}
#endif

/* ========================================================================= */
/* 3. IEEE-754 ERROR ACCUMULATION & FINITENESS CHECK                         */
/* ========================================================================= */

SF_DISABLE_OPT_BEGIN
static inline bool sf_is_ieee754_finite_f32(float val) noexcept {
    uint32_t bits = 0;
    memcpy(&bits, &val, sizeof(uint32_t));
    return ((bits & 0x7F800000u) != 0x7F800000u);
}

MSIMD_API void sf_accumulate_ieee754_error_f32(const float* cand_ptr,
                                              const float* ref_ptr,
                                              size_t n_elems,
                                              float& inout_max_err,
                                              float& inout_sum_sq,
                                              bool& inout_finite_ok) noexcept {
    for (size_t k = 0; k < n_elems; ++k) {
        float c_v = cand_ptr[k];
        float r_v = ref_ptr[k];
        if (!sf_is_ieee754_finite_f32(c_v) || !sf_is_ieee754_finite_f32(r_v)) {
            inout_finite_ok = false;
            inout_max_err = 1e30f;
            inout_sum_sq = 1e30f;
            return;
        }
        float diff = std::abs(c_v - r_v);
        if (diff > inout_max_err) inout_max_err = diff;
        inout_sum_sq += diff * diff;
    }
}

static inline bool sf_is_ieee754_finite_f64(double val) noexcept {
    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(uint64_t));
    return ((bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL);
}

MSIMD_API void sf_accumulate_ieee754_error_f64(const double* cand_ptr,
                                              const double* ref_ptr,
                                              size_t n_elems,
                                              float& inout_max_err,
                                              float& inout_sum_sq,
                                              bool& inout_finite_ok) noexcept {
    for (size_t k = 0; k < n_elems; ++k) {
        double c_v = cand_ptr[k];
        double r_v = ref_ptr[k];
        if (!sf_is_ieee754_finite_f64(c_v) || !sf_is_ieee754_finite_f64(r_v)) {
            inout_finite_ok = false;
            inout_max_err = 1e30f;
            inout_sum_sq = 1e30f;
            return;
        }
        double diff = std::abs(c_v - r_v);
        if (static_cast<float>(diff) > inout_max_err) inout_max_err = static_cast<float>(diff);
        inout_sum_sq += static_cast<float>(diff * diff);
    }
}
SF_DISABLE_OPT_END

/* ========================================================================= */
/* 4. REDLET PUZZLE ARENA CORE IMPLEMENTATION                                */
/* ========================================================================= */

MSIMD_API RedletPuzzleArena::RedletPuzzleArena() = default;

MSIMD_API RedletPuzzleArena::~RedletPuzzleArena() {
    destroy();
}

MSIMD_API void RedletPuzzleArena::init(sacculus_dispatch_table_t* dt,
                                       sacculus_runway_t* rw,
                                       sacculus_hotlog_t* hl,
                                       sacculus_coldlog_t* cl,
                                       size_t arena_bytes) {
    _dispatch = dt;
    _runway = rw;
    _hotlog = hl;
    _coldlog = cl;

    puzzle_heap_capacity = arena_bytes;
    puzzle_heap_base = static_cast<uint8_t*>(sf_os_reserve_memory(puzzle_heap_capacity));
    if (puzzle_heap_base && sf_os_commit_memory(puzzle_heap_base, puzzle_heap_capacity)) {
        sf_pretouch_memory(puzzle_heap_base, puzzle_heap_capacity);
        stagnant_in_payload = puzzle_heap_base;
        candidate_out_payload = puzzle_heap_base + STAGNANT_BUF_CAP;
        current_best_out_payload = puzzle_heap_base + STAGNANT_BUF_CAP * 2;
        puzzle_heap_used.store(STAGNANT_BUF_CAP * 3, std::memory_order_relaxed);
    }

    refresh_simd_pool();
    if (_coldlog) {
        sacculus_coldlog_write(_coldlog, SF_Ebin0_SAFEPOINT, SF_Ebin1_INFO, 0, "puzzle_arena_init_onheap");
    }
}

MSIMD_API void RedletPuzzleArena::destroy() {
    stop();
    if (puzzle_heap_base) {
        sf_os_release_memory(puzzle_heap_base, puzzle_heap_capacity);
        puzzle_heap_base = nullptr;
        puzzle_heap_capacity = 0;
        puzzle_heap_used.store(0, std::memory_order_relaxed);
        stagnant_in_payload = nullptr;
        candidate_out_payload = nullptr;
        current_best_out_payload = nullptr;
    }
}

MSIMD_API void RedletPuzzleArena::start() {
    if (_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (_thread.joinable()) {
        _thread.join();
    }
    _thread = std::thread([this]() {
        while (_running.load(std::memory_order_acquire)) {
            sf_puzzle_experiment_t exp{};
            sf_puzzle_batch_job_t batch_job{};
            bool has_batch = false;
            bool has_exp = false;

            {
                std::lock_guard<std::mutex> g(_queue_lock);
                if (!_pending_batch_jobs.empty()) {
                    batch_job = _pending_batch_jobs.front();
                    _pending_batch_jobs.erase(_pending_batch_jobs.begin());
                    has_batch = true;
                } else if (!_pending_experiments.empty()) {
                    exp = _pending_experiments.front();
                    _pending_experiments.erase(_pending_experiments.begin());
                    has_exp = true;
                }
            }

            if (has_batch) {
                run_puzzle_batch_job(batch_job);
            } else if (has_exp) {
                run_puzzle_experiment(exp);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_BEGIN, SF_Ebin1_INFO, 0, "puzzle_start",
                        "RedletPuzzleArena: On-heap candidate sandbox engaged");
}

MSIMD_API void RedletPuzzleArena::stop() {
    if (_running.exchange(false, std::memory_order_acq_rel)) {
        if (_thread.joinable()) {
            _thread.join();
        }
        std::lock_guard<std::mutex> g(_queue_lock);
        _pending_experiments.clear();
        _pending_batch_jobs.clear();

        sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_RESET, SF_Ebin1_INFO, 0, "puzzle_stop",
                            "RedletPuzzleArena: Candidate sandbox stopped cleanly");
    }
}

MSIMD_API void RedletPuzzleArena::refresh_simd_pool() {
    cached_isa_inventory = sf_cpu_isa_inventory_t::discover();
    active_simd_pool.clear();
    for (const auto& isa : cached_isa_inventory.discovered_isas) {
        if (_stricmp(isa.c_str(), "AUTO") != 0 && _stricmp(isa.c_str(), "ANY") != 0) {
            active_simd_pool.push_back(isa);
        }
    }
    if (active_simd_pool.empty()) {
        active_simd_pool.push_back("SCALAR");
    }
}

MSIMD_API std::vector<std::string> RedletPuzzleArena::pool_simd_capabilities() {
    refresh_simd_pool();
    return active_simd_pool;
}

MSIMD_API void RedletPuzzleArena::set_mode(SF_custominiVm_puzzle_exec_mode_t mode) {
    _execution_mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
}

MSIMD_API SF_custominiVm_puzzle_exec_mode_t RedletPuzzleArena::get_mode() const {
    return static_cast<SF_custominiVm_puzzle_exec_mode_t>(_execution_mode.load(std::memory_order_relaxed));
}

MSIMD_API void RedletPuzzleArena::set_sample_count(size_t count) {
    _stream_sample_count.store(count > 0 ? count : 1, std::memory_order_relaxed);
}

MSIMD_API size_t RedletPuzzleArena::get_sample_count() const {
    return _stream_sample_count.load(std::memory_order_relaxed);
}

MSIMD_API void RedletPuzzleArena::identify_stream_types(std::vector<uint8_t>& detected_dtypes) {
    detected_dtypes.clear();
    uint32_t dtype_mask = 0;
    if (!_runway) {
        detected_dtypes.push_back(SACFOR_F32);
        return;
    }

    auto scan_room = [&](int r, int reg) {
        if (r >= SACFOR_ROOM_COUNT || reg >= SACFOR_REGION_COUNT) return;
        auto* curr = _runway->motherline.room(r).regions[reg].get_head();
        size_t count = 0;
        while (curr && count++ < 32) {
            if (curr->try_pin()) {
                uint8_t dt = SACFOR_F32;
                auto* meta = curr->cold_meta.load(std::memory_order_relaxed);
                if (meta) {
                    uint32_t h = static_cast<uint32_t>(meta->type_sig_hash);
                    if (h <= static_cast<uint32_t>(SF_DTYPE_U256)) {
                        dt = static_cast<uint8_t>(h);
                    }
                }
                curr->unpin();
                if (!(dtype_mask & (1u << dt))) {
                    dtype_mask |= (1u << dt);
                    detected_dtypes.push_back(dt);
                }
            }
            curr = curr->next;
        }
    };

    scan_room(SF_ROOM_YOUNG, SF_REGION_DEMAND);
    scan_room(SF_ROOM_YOUNG, SF_REGION_HOTFIELD);
    scan_room(SF_ROOM_YOUNG, SF_REGION_TANK);
    scan_room(SF_ROOM_ADOLESCENT, SF_REGION_DEMAND);

    if (detected_dtypes.empty()) {
        detected_dtypes.push_back(SACFOR_F32);
    }
}

MSIMD_API bool RedletPuzzleArena::sample_runway_stream(size_t sample_iterations, std::vector<sf_puzzle_sample_t>& aggregated_samples) {
    aggregated_samples.clear();
    std::vector<uint8_t> active_dtypes;
    identify_stream_types(active_dtypes);

    for (size_t it = 0; it < sample_iterations; ++it) {
        for (uint8_t dt : active_dtypes) {
            sf_puzzle_sample_t smp{};
            smp.dtype = dt;
            bool captured = false;

            if (_runway) {
                for (int r = 0; r < SACFOR_ROOM_COUNT && !captured; ++r) {
                    for (int reg = 0; reg < SACFOR_REGION_COUNT && !captured; ++reg) {
                        auto* curr = _runway->motherline.room(r).regions[reg].get_head();
                        size_t depth = 0;
                        while (curr && depth++ < 16) {
                            if (curr->try_pin()) {
                                size_t fp = static_cast<size_t>(curr->memory_footprint.load(std::memory_order_relaxed));
                                if (fp > 0 && fp <= STAGNANT_BUF_CAP) {
                                    memcpy(smp.data, curr->body(), fp);
                                    smp.byte_size = fp;
                                    smp.ndim = 2;
                                    size_t elem_sz = (dt == SACFOR_F64 || dt == SACFOR_I64) ? 8 : 4;
                                    smp.shape[0] = 1;
                                    smp.shape[1] = static_cast<int32_t>(fp / elem_sz);
                                    captured = true;
                                }
                                curr->unpin();
                                if (captured) break;
                            }
                            curr = curr->next;
                        }
                    }
                }
            }

            if (!captured) {
                smp.byte_size = 1024;
                smp.ndim = 2;
                if (dt == SACFOR_F64) {
                    smp.shape[0] = 1; smp.shape[1] = 128;
                    auto* dptr = reinterpret_cast<double*>(smp.data);
                    for (size_t i = 0; i < 128; ++i) dptr[i] = static_cast<double>(i + 1 + it) * 0.05;
                } else {
                    smp.shape[0] = 1; smp.shape[1] = 256;
                    auto* fptr = reinterpret_cast<float*>(smp.data);
                    for (size_t i = 0; i < 256; ++i) fptr[i] = static_cast<float>(i + 1 + it) * 0.05f;
                }
            }
            aggregated_samples.push_back(smp);
        }
    }
    return !aggregated_samples.empty();
}

MSIMD_API void RedletPuzzleArena::submit_batch(uint32_t fn_id,
                                               const SF_custominiVm_tensor_t* batch_samples,
                                               int sample_count,
                                               const SF_custominiVm_tensor_t* target_prec,
                                               float target_epsilon) {
    if (!batch_samples || sample_count <= 0) return;
    std::lock_guard<std::mutex> g(_queue_lock);
    sf_puzzle_batch_job_t job{};
    job.fn_id = fn_id;
    job.target_epsilon = target_epsilon;
    if (target_prec && target_prec->data && target_prec->byte_size > 0) {
        job.target_precision_tensor = *target_prec;
        job.has_target_precision = true;
    }
    for (int i = 0; i < sample_count; ++i) {
        if (batch_samples[i].data && batch_samples[i].byte_size > 0) {
            sf_puzzle_sample_t smp{};
            smp.dtype = batch_samples[i].dtype;
            smp.byte_size = (batch_samples[i].byte_size <= STAGNANT_BUF_CAP) ? batch_samples[i].byte_size : STAGNANT_BUF_CAP;
            smp.ndim = batch_samples[i].ndim;
            for (int d = 0; d < 8; ++d) smp.shape[d] = batch_samples[i].shape[d];
            memcpy(smp.data, batch_samples[i].data, smp.byte_size);
            job.samples.push_back(smp);
        }
    }
    if (!job.samples.empty()) {
        _pending_batch_jobs.push_back(job);
    }
}

MSIMD_API bool RedletPuzzleArena::acquire_evaluation_samples(uint32_t fn_id,
                                                             std::vector<sf_puzzle_sample_t>& samples,
                                                             SF_custominiVm_tensor_t& target_prec,
                                                             bool& has_target_prec,
                                                             float& target_epsilon,
                                                             sf_puzzler_determinism_tier_e& out_tier) {
    {
        std::lock_guard<std::mutex> g(_queue_lock);
        for (auto it = _pending_batch_jobs.begin(); it != _pending_batch_jobs.end(); ++it) {
            if (it->fn_id == fn_id) {
                samples = it->samples;
                target_prec = it->target_precision_tensor;
                has_target_prec = it->has_target_precision;
                target_epsilon = it->target_epsilon;
                _pending_batch_jobs.erase(it);

                uint8_t dt = samples.empty() ? SACFOR_F32 : samples[0].dtype;
                out_tier = sf_puzzler_classify_tier(dt);
                sf_puzzler_validate_target_precision(dt, target_epsilon);
                return true;
            }
        }
    }

    has_target_prec = false;
    size_t n_iters = _stream_sample_count.load(std::memory_order_relaxed);
    bool ok = sample_runway_stream(n_iters > 0 ? n_iters : 5, samples);
    if (ok && !samples.empty()) {
        uint8_t dt = samples[0].dtype;
        out_tier = sf_puzzler_classify_tier(dt);
        sf_puzzler_validate_target_precision(dt, target_epsilon);
    } else {
        out_tier = sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
        target_epsilon = FLT_EPSILON;
    }
    return ok;
}

MSIMD_API sf_hybrid_compat_result_t RedletPuzzleArena::evaluate_hybrid_compatibility(sacculus_kernel_fn_t fn_a,
                                                                                    sacculus_kernel_fn_t fn_b,
                                                                                    const SF_custominiVm_tensor_t* in_t,
                                                                                    SF_custominiVm_tensor_t* out_t) {
    sf_hybrid_compat_result_t res{};
    if (!fn_a || !fn_b) {
        snprintf(res.reason, sizeof(res.reason), "null_kernel");
        return res;
    }

    constexpr int RUNS = 8;
    uint64_t cycles_a[RUNS];
    uint64_t cycles_b[RUNS];
    uint64_t cycles_hybrid[RUNS];

    uint64_t min_a = UINT64_MAX, max_a = 0;
    double sum_a = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        uint32_t fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        if (!sf_safe_kernel_call(fn_a, in_t, 1, out_t, 1, nullptr, &fault)) {
            snprintf(res.reason, sizeof(res.reason), "fn_a_fault:0x%X", fault);
            return res;
        }
        uint64_t dt = sacculus_rdtsc() - t0;
        if (dt == 0) dt = 1;
        cycles_a[r] = dt;
        sum_a += dt;
        if (dt < min_a) min_a = dt;
        if (dt > max_a) max_a = dt;
    }
    double mu_a = sum_a / RUNS;
    double var_a = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        double d = cycles_a[r] - mu_a;
        var_a += d * d;
    }
    var_a /= (RUNS > 1 ? (RUNS - 1) : 1);

    uint64_t min_b = UINT64_MAX, max_b = 0;
    double sum_b = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        uint32_t fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        if (!sf_safe_kernel_call(fn_b, in_t, 1, out_t, 1, nullptr, &fault)) {
            snprintf(res.reason, sizeof(res.reason), "fn_b_fault:0x%X", fault);
            return res;
        }
        uint64_t dt = sacculus_rdtsc() - t0;
        if (dt == 0) dt = 1;
        cycles_b[r] = dt;
        sum_b += dt;
        if (dt < min_b) min_b = dt;
        if (dt > max_b) max_b = dt;
    }
    double mu_b = sum_b / RUNS;
    double var_b = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        double d = cycles_b[r] - mu_b;
        var_b += d * d;
    }
    var_b /= (RUNS > 1 ? (RUNS - 1) : 1);

    double mu_expected = (mu_a + mu_b) * 0.5;
    double noise_sigma = std::sqrt(var_a + var_b);
    double cv_sys = noise_sigma / (mu_expected > 0 ? mu_expected : 1.0);
    double jitter_ratio = static_cast<double>((max_a - min_a) + (max_b - min_b)) / (2.0 * (mu_expected > 0 ? mu_expected : 1.0));

    double theta_dynamic = 1.0 + cv_sys + jitter_ratio;

    double sum_hyb = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        uint32_t fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        bool ok_a = sf_safe_kernel_call(fn_a, in_t, 1, out_t, 1, nullptr, &fault);
        bool ok_b = ok_a && sf_safe_kernel_call(fn_b, in_t, 1, out_t, 1, nullptr, &fault);
        uint64_t dt = sacculus_rdtsc() - t0;
        if (!ok_b) {
            snprintf(res.reason, sizeof(res.reason), "hybrid_fault:0x%X", fault);
            return res;
        }
        cycles_hybrid[r] = dt;
        sum_hyb += dt;
    }
    double mu_hybrid = (sum_hyb / RUNS) * 0.5;
    double observed_ratio = mu_hybrid / (mu_expected > 0 ? mu_expected : 1.0);

    res.mean_expected = mu_expected;
    res.mean_hybrid = mu_hybrid;
    res.dynamic_bound = theta_dynamic;
    res.observed_ratio = observed_ratio;

    if (observed_ratio <= theta_dynamic) {
        res.compatible = true;
        snprintf(res.reason, sizeof(res.reason), "dynamically_compatible(ratio=%.3f<=bound=%.3f)", observed_ratio, theta_dynamic);
    } else {
        res.compatible = false;
        snprintf(res.reason, sizeof(res.reason), "stall_incompatible(ratio=%.3f>bound=%.3f)", observed_ratio, theta_dynamic);
    }
    return res;
}

MSIMD_API sf_score_eval_t RedletPuzzleArena::evaluate_score(uint64_t cand_cycles,
                                                           uint64_t cur_cycles,
                                                           float max_abs_err,
                                                           float mse,
                                                           float target_epsilon,
                                                           const SF_custominiVm_pmu_stat_t* pmu) {
    sf_score_eval_t res{};
    res.max_err = max_abs_err;
    res.mse = mse;

    bool finite_ok = sf_is_ieee754_finite_f32(max_abs_err) &&
                     sf_is_ieee754_finite_f32(mse) &&
                     sf_is_ieee754_finite_f32(target_epsilon);
    bool speed_ok = (cand_cycles < cur_cycles);
    bool precision_ok = false;
    if (target_epsilon <= 0.0f) {
        // EXACT_ALGEBRAIC: strictly 0-bit deviation tolerance
        precision_ok = finite_ok && (max_abs_err == 0.0f);
    } else {
        // NUMERIC_IEEE_FP: within specified target epsilon or machine epsilon
        precision_ok = finite_ok && ((max_abs_err <= target_epsilon) || (mse <= target_epsilon));
    }

    if (!speed_ok || !precision_ok) {
        res.qualified = false;
        res.score = -1.0;
        return res;
    }

    double speedup = static_cast<double>(cur_cycles - cand_cycles) / static_cast<double>(cur_cycles > 0 ? cur_cycles : 1);
    double prec_margin = 0.0;
    if (target_epsilon > 0.0f) {
        prec_margin = static_cast<double>(target_epsilon - mse) / static_cast<double>(target_epsilon);
        if (prec_margin < 0.0) prec_margin = 0.0;
    }

    double pmu_efficiency = 1.0;
    if (pmu) {
        double ipc_bonus = std::min(1.5, pmu->ipc / 2.0);
        double downclock_factor = pmu->freq_downclock_ratio;
        double cache_penalty = (pmu->llc_cache_misses > 0 && pmu->cycles > 0)
            ? std::min(0.4, static_cast<double>(pmu->llc_cache_misses * 64) / static_cast<double>(pmu->cycles))
            : 0.0;
        pmu_efficiency = std::max(0.1, (ipc_bonus * downclock_factor) - cache_penalty);
    }

    res.speedup_ratio = speedup;
    res.precision_headroom = prec_margin;
    res.score = (0.55 * speedup + 0.35 * prec_margin + 0.10 * pmu_efficiency);
    res.qualified = true;
    return res;
}

MSIMD_API void RedletPuzzleArena::redirect_stream(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t new_cycles, const char* name, double score) {
    if (!_dispatch || !new_fn) return;

    SF_custominiVm_set_pausepoint_gate(1);

    _dispatch->swap(fn_id, new_fn, new_cycles);

    if (_runway) {
        _runway->batch_optimize_egg_objects(fn_id, name, 0, new_cycles);
    }

    SF_custominiVm_set_pausepoint_gate(0);

    sacculus_hotlog_write(_hotlog, SF_Ebin0_HOTSWAP, SF_Ebin1_INFO,
        fn_id, static_cast<uint16_t>(new_cycles & 0xFFFF), 0, 0, 0, 0,
        static_cast<uint32_t>(score * 1000.0), name ? name : "puzzle_win");

    if (_coldlog) {
        char msg[64];
        snprintf(msg, sizeof(msg), "redirect:%s cyc=%llu sc=%.3f", name ? name : "simd",
                 static_cast<unsigned long long>(new_cycles), score);
        sacculus_coldlog_write(_coldlog, SF_Ebin0_HOTSWAP, SF_Ebin1_INFO, fn_id, msg);
    }
}

MSIMD_API void RedletPuzzleArena::submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name,
                                        const char* target_isa, float target_epsilon,
                                        const SF_custominiVm_tensor_t* target_tensor) {
    std::lock_guard<std::mutex> g(_queue_lock);
    sf_puzzle_experiment_t exp{};
    exp.fn_id = fn_id;
    exp.candidate = candidate;
    if (name) {
        strncpy(exp.name, name, sizeof(exp.name) - 1);
    } else {
        strncpy(exp.name, "candidate_isa", sizeof(exp.name) - 1);
    }
    if (target_isa) {
        strncpy(exp.target_isa, target_isa, sizeof(exp.target_isa) - 1);
    } else {
        strncpy(exp.target_isa, "AUTO", sizeof(exp.target_isa) - 1);
    }
    exp.target_epsilon = target_epsilon;
    if (target_tensor && target_tensor->data && target_tensor->byte_size > 0) {
        exp.target_tensor = *target_tensor;
        exp.has_target_tensor = true;
    }
    _pending_experiments.push_back(exp);
}

MSIMD_API void RedletPuzzleArena::run_puzzle_suite_parallel(uint32_t fn_id,
                                                           const SF_custominiVm_isa_candidate_t* candidates,
                                                           int n_candidates,
                                                           float target_epsilon,
                                                           const SF_custominiVm_tensor_t* target_tensor) {
    if (!_dispatch || !candidates || n_candidates <= 0 || !current_best_out_payload || !candidate_out_payload) return;
    auto* entry = _dispatch->get(fn_id);
    if (!entry || !entry->fn.load()) return;

    std::vector<sf_puzzle_sample_t> samples;
    SF_custominiVm_tensor_t target_prec{};
    bool has_target_prec = false;
    float final_epsilon = target_epsilon;
    if (target_tensor && target_tensor->data && target_tensor->byte_size > 0) {
        target_prec = *target_tensor;
        has_target_prec = true;
    }

    sf_puzzler_determinism_tier_e tier = sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
    if (!acquire_evaluation_samples(fn_id, samples, target_prec, has_target_prec, final_epsilon, tier)) {
        return;
    }
    if (samples.empty()) return;

    uint64_t cur_cycles_total = 0;
    auto current_fn = entry->fn.load(std::memory_order_acquire);

    // Warmup current_fn
    if (!samples.empty()) {
        SF_custominiVm_tensor_t win_t{};
        win_t.data = const_cast<uint8_t*>(samples[0].data);
        win_t.ndim = samples[0].ndim;
        for (int d = 0; d < 8; ++d) win_t.shape[d] = samples[0].shape[d];
        win_t.dtype = samples[0].dtype;
        win_t.byte_size = samples[0].byte_size;
        SF_custominiVm_tensor_t wout_t = win_t;
        wout_t.data = current_best_out_payload;
        uint32_t wfault = 0;
        sf_safe_kernel_call(current_fn, &win_t, 1, &wout_t, 1, nullptr, &wfault);
    }

    std::vector<sf_puzzle_sample_t> baseline_outputs(samples.size());
    for (size_t s_idx = 0; s_idx < samples.size(); ++s_idx) {
        const auto& smp = samples[s_idx];
        SF_custominiVm_tensor_t in_t{};
        in_t.data = const_cast<uint8_t*>(smp.data);
        in_t.ndim = smp.ndim;
        for (int d = 0; d < 8; ++d) in_t.shape[d] = smp.shape[d];
        in_t.dtype = smp.dtype;
        in_t.byte_size = smp.byte_size;

        SF_custominiVm_tensor_t out_t = in_t;
        out_t.data = baseline_outputs[s_idx].data;
        baseline_outputs[s_idx].byte_size = smp.byte_size;
        memset(baseline_outputs[s_idx].data, 0, smp.byte_size);

        uint32_t cur_fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        bool ok = sf_safe_kernel_call(current_fn, &in_t, 1, &out_t, 1, nullptr, &cur_fault);
        uint64_t dt = sacculus_rdtsc() - t0;
        if (!ok) return;
        cur_cycles_total += (dt > 0 ? dt : 1);
        if (s_idx + 1 == samples.size()) {
            memcpy(current_best_out_payload, baseline_outputs[s_idx].data, smp.byte_size);
        }
    }
    uint64_t current_cycles = cur_cycles_total / samples.size();
    if (current_cycles == 0) current_cycles = 1;

    std::vector<sf_candidate_eval_t> evals(n_candidates);
    std::vector<std::thread> workers;
    workers.reserve(n_candidates);

    for (int i = 0; i < n_candidates; ++i) {
        evals[i].idx = i;
        evals[i].fn = candidates[i].fn;
        if (candidates[i].variant_name) {
            strncpy(evals[i].variant_name, candidates[i].variant_name, sizeof(evals[i].variant_name) - 1);
        } else {
            strncpy(evals[i].variant_name, "suite_variant", sizeof(evals[i].variant_name) - 1);
        }
        if (candidates[i].isa_tag) {
            strncpy(evals[i].isa_tag, candidates[i].isa_tag, sizeof(evals[i].isa_tag) - 1);
        } else {
            strncpy(evals[i].isa_tag, "AUTO", sizeof(evals[i].isa_tag) - 1);
        }

        if (!candidates[i].fn) continue;
        if (!cached_isa_inventory.has_isa(evals[i].isa_tag)) {
            continue;
        }

        workers.emplace_back([this, &evals, i, &samples, &baseline_outputs, has_target_prec, &target_prec, current_cycles, final_epsilon, tier]() {
            // Pin execution to physical P-cores to eliminate SMT contention and E-core latency
            auto* nexus = sf_get_hardware_nexus();
            if (nexus) {
                nexus->hybrid.bind_current_thread(SF_CORE_TIER_PERFORMANCE);
                nexus->smt.apply_affinity_policy(SF_AFFINITY_PHYSICAL_CORES_ONLY);
            }

            uint8_t* local_cand_out = static_cast<uint8_t*>(_mm_malloc(STAGNANT_BUF_CAP, 64));
            if (!local_cand_out) return;
            uint64_t cand_cycles_total = 0;
            float max_err = 0.0f;
            float sum_sq = 0.0f;
            size_t total_elems_tested = 0;
            bool all_ok = true;
            uint32_t cand_fault = 0;

            // Warmup pass
            if (!samples.empty()) {
                SF_custominiVm_tensor_t win_t{};
                win_t.data = const_cast<uint8_t*>(samples[0].data);
                win_t.ndim = samples[0].ndim;
                for (int d = 0; d < 8; ++d) win_t.shape[d] = samples[0].shape[d];
                win_t.dtype = samples[0].dtype;
                win_t.byte_size = samples[0].byte_size;
                SF_custominiVm_tensor_t wout_t = win_t;
                wout_t.data = local_cand_out;
                uint32_t wfault = 0;
                sf_safe_kernel_call(evals[i].fn, &win_t, 1, &wout_t, 1, nullptr, &wfault);
            }

            sf_pmu_session_t cand_pmu_sess{};
            SF_custominiVm_pmu_stat_t cand_pmu{};
            size_t total_bytes_tested = 0;
            cand_pmu_sess.begin();

            for (size_t s_idx = 0; s_idx < samples.size(); ++s_idx) {
                const auto& smp = samples[s_idx];
                memset(local_cand_out, 0, smp.byte_size);
                SF_custominiVm_tensor_t in_t{};
                in_t.data = const_cast<uint8_t*>(smp.data);
                in_t.ndim = smp.ndim;
                for (int d = 0; d < 8; ++d) in_t.shape[d] = smp.shape[d];
                in_t.dtype = smp.dtype;
                in_t.byte_size = smp.byte_size;

                SF_custominiVm_tensor_t cand_out_t = in_t;
                cand_out_t.data = local_cand_out;

                uint64_t t1 = sacculus_rdtsc();
                bool ok = sf_safe_kernel_call(evals[i].fn, &in_t, 1, &cand_out_t, 1, nullptr, &cand_fault);
                uint64_t dt = sacculus_rdtsc() - t1;
                if (!ok) {
                    all_ok = false;
                    break;
                }
                cand_cycles_total += dt;
                total_bytes_tested += smp.byte_size;

                const uint8_t* ref_bytes = (has_target_prec && target_prec.data)
                    ? static_cast<const uint8_t*>(target_prec.data)
                    : static_cast<const uint8_t*>(baseline_outputs[s_idx].data);
                size_t cmp_bytes = (has_target_prec && target_prec.data && target_prec.byte_size < smp.byte_size)
                    ? target_prec.byte_size
                    : smp.byte_size;

                if (tier == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) {
                    // Two-tier determinism directive: Non-FP requires strict 0-bit deviation (exact memcmp == 0)
                    if (memcmp(local_cand_out, ref_bytes, cmp_bytes) != 0) {
                        all_ok = false;
                        max_err = 1.0f;
                        sum_sq = 1.0f;
                        break;
                    }
                    total_elems_tested += cmp_bytes;
                } else if (smp.dtype == SACFOR_F64) {
                    // NUMERIC_IEEE_FP: Double (F64) operations with machine epsilon (DBL_EPSILON) bounds
                    size_t n_elems = cmp_bytes / sizeof(double);
                    const double* cand_ptr = reinterpret_cast<const double*>(local_cand_out);
                    const double* ref_ptr = reinterpret_cast<const double*>(ref_bytes);
                    bool finite_ok = true;
                    sf_accumulate_ieee754_error_f64(cand_ptr, ref_ptr, n_elems, max_err, sum_sq, finite_ok);
                    if (!finite_ok) {
                        all_ok = false;
                        break;
                    }
                    total_elems_tested += n_elems;
                } else {
                    // NUMERIC_IEEE_FP: Float32 operations with machine epsilon (FLT_EPSILON) bounds
                    size_t n_elems = cmp_bytes / sizeof(float);
                    const float* cand_ptr = reinterpret_cast<const float*>(local_cand_out);
                    const float* ref_ptr = reinterpret_cast<const float*>(ref_bytes);
                    bool finite_ok = true;
                    sf_accumulate_ieee754_error_f32(cand_ptr, ref_ptr, n_elems, max_err, sum_sq, finite_ok);
                    if (!finite_ok) {
                        all_ok = false;
                        break;
                    }
                    total_elems_tested += n_elems;
                }
            }
            cand_pmu_sess.end(&cand_pmu, total_bytes_tested);
            _mm_free(local_cand_out);

            evals[i].executed_ok = all_ok;
            evals[i].fault_code = cand_fault;
            if (!all_ok) return;

            uint64_t cand_cycles = cand_cycles_total / samples.size();
            evals[i].cycles = cand_cycles;
            float mse = sum_sq / static_cast<float>(total_elems_tested > 0 ? total_elems_tested : 1);
            evals[i].max_abs_err = max_err;
            evals[i].mse = mse;

            auto sc = evaluate_score(cand_cycles, current_cycles, max_err, mse, final_epsilon, &cand_pmu);
            if (tier == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) {
                evals[i].precision_passed = (max_err == 0.0f);
            } else {
                evals[i].precision_passed = (max_err <= final_epsilon) || (mse <= final_epsilon);
            }
            evals[i].speed_passed = (cand_cycles < current_cycles);
            evals[i].score = sc.score;
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    int best_idx = -1;
    double best_score = -1.0;

    for (int i = 0; i < n_candidates; ++i) {
        if (evals[i].executed_ok && evals[i].score > best_score) {
            best_score = evals[i].score;
            best_idx = i;
        }
    }

    if (n_candidates >= 2) {
        int cand_a = 0, cand_b = 1;
        if (candidates[cand_a].fn && candidates[cand_b].fn) {
            SF_custominiVm_tensor_t test_in{};
            test_in.data = const_cast<uint8_t*>(samples[0].data);
            test_in.ndim = samples[0].ndim;
            for (int d = 0; d < 8; ++d) test_in.shape[d] = samples[0].shape[d];
            test_in.dtype = samples[0].dtype;
            test_in.byte_size = samples[0].byte_size;

            SF_custominiVm_tensor_t test_out = test_in;
            test_out.data = candidate_out_payload;

            auto compat = evaluate_hybrid_compatibility(candidates[cand_a].fn, candidates[cand_b].fn, &test_in, &test_out);
            if (compat.compatible) {
                uint64_t hyb_cycles = static_cast<uint64_t>(compat.mean_hybrid);
                auto hyb_score = evaluate_score(hyb_cycles, current_cycles, evals[cand_a].max_abs_err, evals[cand_a].mse, final_epsilon);
                if (hyb_score.qualified && hyb_score.score > best_score) {
                    best_score = hyb_score.score;
                    best_idx = cand_a;
                }
            }
        }
    }

    if (best_idx >= 0 && best_score > 0.0) {
        redirect_stream(fn_id, evals[best_idx].fn, evals[best_idx].cycles, evals[best_idx].variant_name, best_score);
    }
}

MSIMD_API void RedletPuzzleArena::submit_suite(uint32_t fn_id, const SF_custominiVm_isa_candidate_t* candidates, int n_candidates,
                                               float target_epsilon, const SF_custominiVm_tensor_t* target_tensor) {
    if (!candidates || n_candidates <= 0) return;

    uint8_t mode = _execution_mode.load(std::memory_order_relaxed);
    bool run_parallel = false;
    if (mode == static_cast<uint8_t>(SF_PUZZLE_EXEC_PARALLEL)) {
        run_parallel = true;
    } else if (mode == static_cast<uint8_t>(SF_PUZZLE_EXEC_ADAPTIVE)) {
        unsigned int hw_cores = std::thread::hardware_concurrency();
        if (hw_cores >= 4 && n_candidates > 1) {
            run_parallel = true;
        }
    }

    if (run_parallel) {
        run_puzzle_suite_parallel(fn_id, candidates, n_candidates, target_epsilon, target_tensor);
    } else {
        for (int i = 0; i < n_candidates; ++i) {
            const auto& c = candidates[i];
            if (c.fn) {
                submit(fn_id, c.fn, c.variant_name ? c.variant_name : "suite_variant",
                       c.isa_tag ? c.isa_tag : "AUTO", target_epsilon, target_tensor);
            }
        }
    }
}

MSIMD_API void RedletPuzzleArena::run_puzzle_experiment_with_samples(const sf_puzzle_experiment_t& exp,
                                                                    const std::vector<sf_puzzle_sample_t>& samples,
                                                                    const SF_custominiVm_tensor_t& target_prec,
                                                                    bool has_target_prec,
                                                                    float target_eps,
                                                                    sf_puzzler_determinism_tier_e tier) {
    if (!_dispatch || samples.empty() || !current_best_out_payload || !candidate_out_payload) return;
    auto* entry = _dispatch->get(exp.fn_id);
    if (!entry || !entry->fn.load() || !exp.candidate) return;

    uint64_t cur_cycles_total = 0;
    uint64_t cand_cycles_total = 0;
    float max_abs_err = 0.0f;
    float sum_sq_err = 0.0f;
    size_t total_elems = 0;
    auto current_fn = entry->fn.load(std::memory_order_acquire);

    // Warmup current_fn and candidate
    {
        SF_custominiVm_tensor_t win_t{};
        win_t.data = const_cast<uint8_t*>(samples[0].data);
        win_t.ndim = samples[0].ndim;
        for (int d = 0; d < 8; ++d) win_t.shape[d] = samples[0].shape[d];
        win_t.dtype = samples[0].dtype;
        win_t.byte_size = samples[0].byte_size;
        SF_custominiVm_tensor_t wout_t = win_t;
        wout_t.data = current_best_out_payload;
        uint32_t wfault = 0;
        sf_safe_kernel_call(current_fn, &win_t, 1, &wout_t, 1, nullptr, &wfault);
        wout_t.data = candidate_out_payload;
        sf_safe_kernel_call(exp.candidate, &win_t, 1, &wout_t, 1, nullptr, &wfault);
    }

    for (const auto& smp : samples) {
        SF_custominiVm_tensor_t in_t{};
        in_t.data = const_cast<uint8_t*>(smp.data);
        in_t.ndim = smp.ndim;
        for (int d = 0; d < 8; ++d) in_t.shape[d] = smp.shape[d];
        in_t.dtype = smp.dtype;
        in_t.byte_size = smp.byte_size;

        SF_custominiVm_tensor_t current_out_t = in_t;
        current_out_t.data = current_best_out_payload;
        memset(current_best_out_payload, 0, smp.byte_size);

        uint32_t cur_fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        bool cur_ok = sf_safe_kernel_call(current_fn, &in_t, 1, &current_out_t, 1, nullptr, &cur_fault);
        uint64_t dt0 = sacculus_rdtsc() - t0;
        if (!cur_ok) return;
        cur_cycles_total += (dt0 > 0 ? dt0 : 1);

        SF_custominiVm_tensor_t candidate_out_t = in_t;
        candidate_out_t.data = candidate_out_payload;
        memset(candidate_out_payload, 0, smp.byte_size);

        uint32_t cand_fault = 0;
        sf_pmu_session_t exp_pmu_sess{};
        SF_custominiVm_pmu_stat_t exp_pmu{};
        exp_pmu_sess.begin();
        uint64_t t1 = sacculus_rdtsc();
        bool cand_ok = sf_safe_kernel_call(exp.candidate, &in_t, 1, &candidate_out_t, 1, nullptr, &cand_fault);
        uint64_t dt1 = sacculus_rdtsc() - t1;
        exp_pmu_sess.end(&exp_pmu, smp.byte_size);

        if (!cand_ok) {
            sacculus_hotlog_write(_hotlog, SF_Ebin0_HOTSWAP_FAIL, SF_Ebin1_WARN,
                exp.fn_id, 0, 0, 0, 0, 0, cand_fault, "fault_isolated");
            if (_coldlog) {
                char msg[32];
                snprintf(msg, sizeof(msg), "fault:0x%08X", cand_fault);
                sacculus_coldlog_write(_coldlog, SF_Ebin0_HOTSWAP_FAIL, SF_Ebin1_WARN, exp.fn_id, msg);
            }
            return;
        }
        cand_cycles_total += dt1;

        const uint8_t* ref_bytes = (has_target_prec && target_prec.data)
            ? static_cast<const uint8_t*>(target_prec.data)
            : static_cast<const uint8_t*>(current_best_out_payload);
        size_t cmp_bytes = (has_target_prec && target_prec.data && target_prec.byte_size < smp.byte_size)
            ? target_prec.byte_size
            : smp.byte_size;

        if (tier == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) {
            if (memcmp(candidate_out_payload, ref_bytes, cmp_bytes) != 0) {
                max_abs_err = 1.0f;
                sum_sq_err = 1.0f;
                break;
            }
            total_elems += cmp_bytes;
        } else if (smp.dtype == SACFOR_F64) {
            size_t n_elems = cmp_bytes / sizeof(double);
            const double* cand_ptr = reinterpret_cast<const double*>(candidate_out_payload);
            const double* ref_ptr = reinterpret_cast<const double*>(ref_bytes);
            bool finite_ok = true;
            sf_accumulate_ieee754_error_f64(cand_ptr, ref_ptr, n_elems, max_abs_err, sum_sq_err, finite_ok);
            if (!finite_ok) {
                break;
            }
            total_elems += n_elems;
        } else {
            size_t n_elems = cmp_bytes / sizeof(float);
            const float* cand_ptr = reinterpret_cast<const float*>(candidate_out_payload);
            const float* ref_ptr = reinterpret_cast<const float*>(ref_bytes);
            bool finite_ok = true;
            sf_accumulate_ieee754_error_f32(cand_ptr, ref_ptr, n_elems, max_abs_err, sum_sq_err, finite_ok);
            if (!finite_ok) {
                break;
            }
            total_elems += n_elems;
        }
    }

    uint64_t current_cycles = cur_cycles_total / samples.size();
    if (current_cycles == 0) current_cycles = 1;
    uint64_t candidate_cycles = cand_cycles_total / samples.size();
    float mse = sum_sq_err / static_cast<float>(total_elems > 0 ? total_elems : 1);

    auto score_eval = evaluate_score(candidate_cycles, current_cycles, max_abs_err, mse, target_eps);

    if (score_eval.qualified && score_eval.score > 0.0) {
        redirect_stream(exp.fn_id, exp.candidate, candidate_cycles, exp.name, score_eval.score);
    } else {
        bool prec_failed = (tier == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC)
            ? (max_abs_err != 0.0f)
            : (max_abs_err > target_eps && mse > target_eps);
        const char* reason = prec_failed ? "fail_precision" : "fail_speed";
        if (prec_failed) {
            sacculus_hotlog_write(_hotlog, SF_Ebin0_HEAL_NUMERIC_DIVERGE, SF_Ebin1_WARN,
                exp.fn_id, 0, 0, 0, 0, 0, static_cast<uint32_t>(max_abs_err * 10000.0f), exp.name);
        }
        sacculus_hotlog_write(_hotlog, SF_Ebin0_HOTSWAP_FAIL, SF_Ebin1_TRACE,
            exp.fn_id, 0, 0, 0, 0, 0, static_cast<uint32_t>(max_abs_err * 10000.0f), reason);
    }
}

MSIMD_API void RedletPuzzleArena::run_puzzle_experiment(const sf_puzzle_experiment_t& exp) {
    if (!_dispatch) return;
    auto* entry = _dispatch->get(exp.fn_id);
    if (!entry || !entry->fn.load() || !exp.candidate) {
        return;
    }

    if (!cached_isa_inventory.has_isa(exp.target_isa)) {
        if (_coldlog) {
            sacculus_coldlog_write(_coldlog, SF_Ebin0_HOTSWAP_FAIL, SF_Ebin1_TRACE, exp.fn_id, "isa_not_on_cpu");
        }
        return;
    }

    std::vector<sf_puzzle_sample_t> samples;
    SF_custominiVm_tensor_t target_prec{};
    bool has_target_prec = false;
    float target_eps = exp.target_epsilon;
    if (exp.has_target_tensor && exp.target_tensor.data && exp.target_tensor.byte_size > 0) {
        target_prec = exp.target_tensor;
        has_target_prec = true;
    }

    sf_puzzler_determinism_tier_e tier = sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
    if (!acquire_evaluation_samples(exp.fn_id, samples, target_prec, has_target_prec, target_eps, tier)) {
        return;
    }
    run_puzzle_experiment_with_samples(exp, samples, target_prec, has_target_prec, target_eps, tier);
}

MSIMD_API void RedletPuzzleArena::run_puzzle_batch_job(const sf_puzzle_batch_job_t& job) {
    if (!_dispatch || job.samples.empty()) return;
    auto* entry = _dispatch->get(job.fn_id);
    if (!entry || !entry->fn.load()) return;

    auto capabilities = pool_simd_capabilities();
    if (capabilities.empty()) return;

    auto cur_fn = entry->fn.load(std::memory_order_acquire);
    if (!cur_fn) return;

    sf_puzzle_experiment_t exp{};
    exp.fn_id = job.fn_id;
    exp.candidate = cur_fn;
    strncpy(exp.name, "batch_job_stream", sizeof(exp.name) - 1);
    strncpy(exp.target_isa, capabilities[0].c_str(), sizeof(exp.target_isa) - 1);
    exp.target_epsilon = job.target_epsilon;
    exp.target_tensor = job.target_precision_tensor;
    exp.has_target_tensor = job.has_target_precision;

    uint8_t dt = job.samples[0].dtype;
    sf_puzzler_determinism_tier_e tier = sf_puzzler_classify_tier(dt);
    float final_eps = job.target_epsilon;
    sf_puzzler_validate_target_precision(dt, final_eps);

    run_puzzle_experiment_with_samples(
        exp, job.samples, job.target_precision_tensor, job.has_target_precision, final_eps, tier);
}

MSIMD_API double RedletPuzzleArena::score_variant(uint64_t baseline_cycles, uint64_t candidate_cycles, float max_err) {
    if (candidate_cycles == 0 || baseline_cycles == 0) return 0.0;
    if (max_err > 1e-3f) return -1.0;

    double speedup = static_cast<double>(baseline_cycles) / static_cast<double>(candidate_cycles);
    double accuracy_factor = 1.0 / (1.0 + std::abs(max_err));
    return speedup * accuracy_factor;
}

MSIMD_API void RedletPuzzleArena::evaluate_candidate(sf_candidate_eval_t& candidate) {
    std::lock_guard<std::mutex> g(_queue_lock);
    candidate.executed_ok = true;
    candidate.precision_passed = (candidate.max_abs_err <= 1e-4f);
    candidate.speed_passed = (candidate.cycles < UINT64_MAX);
    if (candidate.precision_passed && candidate.speed_passed) {
        candidate.score = 1.25;
    } else {
        candidate.score = -1.0;
    }
    _eval_history.push_back(candidate);
}

MSIMD_API size_t RedletPuzzleArena::history_count() const {
    return _eval_history.size();
}

/* ========================================================================= */
/* 5. CANONICAL C APIS                                                       */
/* ========================================================================= */

extern "C" {

MSIMD_API RedletPuzzleArena* sf_get_default_puzzle_arena(void) {
    return &g_default_redlet_puzzle_arena;
}

MSIMD_API void SF_custominiVm_puzzle_submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name,
                                            const char* target_isa, float target_epsilon,
                                            const SF_custominiVm_tensor_t* target_tensor) {
    g_default_redlet_puzzle_arena.submit(fn_id, candidate, name, target_isa, target_epsilon, target_tensor);
}

MSIMD_API void SF_custominiVm_puzzle_submit_batch(uint32_t fn_id, const SF_custominiVm_tensor_t* batch_samples,
                                                  int sample_count, const SF_custominiVm_tensor_t* target_prec,
                                                  float target_epsilon) {
    g_default_redlet_puzzle_arena.submit_batch(fn_id, batch_samples, sample_count, target_prec, target_epsilon);
}

MSIMD_API void SF_custominiVm_puzzle_submit_suite(uint32_t fn_id, const SF_custominiVm_isa_candidate_t* candidates,
                                                  int n_candidates, float target_epsilon,
                                                  const SF_custominiVm_tensor_t* target_tensor) {
    g_default_redlet_puzzle_arena.submit_suite(fn_id, candidates, n_candidates, target_epsilon, target_tensor);
}

MSIMD_API void SF_custominiVm_puzzle_set_mode(SF_custominiVm_puzzle_exec_mode_t mode) {
    g_default_redlet_puzzle_arena.set_mode(mode);
}

MSIMD_API SF_custominiVm_puzzle_exec_mode_t SF_custominiVm_puzzle_get_mode(void) {
    return g_default_redlet_puzzle_arena.get_mode();
}

MSIMD_API void SF_custominiVm_puzzle_set_sample_count(size_t count) {
    g_default_redlet_puzzle_arena.set_sample_count(count);
}

MSIMD_API size_t SF_custominiVm_puzzle_get_sample_count(void) {
    return g_default_redlet_puzzle_arena.get_sample_count();
}

} // extern "C"
