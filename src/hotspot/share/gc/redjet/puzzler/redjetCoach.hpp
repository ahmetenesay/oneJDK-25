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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETCOACH_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETCOACH_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>

#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvanceHooks.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvanceHeuristics.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvancePrecision.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvanceOrchestrator.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. REDJET COACH: RUNTIME GC & JAVA WORKLOAD OPTIMIZER                     */
/* ========================================================================= */

class RedjetCoach {
private:
    std::atomic<uint64_t> _gc_coach_ticks{ 0 };
    std::atomic<uint64_t> _affinity_coach_ticks{ 0 };
    std::atomic<uint64_t> _self_advance_ticks{ 0 };

    RedjetPuzzleArena* _puzzle_arena{ nullptr };
    mutable std::mutex _coach_lock;

public:
    RedjetCoach();
    ~RedjetCoach() = default;

    static RedjetCoach& instance();

    void bind_puzzle_arena(RedjetPuzzleArena* arena) noexcept {
        _puzzle_arena = arena;
    }

    RedjetPuzzleArena* puzzle_arena() const noexcept {
        return _puzzle_arena;
    }

    // 1. Coach GC internal operations (memzero, memcpy, bitmap_scan) based on CPU hardware
    RJ_API void coach_gc_operations();

    // 2. Coach thread affinity and P-Core / E-Core distribution for Java threads
    RJ_API void coach_thread_affinity();

    // 3. Periodic self-advancement evaluation of GC metrics and RMRP dynamic tuning
    RJ_API void self_advance_evaluate();

    uint64_t get_gc_coach_ticks() const noexcept {
        return _gc_coach_ticks.load(std::memory_order_relaxed);
    }

    uint64_t get_affinity_coach_ticks() const noexcept {
        return _affinity_coach_ticks.load(std::memory_order_relaxed);
    }

    uint64_t get_self_advance_ticks() const noexcept {
        return _self_advance_ticks.load(std::memory_order_relaxed);
    }

    void reset();
};

extern RedjetCoach g_redjet_coach;

/* ========================================================================= */
/* 2. CANONICAL C EXPORTS                                                    */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void SF_coach_gc_operations(void);
RJ_API void SF_coach_thread_affinity(void);
RJ_API void SF_coach_self_advance_evaluate(void);
RJ_API void SF_coach_reset(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_PUZZLER_REDJETCOACH_HPP