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

#include "gc/redjet/rjspcf/self_advance/redjetSelfAdvanceOrchestrator.hpp"
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#define log_warning(...) if (false) printf
#define log_error(...) if (false) printf
#endif

RedjetSelfAdvancementEngine g_redjet_self_advancement;

/* ========================================================================= */
/* 1. RedjetLaneStealingEngine                                               */
/* ========================================================================= */

RedjetLaneStealingEngine::RedjetLaneStealingEngine() {
    reset();
}

bool RedjetLaneStealingEngine::migrate_gc_to_ecore() {
    if (!hs_is_hybrid_cpu()) {
        return false;
    }

    bool success = hs_bind_current_thread(HS_CORE_TIER_EFFICIENCY);
    if (success) {
        _gc_migrated_to_ecore.store(true, std::memory_order_release);
        _migration_count.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_CORE_PIN_PERF, HS_Ebin1_INFO, 0,
                            "lane_stealing", "[Redjet Lane Stealing] GC worker migrated to E-Core");
        g_redjet_self_advance_telemetry.record_event(
            HS_EVENT_AFFINITY_MIGRATION, "lane_stealing", "GC worker migrated to E-Core", 0.0, 1.0);
    }
    return success;
}

bool RedjetLaneStealingEngine::restore_gc_affinity() {
    bool success = hs_bind_current_thread(HS_CORE_TIER_ANY);
    if (success) {
        _gc_migrated_to_ecore.store(false, std::memory_order_release);
        _revert_count.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_CORE_PIN_PERF, HS_Ebin1_INFO, 0,
                            "lane_stealing", "[Redjet Lane Stealing] GC worker restored to Any-Core");
        g_redjet_self_advance_telemetry.record_event(
            HS_EVENT_AFFINITY_MIGRATION, "lane_stealing", "GC worker restored to Any-Core", 1.0, 0.0);
    }
    return success;
}

void RedjetLaneStealingEngine::reset() {
    _gc_migrated_to_ecore.store(false, std::memory_order_relaxed);
    _migration_count.store(0, std::memory_order_relaxed);
    _revert_count.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. RedjetPausepointHotSwap                                                */
/* ========================================================================= */

RedjetPausepointHotSwap::RedjetPausepointHotSwap() {
    reset();
}

bool RedjetPausepointHotSwap::hot_swap_routine(std::atomic<void*>* target_fn_ptr, void* new_fn_ptr, const char* routine_name) {
    if (!target_fn_ptr || !new_fn_ptr) return false;

    // Coordinate with stream pause gate
    hs_gc_pausepoint_set_gate(false);

    void* old_ptr = target_fn_ptr->exchange(new_fn_ptr, std::memory_order_acq_rel);
    _hot_swaps_completed.fetch_add(1, std::memory_order_relaxed);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "[Redjet Hot-Swap] Swapped routine '%s': %p -> %p",
             routine_name ? routine_name : "unnamed", old_ptr, new_fn_ptr);
    hs_unified_log_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, "pausepoint_hotswap", msg);
    g_redjet_self_advance_telemetry.record_event(
        HS_EVENT_GC_HOTSWAP, "hotswap", msg, 0.0, static_cast<double>(_hot_swaps_completed.load(std::memory_order_relaxed)));

    return true;
}

bool RedjetPausepointHotSwap::hot_swap_routine(void* volatile* target_fn_ptr, void* new_fn_ptr, const char* routine_name) {
    if (!target_fn_ptr || !new_fn_ptr) return false;

    hs_gc_pausepoint_set_gate(false);

#if defined(_WIN32)
    void* old_ptr = _InterlockedExchangePointer(target_fn_ptr, new_fn_ptr);
#else
    void* old_ptr = __atomic_exchange_n(target_fn_ptr, new_fn_ptr, __ATOMIC_ACQ_REL);
#endif

    _hot_swaps_completed.fetch_add(1, std::memory_order_relaxed);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "[Redjet Hot-Swap] Swapped routine '%s': %p -> %p",
             routine_name ? routine_name : "unnamed", old_ptr, new_fn_ptr);
    hs_unified_log_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, "pausepoint_hotswap", msg);
    g_redjet_self_advance_telemetry.record_event(
        HS_EVENT_GC_HOTSWAP, "hotswap", msg, 0.0, static_cast<double>(_hot_swaps_completed.load(std::memory_order_relaxed)));

    return true;
}

