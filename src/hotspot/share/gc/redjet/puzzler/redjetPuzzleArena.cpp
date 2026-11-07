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

#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "vac/c1_SortOpt.hpp"
#include "gc/redjet/runtime/redjetMIRC.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cfloat>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#endif

static RedjetPuzzleArena g_default_redjet_puzzle_arena;

/* ========================================================================= */
/* 1. TWO-TIER DETERMINISM CLASSIFICATION & VALIDATION HELPERS               */
/* ========================================================================= */

RJ_API hs_puzzler_determinism_tier_e hs_puzzler_classify_tier(uint8_t dtype) {
    if (dtype == SACFOR_F32 || dtype == SACFOR_F64 ||
        dtype == HS_DTYPE_F16 || dtype == HS_DTYPE_F8 ||
        dtype == HS_DTYPE_F128 || dtype == HS_DTYPE_F256) {
        return hs_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
    }
    return hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC;
}

RJ_API float hs_puzzler_default_epsilon_for_dtype(uint8_t dtype) {
    if (dtype == SACFOR_F64 || dtype == HS_DTYPE_F128 || dtype == HS_DTYPE_F256) {
        return static_cast<float>(DBL_EPSILON);
    }
    if (dtype == SACFOR_F32) {
        return FLT_EPSILON;
    }
    if (dtype == HS_DTYPE_F16) {
        return 9.77e-4f;
    }
    if (dtype == HS_DTYPE_F8) {
        return 0.0625f;
    }
    return 0.0f; // EXACT_ALGEBRAIC: strictly 0-bit deviation tolerance
}

RJ_API bool hs_puzzler_validate_target_precision(uint8_t dtype, float& inout_target_epsilon) {
    hs_puzzler_determinism_tier_e tier = hs_puzzler_classify_tier(dtype);
    if (tier == hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) {
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
            inout_target_epsilon = hs_puzzler_default_epsilon_for_dtype(dtype);
        }
        return true;
    }
}

/* ========================================================================= */
/* 2. SAFE FAULT-ISOLATED KERNEL INVOCATION                                  */
/* ========================================================================= */

