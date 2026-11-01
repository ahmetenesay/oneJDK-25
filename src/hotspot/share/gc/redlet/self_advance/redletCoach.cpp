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

#include "gc/redlet/self_advance/redletCoach.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

RedletCoach g_redlet_coach;

RedletCoach::RedletCoach() {
    _puzzle_arena = sf_get_default_puzzle_arena();
    reset();
}

RedletCoach& RedletCoach::instance() {
    return g_redlet_coach;
}

void RedletCoach::reset() {
    _gc_coach_ticks.store(0, std::memory_order_relaxed);
    _affinity_coach_ticks.store(0, std::memory_order_relaxed);
    _self_advance_ticks.store(0, std::memory_order_relaxed);
}

void RedletCoach::coach_gc_operations() {
    std::lock_guard<std::mutex> guard(_coach_lock);
    _gc_coach_ticks.fetch_add(1, std::memory_order_relaxed);

    // 1. Evolve card table / mark bitmap scanner kernel (0-bit divergence gold standard)
    g_redlet_bitmap_evolver.verify_and_evolve_best_kernel();

    // 2. Query cache pressure to coach memory zeroing & copying
    bool high_pressure = g_redlet_cache_tracker.is_high_cache_pressure();
    bool on_pcore = !g_redlet_self_advancement.lane_stealing().is_gc_on_ecore();

    // 3. If 3D V-Cache is present or L3 misses are exceptionally low, standard temporal stores win
    bool has_vcache = SF_custominiVm_has_3d_vcache();
    if (has_vcache) {
        high_pressure = false;
    }

    // 4. Trigger self-tuning micro-benchmarks if arena is attached
    if (_puzzle_arena) {
        _puzzle_arena->run_all_self_tuning_benchmarks();
    }

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf),
             "[RedletCoach::coach_gc_operations] Active bitmap=%u high_cache_press=%d on_pcore=%d vcache=%d",
             static_cast<unsigned>(g_redlet_bitmap_evolver.get_active_kernel_type()),
             high_pressure ? 1 : 0, on_pcore ? 1 : 0, has_vcache ? 1 : 0);
    sf_unified_log_emit(SF_Ebin0_HOOK, SF_Ebin1_INFO, 0, "coach_gc_ops", log_buf);
}

void RedletCoach::coach_thread_affinity() {
    std::lock_guard<std::mutex> guard(_coach_lock);
    _affinity_coach_ticks.fetch_add(1, std::memory_order_relaxed);

    if (!sf_is_hybrid_cpu()) {
        return; // Homogeneous architecture; no P/E core affinity partitioning needed
    }

    bool high_pressure = g_redlet_cache_tracker.is_high_cache_pressure();
    bool gc_on_ecore = g_redlet_self_advancement.lane_stealing().is_gc_on_ecore();

    if (high_pressure && !gc_on_ecore) {
        // High mutator contention: migrate background GC threads to E-Cores to free P-Cores for Java
        g_redlet_self_advancement.lane_stealing().migrate_gc_to_ecore();
        g_redlet_self_advance_telemetry.record_event(
            SF_EVENT_AFFINITY_MIGRATION, "coach_lane_steal", "Migrated GC to E-Core due to cache pressure", 0.0, 1.0);
    } else if (!high_pressure && gc_on_ecore) {
        // Low cache pressure: restore GC threads to full system affinity
        g_redlet_self_advancement.lane_stealing().restore_gc_affinity();
        g_redlet_self_advance_telemetry.record_event(
            SF_EVENT_AFFINITY_MIGRATION, "coach_lane_restore", "Restored GC affinity to default cores", 1.0, 0.0);
    }
}

void RedletCoach::self_advance_evaluate() {
    std::lock_guard<std::mutex> guard(_coach_lock);
    _self_advance_ticks.fetch_add(1, std::memory_order_relaxed);

    // Evaluate memory leak heuristics and thread allocation churn
    g_redlet_falline_analyzer.evaluate_leak_heuristics();

    // Query cache pressure metrics
    RedletCachePressureMetrics pressure = g_redlet_cache_tracker.query_metrics();

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf),
             "[RedletCoach::self_advance_evaluate] L1D miss=%.2f%% LLC miss=%.2f%% IPC=%.2f leaks=%llu",
             pressure.l1d_miss_rate * 100.0, pressure.l2_miss_rate * 100.0,
             pressure.instructions_per_cycle,
             static_cast<unsigned long long>(g_redlet_falline_analyzer.get_leak_alert_count()));
    sf_unified_log_emit(SF_Ebin0_HOOK, SF_Ebin1_INFO, 0, "coach_self_advance", log_buf);
}

/* ========================================================================= */
/* C EXPORT APIS                                                             */
/* ========================================================================= */

MSIMD_API void SF_coach_gc_operations(void) {
    g_redlet_coach.coach_gc_operations();
}

MSIMD_API void SF_coach_thread_affinity(void) {
    g_redlet_coach.coach_thread_affinity();
}

MSIMD_API void SF_coach_self_advance_evaluate(void) {
    g_redlet_coach.self_advance_evaluate();
}

MSIMD_API void SF_coach_reset(void) {
    g_redlet_coach.reset();
}