void RedjetPausepointHotSwap::reset() {
    _hot_swaps_completed.store(0, std::memory_order_relaxed);
    _rollbacks_executed.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 3. RedjetSelfAdvancementEngine                                            */
/* ========================================================================= */

RedjetSelfAdvancementEngine::RedjetSelfAdvancementEngine() {
    reset();
}

void RedjetSelfAdvancementEngine::initialize() {
    g_redjet_tsc_hook_counter.reset();
    g_redjet_falline_analyzer.reset();
    g_redjet_cache_tracker.reset();
    g_redjet_bitmap_evolver.reset();
    g_redjet_memreset_evolver.reset();
    g_redjet_rmrp_heuristics.reset();
    g_redjet_resurrection_heuristics.reset();
    g_redjet_precision_guard.reset();
    g_redjet_self_advance_telemetry.reset();
    _lane_stealing.reset();
    _hot_swap.reset();
    _tick_count.store(0, std::memory_order_relaxed);
    _running.store(true, std::memory_order_release);
}

void RedjetSelfAdvancementEngine::start() {
    _running.store(true, std::memory_order_release);
}

void RedjetSelfAdvancementEngine::stop() {
    _running.store(false, std::memory_order_release);
}

void RedjetSelfAdvancementEngine::tick(size_t heap_used, size_t heap_committed, size_t heap_reserved, double alloc_rate_mbps) {
    if (!_running.load(std::memory_order_acquire)) return;

    _tick_count.fetch_add(1, std::memory_order_relaxed);

    // 1. Evaluate Dynamic RMRP Strategy
    g_redjet_rmrp_heuristics.evaluate_and_update(heap_used, heap_committed, heap_reserved, alloc_rate_mbps);

    // 2. Evaluate Bitmap Scanning Kernel (run evolution once or when ticks reach threshold)
    if (g_redjet_bitmap_evolver.get_evolution_count() == 0) {
        g_redjet_bitmap_evolver.verify_and_evolve_best_kernel();
    }

    // 3. Evaluate Thread Allocation Churn and Memory Leak Detection
    g_redjet_falline_analyzer.evaluate_leak_heuristics();

    // 4. Evaluate Heterogeneous Lane Stealing (P-Core to E-Core Migration)
    if (hs_is_hybrid_cpu()) {
        bool high_cache_pressure = g_redjet_cache_tracker.is_high_cache_pressure();
        if (high_cache_pressure && !_lane_stealing.is_gc_on_ecore()) {
            _lane_stealing.migrate_gc_to_ecore();
        } else if (!high_cache_pressure && _lane_stealing.is_gc_on_ecore()) {
            _lane_stealing.restore_gc_affinity();
        }
    }
}

void RedjetSelfAdvancementEngine::trigger_deterministic_rollback(const char* reason) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[Redjet Deterministic Rollback] Reverting all experimental kernels to baseline: %s",
             reason ? reason : "unspecified reason");
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "deterministic_rollback", msg);
    g_redjet_self_advance_telemetry.record_event(HS_EVENT_ROLLBACK, "rollback", msg, 0.0, 0.0);

    // Reset evolved bitmap scanner to scalar baseline
    g_redjet_bitmap_evolver.reset();

    // Reset memory reset to standard libc memset
    g_redjet_memreset_evolver.reset();

    // Reset RMRP to default Plump strategy
    g_redjet_rmrp_heuristics.reset();

    // Restore GC affinity to any available core
    _lane_stealing.restore_gc_affinity();

    _hot_swap.record_rollback();
}

RedjetSelfAdvancementStats RedjetSelfAdvancementEngine::query_stats() const {
    RedjetSelfAdvancementStats s{};
    s.total_ticks = _tick_count.load(std::memory_order_relaxed);
    s.bitmap_evolutions = g_redjet_bitmap_evolver.get_evolution_count();
    s.lane_migrations = _lane_stealing.get_migration_count();
    s.hot_swaps = _hot_swap.get_hot_swaps_completed();
    s.rollbacks = _hot_swap.get_rollbacks_executed();
    s.precision_guards_enforced = g_redjet_precision_guard.get_guards_enforced();
    s.precision_violations = g_redjet_precision_guard.get_violations_caught();
    s.leak_alerts = g_redjet_falline_analyzer.get_leak_alert_count();
    s.active_bitmap_kernel = static_cast<uint8_t>(g_redjet_bitmap_evolver.get_active_kernel_type());
    s.active_memzero_strategy = static_cast<uint8_t>(g_redjet_memreset_evolver.get_active_strategy());
    s.active_rmrp_strategy = static_cast<uint8_t>(g_redjet_rmrp_heuristics.get_current_strategy());
    s.active_resurrection_quota = g_redjet_resurrection_heuristics.get_active_quota();
    s.is_hybrid_active = hs_is_hybrid_cpu();
    s.is_high_cache_pressure = g_redjet_cache_tracker.is_high_cache_pressure();
    return s;
}

void RedjetSelfAdvancementEngine::reset() {
    _running.store(false, std::memory_order_relaxed);
    _tick_count.store(0, std::memory_order_relaxed);
    _lane_stealing.reset();
    _hot_swap.reset();
}

/* ========================================================================= */
/* 4. C EXPORT APIS                                                          */
/* ========================================================================= */

MSIMD_API void HS_self_advance_init(void) {
    g_redjet_self_advancement.initialize();
}

MSIMD_API void HS_self_advance_start(void) {
    g_redjet_self_advancement.start();
}

MSIMD_API void HS_self_advance_stop(void) {
    g_redjet_self_advancement.stop();
}

MSIMD_API int HS_self_advance_is_running(void) {
    return g_redjet_self_advancement.is_running() ? 1 : 0;
}

MSIMD_API void HS_self_advance_tick(size_t heap_used, size_t heap_committed, size_t heap_reserved, double alloc_rate) {
    g_redjet_self_advancement.tick(heap_used, heap_committed, heap_reserved, alloc_rate);
}

MSIMD_API void HS_self_advance_rollback(const char* reason) {
    g_redjet_self_advancement.trigger_deterministic_rollback(reason);
}

MSIMD_API int HS_self_advance_migrate_gc_ecore(void) {
    return g_redjet_self_advancement.lane_stealing().migrate_gc_to_ecore() ? 1 : 0;
}

MSIMD_API int HS_self_advance_restore_gc_affinity(void) {
    return g_redjet_self_advancement.lane_stealing().restore_gc_affinity() ? 1 : 0;
}

MSIMD_API RedjetSelfAdvancementStats HS_self_advance_query_stats(void) {
    return g_redjet_self_advancement.query_stats();
}
