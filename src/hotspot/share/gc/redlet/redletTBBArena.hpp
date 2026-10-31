/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:6904-6971.
 */

#ifndef SHARE_GC_REDLET_REDLETTBBARENA_HPP
#define SHARE_GC_REDLET_REDLETTBBARENA_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <functional>

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

typedef enum SF_custominiVm_core_tier_t {
    SF_CORE_TIER_ANY         = 0,
    SF_CORE_TIER_PERFORMANCE = 1,
    SF_CORE_TIER_EFFICIENCY  = 2
} SF_custominiVm_core_tier_t;

extern thread_local SF_custominiVm_core_tier_t tl_sf_core_tier;

#ifndef REDLET_STANDALONE_BUILD
#define __TBB_NO_IMPLICIT_LINKAGE 1
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>
#include <oneapi/tbb/partitioner.h>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>

/* ========================================================================= */
/* 1. TBB ARENA OBSERVER WITH CORE TIER & FTZ/DAZ CONFIGURATION              */
/* ========================================================================= */

class sf_gc_arena_observer_t : public tbb::task_scheduler_observer {
    SF_custominiVm_core_tier_t m_tier;
public:
    sf_gc_arena_observer_t(tbb::task_arena& arena, SF_custominiVm_core_tier_t tier);
    ~sf_gc_arena_observer_t() override;
    void on_scheduler_entry(bool is_worker) override;
    void on_scheduler_exit(bool is_worker) override;
};

/* ========================================================================= */
/* 2. HETEROGENEOUS TBB TASK ARENA MANAGER (SacFor_VM.cpp:6904-6971)         */
/* ========================================================================= */

struct sf_gc_tbb_arena_manager_t {
    tbb::task_arena            p_arena;
    tbb::task_arena            e_arena;
    tbb::task_arena            homo_arena;
    sf_gc_arena_observer_t*    p_observer{ nullptr };
    sf_gc_arena_observer_t*    e_observer{ nullptr };
    sf_gc_arena_observer_t*    homo_observer{ nullptr };
    tbb::affinity_partitioner  p_affinity;
    tbb::affinity_partitioner  e_affinity;
    tbb::affinity_partitioner  homo_affinity;
    tbb::concurrent_queue<uint32_t> concurrent_resurrect_queue;
    std::atomic<bool>          initialized{ false };

    struct thread_stats_t {
        size_t swept = 0;
        size_t evicted = 0;
        size_t relocated = 0;
        size_t relocated_bytes = 0;
    };
    tbb::enumerable_thread_specific<thread_stats_t> ets_stats;

    MSIMD_API void init();
    MSIMD_API void destroy();

    template <typename F>
    void run_on_p_core(F&& func) {
        if (p_arena.is_active()) {
            p_arena.execute(std::forward<F>(func));
        } else {
            homo_arena.execute(std::forward<F>(func));
        }
    }

    template <typename F>
    void run_on_e_core(F&& func) {
        if (e_arena.is_active()) {
            e_arena.execute(std::forward<F>(func));
        } else {
            homo_arena.execute(std::forward<F>(func));
        }
    }
};

#else // REDLET_STANDALONE_BUILD

struct sf_gc_tbb_arena_manager_t {
    std::atomic<bool> initialized{ false };

    MSIMD_API void init();
    MSIMD_API void destroy();

    template <typename F>
    void run_on_p_core(F&& func) {
        tl_sf_core_tier = SF_CORE_TIER_PERFORMANCE;
        func();
        tl_sf_core_tier = SF_CORE_TIER_ANY;
    }

    template <typename F>
    void run_on_e_core(F&& func) {
        tl_sf_core_tier = SF_CORE_TIER_EFFICIENCY;
        func();
        tl_sf_core_tier = SF_CORE_TIER_ANY;
    }

    template <typename F>
    void run_on_homo(F&& func) {
        tl_sf_core_tier = SF_CORE_TIER_ANY;
        func();
    }
};

#endif // REDLET_STANDALONE_BUILD

using RedletTBBArenaManager = sf_gc_tbb_arena_manager_t;

#endif // SHARE_GC_REDLET_REDLETTBBARENA_HPP
