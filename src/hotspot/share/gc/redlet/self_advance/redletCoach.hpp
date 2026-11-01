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

#ifndef SHARE_GC_REDLET_SELF_ADVANCE_REDLETCOACH_HPP
#define SHARE_GC_REDLET_SELF_ADVANCE_REDLETCOACH_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include "gc/redlet/redletPuzzleArena.hpp"
#include "gc/redlet/self_advance/redletSelfAdvanceHooks.hpp"
#include "gc/redlet/self_advance/redletSelfAdvanceHeuristics.hpp"
#include "gc/redlet/self_advance/redletSelfAdvancePrecision.hpp"
#include "gc/redlet/self_advance/redletSelfAdvanceOrchestrator.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. REDLET COACH: RUNTIME GC & JAVA WORKLOAD OPTIMIZER                     */
/* ========================================================================= */

class RedletCoach {
private:
    std::atomic<uint64_t> _gc_coach_ticks{ 0 };
    std::atomic<uint64_t> _affinity_coach_ticks{ 0 };
    std::atomic<uint64_t> _self_advance_ticks{ 0 };

    RedletPuzzleArena* _puzzle_arena{ nullptr };
    mutable std::mutex _coach_lock;

public:
    RedletCoach();
    ~RedletCoach() = default;

    static RedletCoach& instance();

    void bind_puzzle_arena(RedletPuzzleArena* arena) noexcept {
        _puzzle_arena = arena;
    }

    RedletPuzzleArena* puzzle_arena() const noexcept {
        return _puzzle_arena;
    }

    // 1. Coach GC internal operations (memzero, memcpy, bitmap_scan) based on CPU hardware
    MSIMD_API void coach_gc_operations();

    // 2. Coach thread affinity and P-Core / E-Core distribution for Java threads
    MSIMD_API void coach_thread_affinity();

    // 3. Periodic self-advancement evaluation of GC metrics and RMRP dynamic tuning
    MSIMD_API void self_advance_evaluate();

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

extern RedletCoach g_redlet_coach;

/* ========================================================================= */
/* 2. CANONICAL C EXPORTS                                                    */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API void SF_coach_gc_operations(void);
MSIMD_API void SF_coach_thread_affinity(void);
MSIMD_API void SF_coach_self_advance_evaluate(void);
MSIMD_API void SF_coach_reset(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_SELF_ADVANCE_REDLETCOACH_HPP
