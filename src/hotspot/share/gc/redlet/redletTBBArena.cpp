/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:6904-6971.
 */

#include "gc/redlet/redletTBBArena.hpp"
#include "gc/redlet/redletGearkit.hpp"

#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#include <new>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#endif

thread_local SF_custominiVm_core_tier_t tl_sf_core_tier = SF_CORE_TIER_ANY;

/* ========================================================================= */
/* TBB ARENA OBSERVER IMPLEMENTATION                                         */
/* ========================================================================= */

sf_gc_arena_observer_t::sf_gc_arena_observer_t(tbb::task_arena& arena, SF_custominiVm_core_tier_t tier)
    : tbb::task_scheduler_observer(arena), m_tier(tier) {
    observe(true);
}

sf_gc_arena_observer_t::~sf_gc_arena_observer_t() {
    observe(false);
}

void sf_gc_arena_observer_t::on_scheduler_entry(bool is_worker) {
    (void)is_worker;
    tl_sf_core_tier = m_tier;

#if defined(_M_X64) || defined(__x86_64__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24); // FZ bit
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr));
#endif

    if (m_tier == SF_CORE_TIER_PERFORMANCE) {
        sf_hotlog_emit(SF_Ebin0_CORE_PIN_PERF, SF_Ebin1_TRACE, 0, (uint16_t)m_tier, 0, 0, 0, 0, 0, "core_pin_pcore");
    } else if (m_tier == SF_CORE_TIER_EFFICIENCY) {
        sf_hotlog_emit(SF_Ebin0_CORE_PIN_PERF, SF_Ebin1_TRACE, 0, (uint16_t)m_tier, 0, 0, 0, 0, 0, "core_pin_ecore");
    } else {
        sf_hotlog_emit(SF_Ebin0_CORE_UNPIN_ANY, SF_Ebin1_TRACE, 0, (uint16_t)m_tier, 0, 0, 0, 0, 0, "core_unpin_any");
    }
}

void sf_gc_arena_observer_t::on_scheduler_exit(bool is_worker) {
    (void)is_worker;
}

/* ========================================================================= */
/* TBB ARENA MANAGER IMPLEMENTATION                                          */
/* ========================================================================= */

MSIMD_API void sf_gc_tbb_arena_manager_t::init() {
    if (initialized.exchange(true, std::memory_order_acq_rel)) return;

    auto core_types = tbb::info::core_types();
    if (core_types.size() > 1) {
        // Heterogeneous Hybrid Architecture (e.g. Intel Alder/Raptor/Arrow Lake)
        // 1. P-Core arena: high-priority heavy parallel sweep and compaction bursts
        tbb::task_arena::constraints p_c;
        p_c.set_core_type(core_types.front());
        p_arena.initialize(p_c);
        p_observer = new (std::nothrow) sf_gc_arena_observer_t(p_arena, SF_CORE_TIER_PERFORMANCE);

        // 2. E-Core arena: background housekeeping, decommit, and non-blocking tasks
        tbb::task_arena::constraints e_c;
        e_c.set_core_type(core_types.back());
        e_arena.initialize(e_c);
        e_observer = new (std::nothrow) sf_gc_arena_observer_t(e_arena, SF_CORE_TIER_EFFICIENCY);

        log_info(gc)("RedletTBBArena: Heterogeneous P-Core / E-Core arenas initialized with core_types=%zu", core_types.size());
    } else {
        // Homogeneous symmetric multi-core setup
        tbb::task_arena::constraints h_c;
        auto numa_nodes = tbb::info::numa_nodes();
        if (numa_nodes.size() > 1) {
            h_c.set_numa_id(numa_nodes.front());
        }
        homo_arena.initialize(h_c);
        homo_observer = new (std::nothrow) sf_gc_arena_observer_t(homo_arena, SF_CORE_TIER_ANY);

        log_info(gc)("RedletTBBArena: Homogeneous NUMA-aware task arena initialized");
    }
}

MSIMD_API void sf_gc_tbb_arena_manager_t::destroy() {
    if (!initialized.exchange(false, std::memory_order_acq_rel)) return;

    delete p_observer;    p_observer = nullptr;
    delete e_observer;    e_observer = nullptr;
    delete homo_observer; homo_observer = nullptr;

    if (p_arena.is_active())    p_arena.terminate();
    if (e_arena.is_active())    e_arena.terminate();
    if (homo_arena.is_active()) homo_arena.terminate();

    log_info(gc)("RedletTBBArena: Task arenas destroyed cleanly");
}

#else // REDLET_STANDALONE_BUILD

thread_local SF_custominiVm_core_tier_t tl_sf_core_tier = SF_CORE_TIER_ANY;

MSIMD_API void sf_gc_tbb_arena_manager_t::init() {
    initialized.store(true, std::memory_order_release);
}

MSIMD_API void sf_gc_tbb_arena_manager_t::destroy() {
    initialized.store(false, std::memory_order_release);
}

#endif // REDLET_STANDALONE_BUILD