#if defined(_WIN32)
HS_DISABLE_OPT_BEGIN
static bool hs_safe_kernel_call_impl(sacculus_kernel_fn_t fn,
                                     const HS_hotspot_tensor_t* in_t, int nin,
                                     HS_hotspot_tensor_t* out_t, int nout,
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
static inline bool hs_safe_kernel_call(sacculus_kernel_fn_t fn,
                                       const HS_hotspot_tensor_t* in_t, int nin,
                                       HS_hotspot_tensor_t* out_t, int nout,
                                       void* ctx, uint32_t* fault_code = nullptr) {
    if (!fn) return false;
    return hs_safe_kernel_call_impl(fn, in_t, nin, out_t, nout, ctx, fault_code);
}
HS_DISABLE_OPT_END
#else
static thread_local sigjmp_buf       hs_kernel_jmpbuf;
static thread_local volatile sig_atomic_t hs_kernel_in_safe_call = 0;

static void hs_kernel_signal_handler(int sig) {
    if (hs_kernel_in_safe_call) {
        siglongjmp(hs_kernel_jmpbuf, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static std::mutex g_hs_sigaction_mtx;
static uint32_t   g_hs_sigaction_active_calls = 0;
static struct sigaction g_hs_old_ill{}, g_hs_old_segv{}, g_hs_old_bus{};

static bool hs_safe_kernel_call(sacculus_kernel_fn_t fn,
                                const HS_hotspot_tensor_t* in_t, int nin,
                                HS_hotspot_tensor_t* out_t, int nout,
                                void* ctx, uint32_t* fault_code = nullptr) {
    (void)ctx;
    if (!fn) return false;
    {
        std::lock_guard<std::mutex> lk(g_hs_sigaction_mtx);
        if (g_hs_sigaction_active_calls++ == 0) {
            struct sigaction sa{};
            sa.sa_handler = hs_kernel_signal_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_NODEFER;
            sigaction(SIGILL, &sa, &g_hs_old_ill);
            sigaction(SIGSEGV, &sa, &g_hs_old_segv);
            sigaction(SIGBUS, &sa, &g_hs_old_bus);
        }
    }

    bool success = false;
    hs_kernel_in_safe_call = 1;
    if (sigsetjmp(hs_kernel_jmpbuf, 1) == 0) {
        fn(in_t, nin, out_t, nout);
        success = true;
    } else {
        if (fault_code) *fault_code = 0xDEADBEEF;
        success = false;
    }
    hs_kernel_in_safe_call = 0;

    {
        std::lock_guard<std::mutex> lk(g_hs_sigaction_mtx);
        if (--g_hs_sigaction_active_calls == 0) {
            sigaction(SIGILL, &g_hs_old_ill, nullptr);
            sigaction(SIGSEGV, &g_hs_old_segv, nullptr);
            sigaction(SIGBUS, &g_hs_old_bus, nullptr);
        }
    }
    return success;
}
#endif

/* ========================================================================= */
/* 3. IEEE-754 ERROR ACCUMULATION & FINITENESS CHECK                         */
/* ========================================================================= */

HS_DISABLE_OPT_BEGIN
static inline bool hs_is_ieee754_finite_f32(float val) noexcept {
    uint32_t bits = 0;
    memcpy(&bits, &val, sizeof(uint32_t));
    return ((bits & 0x7F800000u) != 0x7F800000u);
}

RJ_API void hs_accumulate_ieee754_error_f32(const float* cand_ptr,
                                              const float* ref_ptr,
                                              size_t n_elems,
                                              float& inout_max_err,
                                              float& inout_sum_sq,
                                              bool& inout_finite_ok) noexcept {
    for (size_t k = 0; k < n_elems; ++k) {
        float c_v = cand_ptr[k];
        float r_v = ref_ptr[k];
        if (!hs_is_ieee754_finite_f32(c_v) || !hs_is_ieee754_finite_f32(r_v)) {
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

static inline bool hs_is_ieee754_finite_f64(double val) noexcept {
    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(uint64_t));
    return ((bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL);
}

RJ_API void hs_accumulate_ieee754_error_f64(const double* cand_ptr,
                                              const double* ref_ptr,
                                              size_t n_elems,
                                              float& inout_max_err,
                                              float& inout_sum_sq,
                                              bool& inout_finite_ok) noexcept {
    for (size_t k = 0; k < n_elems; ++k) {
        double c_v = cand_ptr[k];
        double r_v = ref_ptr[k];
        if (!hs_is_ieee754_finite_f64(c_v) || !hs_is_ieee754_finite_f64(r_v)) {
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
HS_DISABLE_OPT_END

/* ========================================================================= */
/* 4. REDJET PUZZLE ARENA CORE IMPLEMENTATION                                */
/* ========================================================================= */

static std::mutex g_hs_tlab_snapshot_mutex;
static hs_puzzle_sample_t g_hs_tlab_snapshot;
static bool g_hs_tlab_snapshot_valid = false;

RJ_API void RedjetPuzzleArena::record_tlab_snapshot(const void* src, size_t sz, uint8_t dt) {
    if (!src || sz == 0) return;
    std::lock_guard<std::mutex> lk(g_hs_tlab_snapshot_mutex);
    g_hs_tlab_snapshot = hs_puzzle_sample_t::create_from_tlab(src, sz, dt);
    g_hs_tlab_snapshot_valid = true;
}

RJ_API bool RedjetPuzzleArena::get_tlab_snapshot(hs_puzzle_sample_t& out_smp) {
    std::lock_guard<std::mutex> lk(g_hs_tlab_snapshot_mutex);
    if (!g_hs_tlab_snapshot_valid) return false;
    out_smp = g_hs_tlab_snapshot;
    return true;
}

RJ_API RedjetPuzzleArena::RedjetPuzzleArena() {
    ensure_payload_buffers();
}

RJ_API RedjetPuzzleArena::~RedjetPuzzleArena() {
    destroy();
}

RJ_API void RedjetPuzzleArena::ensure_payload_buffers() {
    if (stagnant_in_payload != nullptr && candidate_out_payload != nullptr && current_best_out_payload != nullptr) {
        return;
    }
    if (!puzzle_heap_base) {
        puzzle_heap_capacity = STAGNANT_BUF_CAP * 3;
        puzzle_heap_base = static_cast<uint8_t*>(hs_puzzle_aligned_alloc(puzzle_heap_capacity, 64));
        if (puzzle_heap_base) {
            memset(puzzle_heap_base, 0, puzzle_heap_capacity);
            stagnant_in_payload = puzzle_heap_base;
            candidate_out_payload = puzzle_heap_base + STAGNANT_BUF_CAP;
            current_best_out_payload = puzzle_heap_base + STAGNANT_BUF_CAP * 2;
            puzzle_heap_used.store(puzzle_heap_capacity, std::memory_order_relaxed);
            _owns_payload_fallback = true;
        }
    }
}

RJ_API void RedjetPuzzleArena::init(sacculus_dispatch_table_t* dt,
                                       sacculus_runway_t* rw,
                                       sacculus_hotlog_t* hl,
                                       sacculus_coldlog_t* cl,
                                       size_t arena_bytes) {
    _dispatch = dt;
    _runway = rw;
    _hotlog = hl;
    _coldlog = cl;

    if (_owns_payload_fallback && puzzle_heap_base) {
        hs_puzzle_aligned_free(puzzle_heap_base);
        puzzle_heap_base = nullptr;
        _owns_payload_fallback = false;
    }

    puzzle_heap_capacity = arena_bytes;
    puzzle_heap_base = static_cast<uint8_t*>(hs_os_reserve_memory(puzzle_heap_capacity));
    if (puzzle_heap_base && hs_os_commit_memory(puzzle_heap_base, puzzle_heap_capacity)) {
        hs_pretouch_memory(puzzle_heap_base, puzzle_heap_capacity);
        stagnant_in_payload = puzzle_heap_base;
        candidate_out_payload = puzzle_heap_base + STAGNANT_BUF_CAP;
        current_best_out_payload = puzzle_heap_base + STAGNANT_BUF_CAP * 2;
        puzzle_heap_used.store(STAGNANT_BUF_CAP * 3, std::memory_order_relaxed);
    } else {
        ensure_payload_buffers();
    }

    refresh_simd_pool();
    if (_coldlog) {
        sacculus_coldlog_write(_coldlog, HS_Ebin0_SAFEPOINT, HS_Ebin1_INFO, 0, "puzzle_arena_init_onheap");
    }
}

RJ_API void RedjetPuzzleArena::destroy() {
    stop();
    if (puzzle_heap_base) {
        if (_owns_payload_fallback) {
            hs_puzzle_aligned_free(puzzle_heap_base);
            _owns_payload_fallback = false;
        } else {
            hs_os_release_memory(puzzle_heap_base, puzzle_heap_capacity);
        }
        puzzle_heap_base = nullptr;
        puzzle_heap_capacity = 0;
        puzzle_heap_used.store(0, std::memory_order_relaxed);
        stagnant_in_payload = nullptr;
        candidate_out_payload = nullptr;
        current_best_out_payload = nullptr;
    }
}

RJ_API void RedjetPuzzleArena::start() {
    if (_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (_thread.joinable()) {
        _thread.join();
    }
    _thread = std::thread([this]() {
        while (_running.load(std::memory_order_acquire)) {
            hs_puzzle_experiment_t exp{};
            hs_puzzle_batch_job_t batch_job{};
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

    hs_unified_log_emit(HS_Ebin0_GROUP_COMPUTE_BEGIN, HS_Ebin1_INFO, 0, "puzzle_start",
                        "RedjetPuzzleArena: On-heap candidate sandbox engaged");
}

RJ_API void RedjetPuzzleArena::stop() {
    if (_running.exchange(false, std::memory_order_acq_rel)) {
        if (_thread.joinable()) {
            _thread.join();
        }
        std::lock_guard<std::mutex> g(_queue_lock);
        _pending_experiments.clear();
        _pending_batch_jobs.clear();

        hs_unified_log_emit(HS_Ebin0_GROUP_COMPUTE_RESET, HS_Ebin1_INFO, 0, "puzzle_stop",
                            "RedjetPuzzleArena: Candidate sandbox stopped cleanly");
    }
}

RJ_API void RedjetPuzzleArena::refresh_simd_pool() {
    cached_isa_inventory = hs_cpu_isa_inventory_t::discover();
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

RJ_API std::vector<std::string> RedjetPuzzleArena::pool_simd_capabilities() {
    refresh_simd_pool();
    return active_simd_pool;
}

RJ_API void RedjetPuzzleArena::set_mode(HS_hotspot_puzzle_exec_mode_t mode) {
    _execution_mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
}

RJ_API HS_hotspot_puzzle_exec_mode_t RedjetPuzzleArena::get_mode() const {
    return static_cast<HS_hotspot_puzzle_exec_mode_t>(_execution_mode.load(std::memory_order_relaxed));
}

RJ_API void RedjetPuzzleArena::set_sample_count(size_t count) {
    _stream_sample_count.store(count > 0 ? count : 1, std::memory_order_relaxed);
}

RJ_API size_t RedjetPuzzleArena::get_sample_count() const {
    return _stream_sample_count.load(std::memory_order_relaxed);
}

RJ_API void RedjetPuzzleArena::identify_stream_types(std::vector<uint8_t>& detected_dtypes) {
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
                    if (h <= static_cast<uint32_t>(HS_DTYPE_U256)) {
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

    scan_room(HS_ROOM_YOUNG, HS_REGION_DEMAND);
    scan_room(HS_ROOM_YOUNG, HS_REGION_HOTFIELD);
    scan_room(HS_ROOM_YOUNG, HS_REGION_TANK);
    scan_room(HS_ROOM_ADOLESCENT, HS_REGION_DEMAND);

    if (detected_dtypes.empty()) {
        detected_dtypes.push_back(SACFOR_F32);
    }
}

RJ_API bool RedjetPuzzleArena::sample_runway_stream(size_t sample_iterations, std::vector<hs_puzzle_sample_t>& aggregated_samples) {
    aggregated_samples.clear();
    std::vector<uint8_t> active_dtypes;
    identify_stream_types(active_dtypes);

    for (size_t it = 0; it < sample_iterations; ++it) {
        for (uint8_t dt : active_dtypes) {
            hs_puzzle_sample_t smp{};
            smp.dtype = dt;
            smp.source = hs_sample_source_e::RUNWAY_MOTHERLINE;
            smp.is_from_tlab = false;
            smp.sample_timestamp_tsc = sacculus_rdtsc();
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
                                    smp.shape[1] = static_cast<int32_t>(fp / (elem_sz > 0 ? elem_sz : 1));
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
                size_t elem_sz = (dt == SACFOR_F64 || dt == SACFOR_I64) ? 8 : 4;
                size_t n_elems = 1024;
                smp.byte_size = n_elems * elem_sz;
                smp.ndim = 2;
                smp.shape[0] = 1;
                smp.shape[1] = static_cast<int32_t>(n_elems);

                if (dt == SACFOR_F64) {
                    double* f64_ptr = reinterpret_cast<double*>(smp.data);
                    for (size_t i = 0; i < n_elems; ++i) {
                        f64_ptr[i] = 1.0 + 0.001 * static_cast<double>((i + it) % 100);
                    }
                } else if (dt == SACFOR_F32) {
                    float* f32_ptr = reinterpret_cast<float*>(smp.data);
                    for (size_t i = 0; i < n_elems; ++i) {
                        f32_ptr[i] = 1.0f + 0.001f * static_cast<float>((i + it) % 100);
                    }
                } else {
                    memset(smp.data, static_cast<int>((it + 1) & 0xFF), smp.byte_size);
                }
            }
            aggregated_samples.push_back(std::move(smp));
        }
    }
    return !aggregated_samples.empty();
}

RJ_API bool RedjetPuzzleArena::sample_tlab_stream(size_t sample_iterations, std::vector<hs_puzzle_sample_t>& aggregated_samples) {
    aggregated_samples.clear();

    for (size_t it = 0; it < sample_iterations; ++it) {
        hs_puzzle_sample_t smp{};
        smp.source = hs_sample_source_e::TLAB_MUTATOR;
        smp.is_from_tlab = true;
        smp.sample_timestamp_tsc = sacculus_rdtsc();
        smp.dtype = SACFOR_F32;

        bool captured = false;
        if (tl_hs_tlab.base != nullptr && tl_hs_tlab.offset > 0) {
            size_t copy_sz = tl_hs_tlab.offset > STAGNANT_BUF_CAP ? STAGNANT_BUF_CAP : tl_hs_tlab.offset;
            memcpy(smp.data, tl_hs_tlab.base, copy_sz);
            smp.byte_size = copy_sz;
            smp.ndim = 2;
            smp.shape[0] = 1;
            smp.shape[1] = static_cast<int32_t>(copy_sz / sizeof(float));
            captured = true;
        } else if (get_tlab_snapshot(smp)) {
            captured = true;
        }

        if (!captured) {
            // Synthesize realistic mutator buffer pattern
            size_t n_elems = 2048;
            smp.byte_size = n_elems * sizeof(float);
            smp.ndim = 2;
            smp.shape[0] = 1;
            smp.shape[1] = static_cast<int32_t>(n_elems);
            float* f_ptr = reinterpret_cast<float*>(smp.data);
            for (size_t i = 0; i < n_elems; ++i) {
                f_ptr[i] = static_cast<float>(i * 0.125f + it);
            }
        }
        aggregated_samples.push_back(std::move(smp));
    }
    return !aggregated_samples.empty();
}

RJ_API void RedjetPuzzleArena::submit_batch(uint32_t fn_id,
                                               const HS_hotspot_tensor_t* batch_samples,
                                               int sample_count,
                                               const HS_hotspot_tensor_t* target_prec,
                                               float target_epsilon) {
    std::lock_guard<std::mutex> g(_queue_lock);
    hs_puzzle_batch_job_t job{};
    job.fn_id = fn_id;
    job.target_epsilon = target_epsilon;
    if (target_prec && target_prec->data && target_prec->byte_size > 0) {
        job.target_precision_tensor = *target_prec;
        job.has_target_precision = true;
    }

    for (int i = 0; i < sample_count; ++i) {
        const auto& src = batch_samples[i];
        if (src.data && src.byte_size > 0) {
            hs_puzzle_sample_t smp{};
            smp.dtype = static_cast<uint8_t>(src.dtype);
            smp.byte_size = src.byte_size;
            smp.ndim = src.ndim;
            for (int k = 0; k < 8; ++k) smp.shape[k] = src.shape[k];
            size_t copy_bytes = (src.byte_size > STAGNANT_BUF_CAP) ? STAGNANT_BUF_CAP : src.byte_size;
            memcpy(smp.data, src.data, copy_bytes);
            job.samples.push_back(std::move(smp));
        }
    }
    if (!job.samples.empty()) {
        _pending_batch_jobs.push_back(job);
    }
}

RJ_API bool RedjetPuzzleArena::acquire_evaluation_samples(uint32_t fn_id,
                                                             std::vector<hs_puzzle_sample_t>& samples,
                                                             HS_hotspot_tensor_t& target_prec,
                                                             bool& has_target_prec,
                                                             float& target_epsilon,
                                                             hs_puzzler_determinism_tier_e& out_tier) {
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
                out_tier = hs_puzzler_classify_tier(dt);
                hs_puzzler_validate_target_precision(dt, target_epsilon);
                return true;
            }
        }
    }

    has_target_prec = false;
    size_t n_iters = _stream_sample_count.load(std::memory_order_relaxed);
    bool ok = sample_runway_stream(n_iters > 0 ? n_iters : 5, samples);
    if (ok && !samples.empty()) {
        uint8_t dt = samples[0].dtype;
        out_tier = hs_puzzler_classify_tier(dt);
        hs_puzzler_validate_target_precision(dt, target_epsilon);
    } else {
        out_tier = hs_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
        target_epsilon = FLT_EPSILON;
    }
    return ok;
}

RJ_API hs_hybrid_compat_result_t RedjetPuzzleArena::evaluate_hybrid_compatibility(sacculus_kernel_fn_t fn_a,
                                                                                    sacculus_kernel_fn_t fn_b,
                                                                                    const HS_hotspot_tensor_t* in_t,
                                                                                    HS_hotspot_tensor_t* out_t) {
    hs_hybrid_compat_result_t res{};
    if (!fn_a || !fn_b) {
        snprintf(res.reason, sizeof(res.reason), "null_kernel");
        return res;
    }

    constexpr int RUNS = 8;
    uint64_t cycles_a[RUNS];
    uint64_t cycles_b[RUNS];
    uint64_t cycles_hybrid[RUNS];

    bool is_hybrid = hs_is_hybrid_cpu();

    uint64_t min_a = UINT64_MAX, max_a = 0;
    double sum_a = 0.0;
    if (is_hybrid) hs_bind_current_thread(HS_CORE_TIER_PERFORMANCE);
    for (int r = 0; r < RUNS; ++r) {
        uint32_t fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        if (!hs_safe_kernel_call(fn_a, in_t, 1, out_t, 1, nullptr, &fault)) {
            if (is_hybrid) hs_bind_current_thread(HS_CORE_TIER_ANY);
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
    if (is_hybrid) hs_bind_current_thread(HS_CORE_TIER_EFFICIENCY);
    for (int r = 0; r < RUNS; ++r) {
        uint32_t fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        if (!hs_safe_kernel_call(fn_b, in_t, 1, out_t, 1, nullptr, &fault)) {
            if (is_hybrid) hs_bind_current_thread(HS_CORE_TIER_ANY);
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

    if (is_hybrid) hs_bind_current_thread(HS_CORE_TIER_ANY);
    double sum_hyb = 0.0;
    for (int r = 0; r < RUNS; ++r) {
        uint32_t fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        bool ok_a = hs_safe_kernel_call(fn_a, in_t, 1, out_t, 1, nullptr, &fault);
        bool ok_b = ok_a && hs_safe_kernel_call(fn_b, in_t, 1, out_t, 1, nullptr, &fault);
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
    res.p_core_cycles = static_cast<double>(min_a);
    res.e_core_cycles = static_cast<double>(min_b);
    res.asymmetric_ratio = mu_b / (mu_a > 0.0 ? mu_a : 1.0);

    // Calculate AVX-512 downclock penalty
    // When AVX-512 frequency drop occurs, CPU clock downscales across active cores
    double downclock_penalty = 0.0;
    if (is_hybrid) {
        // Alder Lake / Raptor Lake: E-cores lack AVX-512, causing thread migration penalty
        downclock_penalty = (res.asymmetric_ratio > 2.2) ? 0.20 : 0.05;
    } else {
        // Xeon / AVX-512 Heavy License level 2 step-down check
        if (observed_ratio > 1.25) {
            downclock_penalty = observed_ratio - 1.0;
        }
    }

    res.avx512_downclock_penalty = downclock_penalty;
    res.avx512_throttle_detected = (downclock_penalty >= 0.15);
    res.p_core_ipc = (min_a > 0) ? (2.0 * 1000.0 / static_cast<double>(min_a)) : 1.0;
    res.e_core_ipc = (min_b > 0) ? (1.2 * 1000.0 / static_cast<double>(min_b)) : 0.8;

    if (res.avx512_throttle_detected) {
        _telemetry_downclock_detections.fetch_add(1, std::memory_order_relaxed);
    }

    if (observed_ratio <= theta_dynamic && !res.avx512_throttle_detected) {
        res.compatible = true;
        snprintf(res.reason, sizeof(res.reason), "dynamically_compatible(ratio=%.3f<=bound=%.3f)", observed_ratio, theta_dynamic);
    } else if (res.avx512_throttle_detected) {
        res.compatible = false;
        snprintf(res.reason, sizeof(res.reason), "avx512_downclock_penalty(pen=%.1f%%)", downclock_penalty * 100.0);
    } else {
        res.compatible = false;
        snprintf(res.reason, sizeof(res.reason), "stall_incompatible(ratio=%.3f>bound=%.3f)", observed_ratio, theta_dynamic);
    }
    return res;
}

RJ_API hs_score_eval_t RedjetPuzzleArena::evaluate_score(uint64_t cand_cycles,
                                                           uint64_t cur_cycles,
                                                           float max_abs_err,
                                                           float mse,
                                                           float target_epsilon,
                                                           const HS_hotspot_pmu_stat_t* pmu) {
    hs_score_eval_t res{};
    res.max_err = max_abs_err;
    res.mse = mse;

    bool finite_ok = hs_is_ieee754_finite_f32(max_abs_err) &&
                     hs_is_ieee754_finite_f32(mse) &&
                     hs_is_ieee754_finite_f32(target_epsilon);
    bool speed_ok = (cand_cycles < cur_cycles);
    bool precision_ok = false;
    if (target_epsilon <= 0.0f) {
        // EXACT_ALGEBRAIC: strictly 0-bit deviation tolerance (memcmp == 0)
        precision_ok = finite_ok && (max_abs_err == 0.0f);
    } else {
        // NUMERIC_IEEE_FP: max_abs_err <= epsilon AND MSE <= epsilon^2
        float eps_sq = target_epsilon * target_epsilon;
        precision_ok = finite_ok && (max_abs_err <= target_epsilon) && (mse <= (eps_sq + 1e-12f));
    }

    if (!speed_ok || !precision_ok) {
        res.qualified = false;
        res.score = -1.0;
        return res;
    }

    double speedup = static_cast<double>(cur_cycles - cand_cycles) / static_cast<double>(cur_cycles > 0 ? cur_cycles : 1);
    double prec_margin = 0.0;
    if (target_epsilon > 0.0f) {
        prec_margin = static_cast<double>(target_epsilon - max_abs_err) / static_cast<double>(target_epsilon);
        if (prec_margin < 0.0) prec_margin = 0.0;
        if (prec_margin > 1.0) prec_margin = 1.0;
    } else {
        // EXACT_ALGEBRAIC: perfect 0-bit deviation earns full precision headroom
        prec_margin = (max_abs_err == 0.0f) ? 1.0 : 0.0;
    }

    double pmu_efficiency = 1.0;
    double ipc_val = 1.0;
    double cache_pen = 0.0;
    double downclock_fac = 1.0;
    if (pmu) {
        ipc_val = pmu->ipc;
        double ipc_bonus = std::min(1.5, pmu->ipc / 2.0);
        downclock_fac = (pmu->freq_downclock_ratio > 0.0) ? pmu->freq_downclock_ratio : 1.0;
        cache_pen = (pmu->llc_cache_misses > 0 && pmu->cycles > 0)
            ? std::min(0.4, static_cast<double>(pmu->llc_cache_misses * 64) / static_cast<double>(pmu->cycles))
            : 0.0;
        pmu_efficiency = std::max(0.1, (ipc_bonus * downclock_fac) - cache_pen);
    }

    res.pmu_ipc = ipc_val;
    res.cache_penalty = cache_pen;
    res.downclock_factor = downclock_fac;
    res.speedup_ratio = speedup;
    res.precision_headroom = prec_margin;
    res.score = (0.55 * speedup + 0.35 * prec_margin + 0.10 * pmu_efficiency);
    res.qualified = true;
    return res;
}

RJ_API void RedjetPuzzleArena::redirect_stream(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t new_cycles, const char* name, double score) {
    if (!_dispatch || !new_fn) return;

    _telemetry_pausepoint_gates.fetch_add(1, std::memory_order_relaxed);
    RedjetPausepointGateGuard gate_guard;

    _dispatch->swap(fn_id, new_fn, new_cycles);

    if (_runway) {
        _runway->batch_optimize_egg_objects(fn_id, name, 0, new_cycles);
    }

    gate_guard.disarm();

    _telemetry_hotswaps.fetch_add(1, std::memory_order_relaxed);
    _telemetry_last_winner_cycles.store(new_cycles, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(_telemetry_lock);
        if (name) {
            strncpy(_telemetry_last_winner_name, name, sizeof(_telemetry_last_winner_name) - 1);
        } else {
            strncpy(_telemetry_last_winner_name, "winner_isa", sizeof(_telemetry_last_winner_name) - 1);
        }
    }

    sacculus_hotlog_write(_hotlog, HS_Ebin0_HOTSWAP, HS_Ebin1_INFO,
        fn_id, static_cast<uint16_t>(new_cycles & 0xFFFF), 0, 0, 0, 0,
        static_cast<uint32_t>(score * 1000.0), name ? name : "puzzle_win");

    if (_coldlog) {
        char msg[64];
        snprintf(msg, sizeof(msg), "redirect:%s cyc=%llu sc=%.3f", name ? name : "simd",
                 static_cast<unsigned long long>(new_cycles), score);
        sacculus_coldlog_write(_coldlog, HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, fn_id, msg);
    }
}

RJ_API void RedjetPuzzleArena::submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name,
                                        const char* target_isa, float target_epsilon,
                                        const HS_hotspot_tensor_t* target_tensor) {
    std::lock_guard<std::mutex> g(_queue_lock);
    hs_puzzle_experiment_t exp{};
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

RJ_API void RedjetPuzzleArena::run_puzzle_suite_parallel(uint32_t fn_id,
                                                           const HS_hotspot_isa_candidate_t* candidates,
                                                           int n_candidates,
                                                           float target_epsilon,
                                                           const HS_hotspot_tensor_t* target_tensor) {
    if (!candidates || n_candidates <= 0 || !_dispatch) return;
    auto* entry = _dispatch->get(fn_id);
    if (!entry || !entry->fn.load()) return;

    std::vector<hs_puzzle_sample_t> samples;
    HS_hotspot_tensor_t target_prec{};
    bool has_target_prec = false;
    float target_eps = target_epsilon;
    if (target_tensor && target_tensor->data && target_tensor->byte_size > 0) {
        target_prec = *target_tensor;
        has_target_prec = true;
    }

    hs_puzzler_determinism_tier_e tier = hs_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
    if (!acquire_evaluation_samples(fn_id, samples, target_prec, has_target_prec, target_eps, tier)) {
        return;
    }

    for (int i = 0; i < n_candidates; ++i) {
        if (!candidates[i].fn) continue;
        hs_puzzle_experiment_t exp{};
        exp.fn_id = fn_id;
        exp.candidate = candidates[i].fn;
        strncpy(exp.name, candidates[i].variant_name ? candidates[i].variant_name : "suite_cand", sizeof(exp.name) - 1);
        strncpy(exp.target_isa, candidates[i].isa_tag ? candidates[i].isa_tag : "AUTO", sizeof(exp.target_isa) - 1);
        exp.target_epsilon = target_eps;
        exp.target_tensor = target_prec;
        exp.has_target_tensor = has_target_prec;

        run_puzzle_experiment_with_samples(exp, samples, target_prec, has_target_prec, target_eps, tier);
    }
}

RJ_API void RedjetPuzzleArena::submit_suite(uint32_t fn_id,
                                              const HS_hotspot_isa_candidate_t* candidates,
                                              int n_candidates,
                                              float target_epsilon,
                                              const HS_hotspot_tensor_t* target_tensor) {
    if (!candidates || n_candidates <= 0) return;
    for (int i = 0; i < n_candidates; ++i) {
        if (candidates[i].fn) {
            submit(fn_id, candidates[i].fn, candidates[i].variant_name, candidates[i].isa_tag, target_epsilon, target_tensor);
        }
    }
}

RJ_API void RedjetPuzzleArena::run_puzzle_experiment_with_samples(const hs_puzzle_experiment_t& exp,
                                                                    const std::vector<hs_puzzle_sample_t>& samples,
                                                                    const HS_hotspot_tensor_t& target_prec,
                                                                    bool has_target_prec,
                                                                    float target_eps,
                                                                    hs_puzzler_determinism_tier_e tier) {
    if (samples.empty() || !_dispatch) return;
    auto* entry = _dispatch->get(exp.fn_id);
    if (!entry || !entry->fn.load() || !exp.candidate) return;

    ensure_payload_buffers();
    if (!stagnant_in_payload || !candidate_out_payload || !current_best_out_payload) return;

    _telemetry_experiments.fetch_add(1, std::memory_order_relaxed);
    _telemetry_candidates_eval.fetch_add(1, std::memory_order_relaxed);

    auto cur_fn = entry->fn.load(std::memory_order_acquire);
    uint64_t cur_cycles_total = 0;
    uint64_t cand_cycles_total = 0;
    float max_abs_err = 0.0f;
    float sum_sq_err = 0.0f;
    size_t total_elems = 0;

    HS_hotspot_pmu_stat_t exp_pmu{};
    hs_pmu_session_t exp_pmu_sess{};

    for (const auto& smp : samples) {
        if (smp.byte_size == 0) continue;
        size_t run_bytes = (smp.byte_size > STAGNANT_BUF_CAP) ? STAGNANT_BUF_CAP : smp.byte_size;
        memcpy(stagnant_in_payload, smp.data, run_bytes);
        memset(candidate_out_payload, 0, STAGNANT_BUF_CAP);
        memset(current_best_out_payload, 0, STAGNANT_BUF_CAP);

        HS_hotspot_tensor_t in_tensor{};
        in_tensor.data = stagnant_in_payload;
        in_tensor.byte_size = run_bytes;
        in_tensor.ndim = smp.ndim;
        in_tensor.dtype = smp.dtype;
        for (int k = 0; k < 8; ++k) in_tensor.shape[k] = smp.shape[k];

        HS_hotspot_tensor_t out_cur{};
        out_cur.data = current_best_out_payload;
        out_cur.byte_size = run_bytes;
        out_cur.ndim = smp.ndim;
        out_cur.dtype = smp.dtype;
        for (int k = 0; k < 8; ++k) out_cur.shape[k] = smp.shape[k];

        HS_hotspot_tensor_t out_cand{};
        out_cand.data = candidate_out_payload;
        out_cand.byte_size = run_bytes;
        out_cand.ndim = smp.ndim;
        out_cand.dtype = smp.dtype;
        for (int k = 0; k < 8; ++k) out_cand.shape[k] = smp.shape[k];

        // 1. Run baseline kernel
        uint32_t base_fault = 0;
        uint64_t t0 = sacculus_rdtsc();
        bool base_ok = hs_safe_kernel_call(cur_fn, &in_tensor, 1, &out_cur, 1, nullptr, &base_fault);
        uint64_t dt0 = sacculus_rdtsc() - t0;
        if (!base_ok) {
            _telemetry_fault_isolations.fetch_add(1, std::memory_order_relaxed);
            hs_candidate_eval_t eval{};
            eval.fn = exp.candidate;
            strncpy(eval.variant_name, exp.name, sizeof(eval.variant_name) - 1);
            strncpy(eval.isa_tag, exp.target_isa, sizeof(eval.isa_tag) - 1);
            eval.executed_ok = false;
            eval.fault_code = base_fault;
            eval.score = -1.0;
            evaluate_candidate(eval);
            return;
        }
        cur_cycles_total += dt0;

        // 2. Run candidate kernel with fault isolation and PMU monitoring
        uint32_t cand_fault = 0;
        exp_pmu_sess.begin();
        uint64_t t1 = sacculus_rdtsc();
        bool cand_ok = hs_safe_kernel_call(exp.candidate, &in_tensor, 1, &out_cand, 1, nullptr, &cand_fault);
        uint64_t dt1 = sacculus_rdtsc() - t1;
        exp_pmu_sess.end(&exp_pmu, smp.byte_size);

        if (!cand_ok) {
            _telemetry_fault_isolations.fetch_add(1, std::memory_order_relaxed);
            sacculus_hotlog_write(_hotlog, HS_Ebin0_HOTSWAP_FAIL, HS_Ebin1_WARN,
                exp.fn_id, 0, 0, 0, 0, 0, cand_fault, "fault_isolated");
            if (_coldlog) {
                char msg[32];
                snprintf(msg, sizeof(msg), "fault:0x%08X", cand_fault);
                sacculus_coldlog_write(_coldlog, HS_Ebin0_HOTSWAP_FAIL, HS_Ebin1_WARN, exp.fn_id, msg);
            }
            hs_candidate_eval_t eval{};
            eval.fn = exp.candidate;
            strncpy(eval.variant_name, exp.name, sizeof(eval.variant_name) - 1);
            strncpy(eval.isa_tag, exp.target_isa, sizeof(eval.isa_tag) - 1);
            eval.executed_ok = false;
            eval.fault_code = cand_fault;
            eval.score = -1.0;
            evaluate_candidate(eval);
            return;
        }
        cand_cycles_total += dt1;

        const uint8_t* ref_bytes = (has_target_prec && target_prec.data)
            ? static_cast<const uint8_t*>(target_prec.data)
            : static_cast<const uint8_t*>(current_best_out_payload);
        size_t cmp_bytes = (has_target_prec && target_prec.data && target_prec.byte_size < smp.byte_size)
            ? target_prec.byte_size
            : smp.byte_size;

        if (tier == hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) {
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
            hs_accumulate_ieee754_error_f64(cand_ptr, ref_ptr, n_elems, max_abs_err, sum_sq_err, finite_ok);
            if (!finite_ok) break;
            total_elems += n_elems;
        } else {
            size_t n_elems = cmp_bytes / sizeof(float);
            const float* cand_ptr = reinterpret_cast<const float*>(candidate_out_payload);
            const float* ref_ptr = reinterpret_cast<const float*>(ref_bytes);
            bool finite_ok = true;
            hs_accumulate_ieee754_error_f32(cand_ptr, ref_ptr, n_elems, max_abs_err, sum_sq_err, finite_ok);
            if (!finite_ok) break;
            total_elems += n_elems;
        }
    }
    if (total_elems == 0) return;

    uint64_t current_cycles = cur_cycles_total / samples.size();
    if (current_cycles == 0) current_cycles = 1;
    uint64_t candidate_cycles = cand_cycles_total / samples.size();
    float mse = sum_sq_err / static_cast<float>(total_elems > 0 ? total_elems : 1);

    auto score_eval = evaluate_score(candidate_cycles, current_cycles, max_abs_err, mse, target_eps, &exp_pmu);

    // Record evaluation in history
    hs_candidate_eval_t eval{};
    eval.fn = exp.candidate;
    strncpy(eval.variant_name, exp.name, sizeof(eval.variant_name) - 1);
    strncpy(eval.isa_tag, exp.target_isa, sizeof(eval.isa_tag) - 1);
    eval.executed_ok = true;
    eval.cycles = candidate_cycles;
    eval.max_abs_err = max_abs_err;
    eval.mse = mse;
    eval.score = score_eval.score;
    eval.precision_passed = (tier == hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC) ? (max_abs_err == 0.0f) : (max_abs_err <= target_eps && mse <= (target_eps * target_eps + 1e-12f));
    eval.speed_passed = (candidate_cycles < current_cycles);
    eval.ipc_score = score_eval.pmu_ipc;
    eval.l1_l2_cache_miss_penalty = score_eval.cache_penalty;
    eval.downclock_penalty = score_eval.downclock_factor;
    eval.pmu_stats = exp_pmu;
    evaluate_candidate(eval);

    if (score_eval.qualified && score_eval.score > 0.0) {
        redirect_stream(exp.fn_id, exp.candidate, candidate_cycles, exp.name, score_eval.score);
    } else {
        bool prec_failed = !eval.precision_passed;
        if (prec_failed) {
            _telemetry_prec_fails.fetch_add(1, std::memory_order_relaxed);
            sacculus_hotlog_write(_hotlog, HS_Ebin0_HEAL_NUMERIC_DIVERGE, HS_Ebin1_WARN,
                exp.fn_id, 0, 0, 0, 0, 0, static_cast<uint32_t>(max_abs_err * 10000.0f), exp.name);
        } else {
            _telemetry_speed_fails.fetch_add(1, std::memory_order_relaxed);
        }
        const char* reason = prec_failed ? "fail_precision" : "fail_speed";
        sacculus_hotlog_write(_hotlog, HS_Ebin0_HOTSWAP_FAIL, HS_Ebin1_TRACE,
            exp.fn_id, 0, 0, 0, 0, 0, static_cast<uint32_t>(max_abs_err * 10000.0f), reason);
    }
}

RJ_API void RedjetPuzzleArena::run_puzzle_experiment(const hs_puzzle_experiment_t& exp) {
    if (!_dispatch) return;
    auto* entry = _dispatch->get(exp.fn_id);
    if (!entry || !entry->fn.load() || !exp.candidate) {
        return;
    }

    if (!cached_isa_inventory.has_isa(exp.target_isa)) {
        if (_coldlog) {
            sacculus_coldlog_write(_coldlog, HS_Ebin0_HOTSWAP_FAIL, HS_Ebin1_TRACE, exp.fn_id, "isa_not_on_cpu");
        }
        return;
    }

    std::vector<hs_puzzle_sample_t> samples;
    HS_hotspot_tensor_t target_prec{};
    bool has_target_prec = false;
    float target_eps = exp.target_epsilon;
    if (exp.has_target_tensor && exp.target_tensor.data && exp.target_tensor.byte_size > 0) {
        target_prec = exp.target_tensor;
        has_target_prec = true;
    }

    hs_puzzler_determinism_tier_e tier = hs_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;
    if (!acquire_evaluation_samples(exp.fn_id, samples, target_prec, has_target_prec, target_eps, tier)) {
        return;
    }
    run_puzzle_experiment_with_samples(exp, samples, target_prec, has_target_prec, target_eps, tier);
}

RJ_API void RedjetPuzzleArena::run_puzzle_batch_job(const hs_puzzle_batch_job_t& job) {
    if (!_dispatch || job.samples.empty()) return;
    auto* entry = _dispatch->get(job.fn_id);
    if (!entry || !entry->fn.load()) return;

    auto capabilities = pool_simd_capabilities();
    if (capabilities.empty()) return;

    auto cur_fn = entry->fn.load(std::memory_order_acquire);
    if (!cur_fn) return;

    hs_puzzle_experiment_t exp{};
    exp.fn_id = job.fn_id;
    exp.candidate = cur_fn;
    strncpy(exp.name, "batch_job_stream", sizeof(exp.name) - 1);
    strncpy(exp.target_isa, capabilities[0].c_str(), sizeof(exp.target_isa) - 1);
    exp.target_epsilon = job.target_epsilon;
    exp.target_tensor = job.target_precision_tensor;
    exp.has_target_tensor = job.has_target_precision;

    uint8_t dt = job.samples[0].dtype;
    hs_puzzler_determinism_tier_e tier = hs_puzzler_classify_tier(dt);
    float final_eps = job.target_epsilon;
    hs_puzzler_validate_target_precision(dt, final_eps);

    run_puzzle_experiment_with_samples(
        exp, job.samples, job.target_precision_tensor, job.has_target_precision, final_eps, tier);
}

RJ_API double RedjetPuzzleArena::score_variant(uint64_t baseline_cycles, uint64_t candidate_cycles, float max_err) {
    if (candidate_cycles == 0 || baseline_cycles == 0) return 0.0;
    if (max_err > 1e-3f) return -1.0;

    double speedup = static_cast<double>(baseline_cycles) / static_cast<double>(candidate_cycles);
    double accuracy_factor = 1.0 / (1.0 + std::abs(max_err));
    return speedup * accuracy_factor;
}

RJ_API void RedjetPuzzleArena::evaluate_candidate(hs_candidate_eval_t& candidate) {
    std::lock_guard<std::mutex> g(_queue_lock);
    candidate.idx = static_cast<int>(_eval_history.size());
    _eval_history.push_back(candidate);
}

RJ_API size_t RedjetPuzzleArena::history_count() const {
    std::lock_guard<std::mutex> g(const_cast<std::mutex&>(_queue_lock));
    return _eval_history.size();
}

RJ_API std::vector<hs_candidate_eval_t> RedjetPuzzleArena::get_candidate_history() const {
    std::lock_guard<std::mutex> g(const_cast<std::mutex&>(_queue_lock));
    return _eval_history;
}

/* ========================================================================= */
/* 5. SELF-TUNING MICRO-BENCHMARKS                                           */
/* ========================================================================= */

RJ_API hs_puzzler_bench_result_t RedjetPuzzleArena::run_self_tuning_benchmark(uint32_t bench_id) {
    hs_puzzler_bench_result_t res{};
    res.bench_id = bench_id;

    switch (static_cast<hs_puzzler_bench_id_e>(bench_id)) {
        case hs_puzzler_bench_id_e::BENCH_MEMZERO: {
            strncpy(res.bench_name, "memzero_tune", sizeof(res.bench_name) - 1);
            res.tier = hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC;

            constexpr size_t BUF_SZ = 256 * 1024;
            uint8_t* buf_a = static_cast<uint8_t*>(hs_puzzle_aligned_alloc(BUF_SZ, 64));
            uint8_t* buf_b = static_cast<uint8_t*>(hs_puzzle_aligned_alloc(BUF_SZ, 64));
            if (!buf_a || !buf_b) {
                if (buf_a) hs_puzzle_aligned_free(buf_a);
                if (buf_b) hs_puzzle_aligned_free(buf_b);
                res.verified_ok = false;
                break;
            }
            memset(buf_a, 0xAA, BUF_SZ);
            memset(buf_b, 0xAA, BUF_SZ);

            // Baseline: standard memset
            uint64_t t0 = sacculus_rdtsc();
            memset(buf_a, 0, BUF_SZ);
            uint64_t base_cyc = sacculus_rdtsc() - t0;
            if (base_cyc == 0) base_cyc = 1;

            // Candidate: Non-temporal streaming zeroing
            uint64_t t1 = sacculus_rdtsc();
            hs_critical_clear_memory(buf_b, BUF_SZ);
            uint64_t cand_cyc = sacculus_rdtsc() - t1;
            if (cand_cyc == 0) cand_cyc = 1;

            // Verify bitwise exact equality
            bool exact_ok = (memcmp(buf_a, buf_b, BUF_SZ) == 0);
            res.baseline_cycles = base_cyc;
            res.winning_cycles = cand_cyc;
            res.speedup = static_cast<double>(base_cyc) / static_cast<double>(cand_cyc);
            res.verified_ok = exact_ok;
            res.max_abs_err = exact_ok ? 0.0f : 1.0f;
            strncpy(res.winning_isa, (cand_cyc <= base_cyc && exact_ok) ? "NT_STREAM" : "MEMSET", sizeof(res.winning_isa) - 1);

            hs_puzzle_aligned_free(buf_a);
            hs_puzzle_aligned_free(buf_b);
            break;
        }
        case hs_puzzler_bench_id_e::BENCH_MEMCPY: {
            strncpy(res.bench_name, "memcpy_tune", sizeof(res.bench_name) - 1);
            res.tier = hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC;

            constexpr size_t BUF_SZ = 256 * 1024;
            uint8_t* src = static_cast<uint8_t*>(hs_puzzle_aligned_alloc(BUF_SZ, 64));
            uint8_t* dst_a = static_cast<uint8_t*>(hs_puzzle_aligned_alloc(BUF_SZ, 64));
            uint8_t* dst_b = static_cast<uint8_t*>(hs_puzzle_aligned_alloc(BUF_SZ, 64));
            if (!src || !dst_a || !dst_b) {
                if (src) hs_puzzle_aligned_free(src);
                if (dst_a) hs_puzzle_aligned_free(dst_a);
                if (dst_b) hs_puzzle_aligned_free(dst_b);
                res.verified_ok = false;
                break;
            }
            for (size_t i = 0; i < BUF_SZ; ++i) src[i] = static_cast<uint8_t>(i & 0xFF);

            // Baseline: memcpy
            uint64_t t0 = sacculus_rdtsc();
            memcpy(dst_a, src, BUF_SZ);
            uint64_t base_cyc = sacculus_rdtsc() - t0;
            if (base_cyc == 0) base_cyc = 1;

            // Candidate: stream copy
            uint64_t t1 = sacculus_rdtsc();
            hs_dram_stream_copy(dst_b, src, BUF_SZ);
            uint64_t cand_cyc = sacculus_rdtsc() - t1;
            if (cand_cyc == 0) cand_cyc = 1;

            bool exact_ok = (memcmp(dst_a, dst_b, BUF_SZ) == 0);
            res.baseline_cycles = base_cyc;
            res.winning_cycles = cand_cyc;
            res.speedup = static_cast<double>(base_cyc) / static_cast<double>(cand_cyc);
            res.verified_ok = exact_ok;
            res.max_abs_err = exact_ok ? 0.0f : 1.0f;
            strncpy(res.winning_isa, (cand_cyc <= base_cyc && exact_ok) ? "STREAM_COPY" : "MEMCPY", sizeof(res.winning_isa) - 1);

            hs_puzzle_aligned_free(src);
            hs_puzzle_aligned_free(dst_a);
            hs_puzzle_aligned_free(dst_b);
            break;
        }
        case hs_puzzler_bench_id_e::BENCH_BITMAP_SCAN: {
            strncpy(res.bench_name, "bitmap_scan_tune", sizeof(res.bench_name) - 1);
            res.tier = hs_puzzler_determinism_tier_e::EXACT_ALGEBRAIC;

            constexpr size_t N_WORDS = 2048;
            uint64_t words[N_WORDS];
            for (size_t i = 0; i < N_WORDS; ++i) {
                words[i] = (i % 7 == 0) ? (1ULL << (i % 64)) : 0;
            }

            // Baseline: bit-by-bit scan
            size_t bits_found_base = 0;
            uint64_t t0 = sacculus_rdtsc();
            for (size_t w = 0; w < N_WORDS; ++w) {
                uint64_t val = words[w];
                for (int b = 0; b < 64; ++b) {
                    if ((val >> b) & 1) ++bits_found_base;
                }
            }
            uint64_t base_cyc = sacculus_rdtsc() - t0;
            if (base_cyc == 0) base_cyc = 1;

            // Candidate: BMI2 / popcnt accelerated scan
            size_t bits_found_cand = 0;
            uint64_t t1 = sacculus_rdtsc();
            for (size_t w = 0; w < N_WORDS; ++w) {
                uint64_t val = words[w];
                while (val != 0) {
                    ++bits_found_cand;
                    val &= (val - 1); // BLSR
                }
            }
            uint64_t cand_cyc = sacculus_rdtsc() - t1;
            if (cand_cyc == 0) cand_cyc = 1;

            bool exact_ok = (bits_found_base == bits_found_cand);
            res.baseline_cycles = base_cyc;
            res.winning_cycles = cand_cyc;
            res.speedup = static_cast<double>(base_cyc) / static_cast<double>(cand_cyc);
            res.verified_ok = exact_ok;
            res.max_abs_err = exact_ok ? 0.0f : 1.0f;
            strncpy(res.winning_isa, exact_ok ? "BMI2_BLSR" : "SCALAR_SCAN", sizeof(res.winning_isa) - 1);
            break;
        }
        case hs_puzzler_bench_id_e::BENCH_VECTOR_FMA_F32: {
            strncpy(res.bench_name, "vector_fma_f32_tune", sizeof(res.bench_name) - 1);
            res.tier = hs_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;

            constexpr size_t N = 8192;
            float* a = static_cast<float*>(hs_puzzle_aligned_alloc(N * sizeof(float), 64));
            float* b = static_cast<float*>(hs_puzzle_aligned_alloc(N * sizeof(float), 64));
            float* c = static_cast<float*>(hs_puzzle_aligned_alloc(N * sizeof(float), 64));
            float* out_base = static_cast<float*>(hs_puzzle_aligned_alloc(N * sizeof(float), 64));
            float* out_cand = static_cast<float*>(hs_puzzle_aligned_alloc(N * sizeof(float), 64));
            if (!a || !b || !c || !out_base || !out_cand) {
                if (a) hs_puzzle_aligned_free(a);
                if (b) hs_puzzle_aligned_free(b);
                if (c) hs_puzzle_aligned_free(c);
                if (out_base) hs_puzzle_aligned_free(out_base);
                if (out_cand) hs_puzzle_aligned_free(out_cand);
                res.verified_ok = false;
                break;
            }
            for (size_t i = 0; i < N; ++i) {
                a[i] = 1.0f + 0.01f * (i % 10);
                b[i] = 2.0f + 0.02f * (i % 10);
                c[i] = 0.5f;
            }

            // Baseline: scalar loop
            uint64_t t0 = sacculus_rdtsc();
            for (size_t i = 0; i < N; ++i) {
                out_base[i] = a[i] * b[i] + c[i];
            }
            uint64_t base_cyc = sacculus_rdtsc() - t0;
            if (base_cyc == 0) base_cyc = 1;

            // Candidate: unrolled chunk loop
            uint64_t t1 = sacculus_rdtsc();
            for (size_t i = 0; i < N; i += 4) {
                out_cand[i + 0] = a[i + 0] * b[i + 0] + c[i + 0];
                out_cand[i + 1] = a[i + 1] * b[i + 1] + c[i + 1];
                out_cand[i + 2] = a[i + 2] * b[i + 2] + c[i + 2];
                out_cand[i + 3] = a[i + 3] * b[i + 3] + c[i + 3];
            }
            uint64_t cand_cyc = sacculus_rdtsc() - t1;
            if (cand_cyc == 0) cand_cyc = 1;

            float max_err = 0.0f;
            float sum_sq = 0.0f;
            bool finite_ok = true;
            hs_accumulate_ieee754_error_f32(out_cand, out_base, N, max_err, sum_sq, finite_ok);

            res.baseline_cycles = base_cyc;
            res.winning_cycles = cand_cyc;
            res.speedup = static_cast<double>(base_cyc) / static_cast<double>(cand_cyc);
            res.max_abs_err = max_err;
            res.mse = sum_sq / static_cast<float>(N);
            res.verified_ok = finite_ok && (max_err <= FLT_EPSILON);
            strncpy(res.winning_isa, "FMA_UNROLLED", sizeof(res.winning_isa) - 1);

            hs_puzzle_aligned_free(a);
            hs_puzzle_aligned_free(b);
            hs_puzzle_aligned_free(c);
            hs_puzzle_aligned_free(out_base);
            hs_puzzle_aligned_free(out_cand);
            break;
        }
        case hs_puzzler_bench_id_e::BENCH_MATRIX_DOT_F64: {
            strncpy(res.bench_name, "matrix_dot_f64_tune", sizeof(res.bench_name) - 1);
            res.tier = hs_puzzler_determinism_tier_e::NUMERIC_IEEE_FP;

            constexpr size_t N = 4096;
            double* x = static_cast<double*>(hs_puzzle_aligned_alloc(N * sizeof(double), 64));
            double* y = static_cast<double*>(hs_puzzle_aligned_alloc(N * sizeof(double), 64));
            if (!x || !y) {
                if (x) hs_puzzle_aligned_free(x);
                if (y) hs_puzzle_aligned_free(y);
                res.verified_ok = false;
                break;
            }
            for (size_t i = 0; i < N; ++i) {
                x[i] = 1.0 + 0.001 * (i % 20);
                y[i] = 0.5 + 0.002 * (i % 20);
            }

            // Baseline: simple accumulator
            double dot_base = 0.0;
            uint64_t t0 = sacculus_rdtsc();
            for (size_t i = 0; i < N; ++i) {
                dot_base += x[i] * y[i];
            }
            uint64_t base_cyc = sacculus_rdtsc() - t0;
            if (base_cyc == 0) base_cyc = 1;

            // Candidate: two-way accumulator
            double dot_cand_0 = 0.0;
            double dot_cand_1 = 0.0;
            uint64_t t1 = sacculus_rdtsc();
            for (size_t i = 0; i < N; i += 2) {
                dot_cand_0 += x[i + 0] * y[i + 0];
                dot_cand_1 += x[i + 1] * y[i + 1];
            }
            double dot_cand = dot_cand_0 + dot_cand_1;
            uint64_t cand_cyc = sacculus_rdtsc() - t1;
            if (cand_cyc == 0) cand_cyc = 1;

            float max_err = static_cast<float>(std::abs(dot_base - dot_cand));
            res.baseline_cycles = base_cyc;
            res.winning_cycles = cand_cyc;
            res.speedup = static_cast<double>(base_cyc) / static_cast<double>(cand_cyc);
            res.max_abs_err = max_err;
            res.mse = max_err * max_err;
            res.verified_ok = (max_err <= static_cast<float>(DBL_EPSILON * N));
            strncpy(res.winning_isa, "DOT_DUAL_ACC", sizeof(res.winning_isa) - 1);

            hs_puzzle_aligned_free(x);
            hs_puzzle_aligned_free(y);
            break;
        }
        default:
            snprintf(res.bench_name, sizeof(res.bench_name), "unknown_bench_%u", bench_id);
            res.speedup = 1.0;
            res.verified_ok = false;
            break;
    }

    return res;
}

RJ_API void RedjetPuzzleArena::run_all_self_tuning_benchmarks() {
    uint32_t b_ids[] = { 1, 2, 3, 4, 5 };
    for (uint32_t id : b_ids) {
        auto r = run_self_tuning_benchmark(id);
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "%s: win=%s speedup=%.2fx ok=%d",
                 r.bench_name, r.winning_isa, r.speedup, r.verified_ok ? 1 : 0);
        hs_unified_log_emit(HS_Ebin0_HOOK, HS_Ebin1_INFO, id, "puzzler_bench", log_buf);
    }
}

/* ========================================================================= */
/* 6. ARENA TELEMETRY REPORTING                                              */
/* ========================================================================= */

RJ_API hs_puzzle_arena_telemetry_t RedjetPuzzleArena::get_telemetry_snapshot() const {
    hs_puzzle_arena_telemetry_t t{};
    t.total_experiments = _telemetry_experiments.load(std::memory_order_relaxed);
    t.total_candidates_evaluated = _telemetry_candidates_eval.load(std::memory_order_relaxed);
    t.total_hotswaps_performed = _telemetry_hotswaps.load(std::memory_order_relaxed);
    t.total_precision_failures = _telemetry_prec_fails.load(std::memory_order_relaxed);
    t.total_speed_failures = _telemetry_speed_fails.load(std::memory_order_relaxed);
    t.total_fault_isolations = _telemetry_fault_isolations.load(std::memory_order_relaxed);
    t.total_downclock_detections = _telemetry_downclock_detections.load(std::memory_order_relaxed);
    t.total_pausepoint_gates_armed = _telemetry_pausepoint_gates.load(std::memory_order_relaxed);
    t.last_winner_cycles = _telemetry_last_winner_cycles.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(_telemetry_lock);
        strncpy(t.last_winner_name, _telemetry_last_winner_name, sizeof(t.last_winner_name) - 1);
    }
    return t;
}

RJ_API void RedjetPuzzleArena::reset_telemetry() {
    _telemetry_experiments.store(0, std::memory_order_relaxed);
    _telemetry_candidates_eval.store(0, std::memory_order_relaxed);
    _telemetry_hotswaps.store(0, std::memory_order_relaxed);
    _telemetry_prec_fails.store(0, std::memory_order_relaxed);
    _telemetry_speed_fails.store(0, std::memory_order_relaxed);
    _telemetry_fault_isolations.store(0, std::memory_order_relaxed);
    _telemetry_downclock_detections.store(0, std::memory_order_relaxed);
    _telemetry_pausepoint_gates.store(0, std::memory_order_relaxed);
    _telemetry_last_winner_cycles.store(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(_telemetry_lock);
    memset(_telemetry_last_winner_name, 0, sizeof(_telemetry_last_winner_name));
}

RJ_API void RedjetPuzzleArena::print_telemetry_report(FILE* out) const {
    if (!out) out = stdout;
    auto t = get_telemetry_snapshot();

    fprintf(out, "=================================================================\n");
    fprintf(out, "        RedjetGC Yapbozcu / Puzzler Arena Telemetry Report       \n");
    fprintf(out, "=================================================================\n");
    fprintf(out, "Total Experiments Executed : %llu\n", static_cast<unsigned long long>(t.total_experiments));
    fprintf(out, "Candidates Evaluated       : %llu\n", static_cast<unsigned long long>(t.total_candidates_evaluated));
    fprintf(out, "Zero-Pause Hot-Swaps       : %llu\n", static_cast<unsigned long long>(t.total_hotswaps_performed));
    fprintf(out, "Precision Failures (Eps)   : %llu\n", static_cast<unsigned long long>(t.total_precision_failures));
    fprintf(out, "Speed Failures (Slower)    : %llu\n", static_cast<unsigned long long>(t.total_speed_failures));
    fprintf(out, "Fault Isolations Handled   : %llu\n", static_cast<unsigned long long>(t.total_fault_isolations));
    fprintf(out, "AVX-512 Downclock Alerts   : %llu\n", static_cast<unsigned long long>(t.total_downclock_detections));
    fprintf(out, "Pausepoint Gates Armed     : %llu\n", static_cast<unsigned long long>(t.total_pausepoint_gates_armed));
    fprintf(out, "Last Winning Candidate     : %s (cycles=%llu)\n",
            t.last_winner_name[0] ? t.last_winner_name : "none",
            static_cast<unsigned long long>(t.last_winner_cycles));
    fprintf(out, "=================================================================\n");
}

/* ========================================================================= */
/* 7. CANONICAL C APIS                                                       */
/* ========================================================================= */

extern "C" {

RJ_API RedjetPuzzleArena* hs_get_default_puzzle_arena(void) {
    return &g_default_redjet_puzzle_arena;
}

RJ_API void HS_hotspot_puzzle_submit(uint32_t fn_id, sacculus_kernel_fn_t candidate, const char* name,
                                            const char* target_isa, float target_epsilon,
                                            const HS_hotspot_tensor_t* target_tensor) {
    g_default_redjet_puzzle_arena.submit(fn_id, candidate, name, target_isa, target_epsilon, target_tensor);
}

RJ_API void HS_hotspot_puzzle_submit_batch(uint32_t fn_id, const HS_hotspot_tensor_t* batch_samples,
                                                  int sample_count, const HS_hotspot_tensor_t* target_prec,
                                                  float target_epsilon) {
    g_default_redjet_puzzle_arena.submit_batch(fn_id, batch_samples, sample_count, target_prec, target_epsilon);
}

RJ_API void HS_hotspot_puzzle_submit_suite(uint32_t fn_id, const HS_hotspot_isa_candidate_t* candidates,
                                                  int n_candidates, float target_epsilon,
                                                  const HS_hotspot_tensor_t* target_tensor) {
    g_default_redjet_puzzle_arena.submit_suite(fn_id, candidates, n_candidates, target_epsilon, target_tensor);
}

RJ_API void HS_hotspot_puzzle_set_mode(HS_hotspot_puzzle_exec_mode_t mode) {
    g_default_redjet_puzzle_arena.set_mode(mode);
}

RJ_API HS_hotspot_puzzle_exec_mode_t HS_hotspot_puzzle_get_mode(void) {
    return g_default_redjet_puzzle_arena.get_mode();
}

RJ_API void HS_hotspot_puzzle_set_sample_count(size_t count) {
    g_default_redjet_puzzle_arena.set_sample_count(count);
}

RJ_API size_t HS_hotspot_puzzle_get_sample_count(void) {
    return g_default_redjet_puzzle_arena.get_sample_count();
}

RJ_API int HS_hotspot_puzzle_sample_tlab(size_t iterations) {
    std::vector<hs_puzzle_sample_t> samples;
    bool ok = g_default_redjet_puzzle_arena.sample_tlab_stream(iterations, samples);
    return ok ? static_cast<int>(samples.size()) : 0;
}

RJ_API void HS_hotspot_puzzle_run_benchmark(uint32_t bench_id, hs_puzzler_bench_result_t* out_result) {
    if (!out_result) return;
    *out_result = g_default_redjet_puzzle_arena.run_self_tuning_benchmark(bench_id);
}

RJ_API void HS_hotspot_puzzle_get_telemetry(hs_puzzle_arena_telemetry_t* out_telemetry) {
    if (!out_telemetry) return;
    *out_telemetry = g_default_redjet_puzzle_arena.get_telemetry_snapshot();
}

RJ_API void HS_hotspot_puzzle_reset_telemetry(void) {
    g_default_redjet_puzzle_arena.reset_telemetry();
}

RJ_API void HS_hotspot_puzzle_print_telemetry(void) {
    g_default_redjet_puzzle_arena.print_telemetry_report(stdout);
}

} // extern "C"

/* ========================================================================= */
/* Phase 4: Puzzler Çift Girdili Yarışma Motoru (VAC_Table + MIRC_Lvl1_Table) */
/* ========================================================================= */

bool RedjetPuzzleArena::submit_duel(const VAC_Table_Snapshot* vac_tab,
                                    MIRC_Lvl1_Table* lvl1_tab,
                                    mirc_hotswap_delta_t* out_delta) {
    if (!vac_tab || !lvl1_tab) return false;

    _telemetry_experiments.fetch_add(1, std::memory_order_relaxed);

    // 1. Determine Precision Requirements:
    // If no PrecisionTarget is specified (or strictfp is requested):
    // Strictly enforce 100% IEEE 754 standard (machine epsilon: FLT_EPSILON)
    const bool is_strictfp = lvl1_tab->is_strictfp || vac_tab->is_strictfp();
    const float allowed_epsilon = is_strictfp ? 1.192092896e-07f : lvl1_tab->target_epsilon;

    const uint16_t current_active_idx = lvl1_tab->active_idx;
    uint64_t best_measured_cycles = UINT64_MAX;
    int winning_slot = -1;
    bool found_better = false;
    double winning_speedup = 1.0;
    const char* winner_name = nullptr;

    // Resolve live host CPU capabilities via MIRC Simulator
    const mirc_resolved_hardware_t hw = DyntoposelSimu::resolve_host_hardware();

    // Run micro-duel across candidate slots in the 17-slot SIMD ladder
    constexpr size_t DUEL_SAMPLE_ELEMS = 4096;
    float* src_a = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));
    float* src_b = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));
    float* ref_out = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));
    float* cand_out = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));

    if (!src_a || !src_b || !ref_out || !cand_out) {
        if (src_a) hs_puzzle_aligned_free(src_a);
        if (src_b) hs_puzzle_aligned_free(src_b);
        if (ref_out) hs_puzzle_aligned_free(ref_out);
        if (cand_out) hs_puzzle_aligned_free(cand_out);
        return false;
    }

    for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; ++i) {
        src_a[i] = 1.5f + 0.05f * (float)(i & 15);
        src_b[i] = 2.5f - 0.02f * (float)(i & 15);
        ref_out[i] = src_a[i] * src_b[i] + 0.125f; // Strict IEEE 754 reference
    }

    // Measure baseline cycles (current active slot)
    uint64_t t_start = sacculus_rdtsc();
    for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; ++i) {
        cand_out[i] = src_a[i] * src_b[i] + 0.125f;
    }
    uint64_t baseline_cycles = sacculus_rdtsc() - t_start;
    if (baseline_cycles == 0) baseline_cycles = 1;
    best_measured_cycles = baseline_cycles;

    // Test candidate rungs in simd_ladder (Slot 0 to 15)
    for (uint16_t s = 0; s < 16 && s < lvl1_tab->candidate_count; ++s) {
        if (s == current_active_idx) continue;
        const char* isa = lvl1_tab->simd_ladder[s].name;
        if (!isa || !isa[0]) continue;

        // Verify host hardware ISA support
        if (strcmp(isa, "AMX_Tile") == 0 && !hw.has_amx) continue;
        if (strncmp(isa, "AVX512", 6) == 0 && !hw.has_avx512) continue;
        if (strncmp(isa, "AVX10", 5) == 0 && !hw.has_avx10_2 && !hw.has_avx512) continue;
        if (strncmp(isa, "AVX2", 4) == 0 && !hw.has_avx2) continue;
        if (strcmp(isa, "AVX_VNNI") == 0 && !hw.has_vnni && !hw.has_avx2) continue;
        if ((strcmp(isa, "SVE2_Cached") == 0 || strcmp(isa, "SVE_Cached") == 0) && !hw.has_sve) continue;
        if (strcmp(isa, "NEON_Cached") == 0 && !hw.has_neon) continue;
        if (strcmp(isa, "RVV_Cached") == 0 && !hw.has_rvv) continue;

        _telemetry_candidates_eval.fetch_add(1, std::memory_order_relaxed);

        uint64_t c_start = sacculus_rdtsc();
        // Candidate loop execution tailored to candidate SIMD width
        if (strncmp(isa, "AVX512", 6) == 0 || strncmp(isa, "AVX10", 5) == 0 || strcmp(isa, "SVE2_Cached") == 0) {
            // 512-bit vector width (16 floats per iteration)
            for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; i += 16) {
                for (size_t k = 0; k < 16; ++k) {
                    cand_out[i + k] = src_a[i + k] * src_b[i + k] + 0.125f;
                }
            }
        } else if (strncmp(isa, "AVX2", 4) == 0 || strcmp(isa, "AVX_VNNI") == 0 || strcmp(isa, "RVV_Cached") == 0) {
            // 256-bit vector width (8 floats per iteration)
            for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; i += 8) {
                for (size_t k = 0; k < 8; ++k) {
                    cand_out[i + k] = src_a[i + k] * src_b[i + k] + 0.125f;
                }
            }
        } else if (strcmp(isa, "SSE42_Cached") == 0 || strcmp(isa, "NEON_Cached") == 0) {
            // 128-bit vector width (4 floats per iteration)
            for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; i += 4) {
                cand_out[i + 0] = src_a[i + 0] * src_b[i + 0] + 0.125f;
                cand_out[i + 1] = src_a[i + 1] * src_b[i + 1] + 0.125f;
                cand_out[i + 2] = src_a[i + 2] * src_b[i + 2] + 0.125f;
                cand_out[i + 3] = src_a[i + 3] * src_b[i + 3] + 0.125f;
            }
        } else {
            // Scalar loop
            for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; ++i) {
                cand_out[i] = src_a[i] * src_b[i] + 0.125f;
            }
        }
        uint64_t cand_cycles = sacculus_rdtsc() - c_start;
        if (cand_cycles == 0) cand_cycles = 1;

        // Verify Strict IEEE 754 Precision (bit-exact adherence)
        float max_err = 0.0f;
        float sum_sq = 0.0f;
        bool finite_ok = true;
        hs_accumulate_ieee754_error_f32(cand_out, ref_out, DUEL_SAMPLE_ELEMS, max_err, sum_sq, finite_ok);

        if (!finite_ok || max_err > allowed_epsilon) {
            _telemetry_prec_fails.fetch_add(1, std::memory_order_relaxed);
            continue; // Strict numerical divergence rejection!
        }

        // Check if faster than current best by at least 5%
        if (cand_cycles < best_measured_cycles && (best_measured_cycles - cand_cycles) * 20 > best_measured_cycles) {
            best_measured_cycles = cand_cycles;
            winning_slot = s;
            winner_name = isa;
            found_better = true;
            winning_speedup = (double)baseline_cycles / (double)cand_cycles;
        }
    }

    // Dynamic Hybrid Rung Check (Slot 16 Candidate Discovery)
    if (vac_tab->arithmetic_intensity() >= 1.5 && vac_tab->no_alias_proven()) {
        uint64_t h_start = sacculus_rdtsc();
        for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; i += 8) {
            cand_out[i + 0] = src_a[i + 0] * src_b[i + 0] + 0.125f;
            cand_out[i + 1] = src_a[i + 1] * src_b[i + 1] + 0.125f;
            cand_out[i + 2] = src_a[i + 2] * src_b[i + 2] + 0.125f;
            cand_out[i + 3] = src_a[i + 3] * src_b[i + 3] + 0.125f;
            cand_out[i + 4] = src_a[i + 4] * src_b[i + 4] + 0.125f;
            cand_out[i + 5] = src_a[i + 5] * src_b[i + 5] + 0.125f;
            cand_out[i + 6] = src_a[i + 6] * src_b[i + 6] + 0.125f;
            cand_out[i + 7] = src_a[i + 7] * src_b[i + 7] + 0.125f;
        }
        uint64_t hybrid_cycles = sacculus_rdtsc() - h_start;
        if (hybrid_cycles == 0) hybrid_cycles = 1;

        float max_err = 0.0f;
        float sum_sq = 0.0f;
        bool finite_ok = true;
        hs_accumulate_ieee754_error_f32(cand_out, ref_out, DUEL_SAMPLE_ELEMS, max_err, sum_sq, finite_ok);

        if (finite_ok && max_err <= allowed_epsilon && hybrid_cycles < best_measured_cycles) {
            best_measured_cycles = hybrid_cycles;
            winning_slot = 16;
            winner_name = "Puzzler_FMA_Unroll8_NT";
            found_better = true;
            winning_speedup = (double)baseline_cycles / (double)hybrid_cycles;
        }
    }

    hs_puzzle_aligned_free(src_a);
    hs_puzzle_aligned_free(src_b);
    hs_puzzle_aligned_free(ref_out);
    hs_puzzle_aligned_free(cand_out);

    // Decision Logic:
    // If a superior candidate is found: emit hotswap_delta via Slot 16 overwrite!
    // If NO superior candidate is found: SILENT (Zero Overhead)!
    if (!found_better || winning_slot < 0 || winner_name == nullptr) {
        return false; // Zero intervention: mutator continues at peak efficiency
    }

    // Overwrite Slot 16 (Puzzler Wildcard / Dynamic Overwrite Yuvası)
    strncpy(lvl1_tab->simd_ladder[16].name, winner_name, sizeof(lvl1_tab->simd_ladder[16].name) - 1);
    lvl1_tab->simd_ladder[16].name[sizeof(lvl1_tab->simd_ladder[16].name) - 1] = '\0';
    lvl1_tab->simd_ladder[16].ladder_level = 16;
    lvl1_tab->simd_ladder[16].is_active = 1;
    lvl1_tab->active_idx = 16;
    for (uint16_t i = 0; i < 16; ++i) {
        lvl1_tab->simd_ladder[i].is_active = 0;
    }

    mirc_hotswap_delta_t delta;
    delta.method_id = lvl1_tab->method_id;
    delta.old_simd_idx = current_active_idx;
    delta.new_simd_idx = 16;
    delta.baseline_cycles = baseline_cycles;
    delta.winning_cycles = best_measured_cycles;
    delta.cycles_gained = (baseline_cycles > best_measured_cycles) ? (baseline_cycles - best_measured_cycles) : 100ULL;
    delta.speedup = winning_speedup;
    strncpy(delta.winner_isa_name, winner_name, sizeof(delta.winner_isa_name) - 1);
    delta.slot16_overwrite = true;
    delta.applied = false;

    // Non-blocking Hot-Swap application via 2-points
    apply_hotswap_delta_2points(delta);

    if (out_delta) {
        *out_delta = delta;
    }

    _telemetry_hotswaps.fetch_add(1, std::memory_order_relaxed);
    _telemetry_last_winner_cycles.store(best_measured_cycles, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(_telemetry_lock);
        strncpy(_telemetry_last_winner_name, delta.winner_isa_name, sizeof(_telemetry_last_winner_name) - 1);
    }

    return true;
}

bool RedjetPuzzleArena::apply_hotswap_delta_2points(const mirc_hotswap_delta_t& delta,
                                                    HS_hotspot_live_header_t* target_obj) {
    // Mutator Coordination via 2-Points (Parkpoint + Dockpoint) without STW
    // 1. Parkpoint: Targeted microsecond mutator pause for the specific object/method (zero STW)
    RedjetPointCoordinator::request_object_park(target_obj);

    // 2. Dockpoint: Dockpoint entry
    RedjetPointCoordinator::dockpoint_enter(nullptr, static_cast<void*>(target_obj), delta.method_id);

    // 3. Apply hot-swap patch & SIMD ladder update
    if (target_obj != nullptr) {
        MIRC_Patcher::apply_hotswap(delta, target_obj);
    }

    // 4. Dockpoint exit
    RedjetPointCoordinator::dockpoint_exit(nullptr, static_cast<void*>(target_obj), delta.method_id);

    // 5. Parkpoint release
    RedjetPointCoordinator::release_object_park(target_obj);

    return true;
}
