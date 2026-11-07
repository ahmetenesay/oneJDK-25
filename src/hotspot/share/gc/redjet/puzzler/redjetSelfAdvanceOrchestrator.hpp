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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETSELFADVANCEORCHESTRATOR_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETSELFADVANCEORCHESTRATOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <thread>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvanceHooks.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvanceHeuristics.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvancePrecision.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. P-CORE / E-CORE LANE STEALING & WORKLOAD MIGRATION                     */
/* ========================================================================= */

class RedjetLaneStealingEngine {
private:
    std::atomic<bool>     _gc_migrated_to_ecore{ false };
    std::atomic<uint64_t> _migration_count{ 0 };
    std::atomic<uint64_t> _revert_count{ 0 };

public:
    RedjetLaneStealingEngine();

    // Migrate calling GC thread to Efficiency cores to free P-cores for Java threads
    bool migrate_gc_to_ecore();

    // Restore calling GC thread to any available core
    bool restore_gc_affinity();

    bool is_gc_on_ecore() const noexcept {
        return _gc_migrated_to_ecore.load(std::memory_order_relaxed);
    }

    uint64_t get_migration_count() const noexcept {
        return _migration_count.load(std::memory_order_relaxed);
    }

    uint64_t get_revert_count() const noexcept {
        return _revert_count.load(std::memory_order_relaxed);
    }

    void reset();
};

/* ========================================================================= */
/* 2. ZERO-PAUSE PAUSEPOINT HOT-SWAP COORDINATOR                             */
/* ========================================================================= */

class RedjetPausepointHotSwap {
private:
    std::atomic<uint64_t> _hot_swaps_completed{ 0 };
    std::atomic<uint64_t> _rollbacks_executed{ 0 };

public:
    RedjetPausepointHotSwap();

    // Atomically swap a GC routine pointer without stopping Java threads
    bool hot_swap_routine(std::atomic<void*>* target_fn_ptr, void* new_fn_ptr, const char* routine_name);
    bool hot_swap_routine(void* volatile* target_fn_ptr, void* new_fn_ptr, const char* routine_name);

    uint64_t get_hot_swaps_completed() const noexcept {
        return _hot_swaps_completed.load(std::memory_order_relaxed);
    }

    uint64_t get_rollbacks_executed() const noexcept {
        return _rollbacks_executed.load(std::memory_order_relaxed);
    }

    void record_rollback() noexcept {
        _rollbacks_executed.fetch_add(1, std::memory_order_relaxed);
    }

    void reset();
};

/* ========================================================================= */
/* 3. CENTRAL SELF-ADVANCEMENT ORCHESTRATION ENGINE                          */
/* ========================================================================= */

struct RedjetSelfAdvancementStats {
    uint64_t total_ticks;
    uint64_t bitmap_evolutions;
    uint64_t lane_migrations;
    uint64_t hot_swaps;
    uint64_t rollbacks;
    uint64_t precision_guards_enforced;
    uint64_t precision_violations;
    uint64_t leak_alerts;
    uint8_t  active_bitmap_kernel;
    uint8_t  active_memzero_strategy;
    uint8_t  active_rmrp_strategy;
    uint32_t active_resurrection_quota;
    bool     is_hybrid_active;
    bool     is_high_cache_pressure;
};

class RedjetSelfAdvancementEngine {
private:
    std::atomic<bool>     _running{ false };
    std::atomic<uint64_t> _tick_count{ 0 };

    RedjetLaneStealingEngine _lane_stealing;
    RedjetPausepointHotSwap  _hot_swap;

public:
    RedjetSelfAdvancementEngine();

    void initialize();
    void start();
    void stop();
    bool is_running() const noexcept { return _running.load(std::memory_order_relaxed); }

    // Periodic self-advancement evaluation step
    void tick(size_t heap_used, size_t heap_committed, size_t heap_reserved, double alloc_rate_mbps);

    // Deterministic rollback of all experimental optimizations to baseline reference kernels
    void trigger_deterministic_rollback(const char* reason);

    RedjetLaneStealingEngine& lane_stealing() noexcept { return _lane_stealing; }
    RedjetPausepointHotSwap&  hot_swap() noexcept { return _hot_swap; }

    RedjetSelfAdvancementStats query_stats() const;
    void reset();
};

/* ========================================================================= */
/* 4. GLOBAL INSTANCE & C EXPORT APIS                                        */
/* ========================================================================= */

extern RedjetSelfAdvancementEngine g_redjet_self_advancement;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void SF_self_advance_init(void);
RJ_API void SF_self_advance_start(void);
RJ_API void SF_self_advance_stop(void);
RJ_API int  SF_self_advance_is_running(void);
RJ_API void SF_self_advance_tick(size_t heap_used, size_t heap_committed, size_t heap_reserved, double alloc_rate);
RJ_API void SF_self_advance_rollback(const char* reason);
RJ_API int  SF_self_advance_migrate_gc_ecore(void);
RJ_API int  SF_self_advance_restore_gc_affinity(void);
RJ_API RedjetSelfAdvancementStats SF_self_advance_query_stats(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_PUZZLER_REDJETSELFADVANCEORCHESTRATOR_HPP