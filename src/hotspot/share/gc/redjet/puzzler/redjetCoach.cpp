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

#include "gc/redjet/puzzler/redjetCoach.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

RedjetCoach g_redjet_coach;

RedjetCoach::RedjetCoach() {
    _puzzle_arena = hs_get_default_puzzle_arena();
    reset();
}

RedjetCoach& RedjetCoach::instance() {
    return g_redjet_coach;
}

void RedjetCoach::reset() {
    _gc_coach_ticks.store(0, std::memory_order_relaxed);
    _affinity_coach_ticks.store(0, std::memory_order_relaxed);
    _self_advance_ticks.store(0, std::memory_order_relaxed);
}

void RedjetCoach::coach_gc_operations() {
    std::lock_guard<std::mutex> guard(_coach_lock);
    _gc_coach_ticks.fetch_add(1, std::memory_order_relaxed);

    // 1. Evolve card table / mark bitmap scanner kernel (0-bit divergence gold standard)
    g_redjet_bitmap_evolver.verify_and_evolve_best_kernel();

    // 2. Query cache pressure to coach memory zeroing & copying
    bool high_pressure = g_redjet_cache_tracker.is_high_cache_pressure();
    bool on_pcore = !g_redjet_self_advancement.lane_stealing().is_gc_on_ecore();

    // 3. If 3D V-Cache is present or L3 misses are exceptionally low, standard temporal stores win
    bool has_vcache = HS_hotspot_has_3d_vcache();
    if (has_vcache) {
        high_pressure = false;
    }

    // 4. Trigger self-tuning micro-benchmarks if arena is attached
    if (_puzzle_arena) {
        _puzzle_arena->run_all_self_tuning_benchmarks();
    }

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf),
             "[RedjetCoach::coach_gc_operations] Active bitmap=%u high_cache_press=%d on_pcore=%d vcache=%d",
             static_cast<unsigned>(g_redjet_bitmap_evolver.get_active_kernel_type()),
             high_pressure ? 1 : 0, on_pcore ? 1 : 0, has_vcache ? 1 : 0);
    hs_unified_log_emit(HS_Ebin0_HOOK, HS_Ebin1_INFO, 0, "coach_gc_ops", log_buf);
}

void RedjetCoach::coach_thread_affinity() {
    std::lock_guard<std::mutex> guard(_coach_lock);
    _affinity_coach_ticks.fetch_add(1, std::memory_order_relaxed);

    if (!hs_is_hybrid_cpu()) {
        return; // Homogeneous architecture; no P/E core affinity partitioning needed
    }

    bool high_pressure = g_redjet_cache_tracker.is_high_cache_pressure();
    bool gc_on_ecore = g_redjet_self_advancement.lane_stealing().is_gc_on_ecore();

    if (high_pressure && !gc_on_ecore) {
        // High mutator contention: migrate background GC threads to E-Cores to free P-Cores for Java
        g_redjet_self_advancement.lane_stealing().migrate_gc_to_ecore();
        g_redjet_self_advance_telemetry.record_event(
            HS_EVENT_AFFINITY_MIGRATION, "coach_lane_steal", "Migrated GC to E-Core due to cache pressure", 0.0, 1.0);
    } else if (!high_pressure && gc_on_ecore) {
        // Low cache pressure: restore GC threads to full system affinity
        g_redjet_self_advancement.lane_stealing().restore_gc_affinity();
        g_redjet_self_advance_telemetry.record_event(
            HS_EVENT_AFFINITY_MIGRATION, "coach_lane_restore", "Restored GC affinity to default cores", 1.0, 0.0);
    }
}

void RedjetCoach::self_advance_evaluate() {
    std::lock_guard<std::mutex> guard(_coach_lock);
    _self_advance_ticks.fetch_add(1, std::memory_order_relaxed);

    // Evaluate memory leak heuristics and thread allocation churn
    g_redjet_falline_analyzer.evaluate_leak_heuristics();

    // Query cache pressure metrics
    RedjetCachePressureMetrics pressure = g_redjet_cache_tracker.query_metrics();

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf),
             "[RedjetCoach::self_advance_evaluate] L1D miss=%.2f%% LLC miss=%.2f%% IPC=%.2f leaks=%llu",
             pressure.l1d_miss_rate * 100.0, pressure.l2_miss_rate * 100.0,
             pressure.instructions_per_cycle,
             static_cast<unsigned long long>(g_redjet_falline_analyzer.get_leak_alert_count()));
    hs_unified_log_emit(HS_Ebin0_HOOK, HS_Ebin1_INFO, 0, "coach_self_advance", log_buf);
}

/* ========================================================================= */
/* C EXPORT APIS                                                             */
/* ========================================================================= */

RJ_API void HS_coach_gc_operations(void) {
    g_redjet_coach.coach_gc_operations();
}

RJ_API void HS_coach_thread_affinity(void) {
    g_redjet_coach.coach_thread_affinity();
}

RJ_API void HS_coach_self_advance_evaluate(void) {
    g_redjet_coach.self_advance_evaluate();
}

RJ_API void HS_coach_reset(void) {
    g_redjet_coach.reset();
}
