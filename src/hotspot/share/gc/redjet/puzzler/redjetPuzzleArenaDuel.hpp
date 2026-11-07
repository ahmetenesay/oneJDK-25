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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETPUZZLEARENADUEL_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETPUZZLEARENADUEL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "mirc/mirc.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// RedjetPuzzleArenaDuel: NMethod-Level Dual Execution & 64-Iteration Profiler
// Connects RedjetMIRCBridge and RedjetPuzzleArena with Atomic Slot 16 Hot-Swap
// ============================================================================

struct alignas(64) RedjetDuelProfile64 {
    static constexpr size_t ITERATIONS = 64;
    static constexpr size_t OUTLIER_TRIM_COUNT = 6; // 10% trimmed from top and bottom (PLAN.md Sec 8.8)

    uint64_t tsc_deltas[ITERATIONS]{ 0 };
    uint64_t inst_retired[ITERATIONS]{ 0 };
    uint64_t l1d_misses[ITERATIONS]{ 0 };

    uint64_t mean_tsc{ 0 };
    uint64_t min_tsc{ UINT64_MAX };
    uint64_t max_tsc{ 0 };
    uint64_t median_tsc{ 0 };
    uint64_t trimmed_mean_tsc{ 0 }; // Trims top and bottom 10% (6 samples each)
    uint64_t mean_inst_retired{ 0 };
    uint64_t mean_l1d_misses{ 0 };
    double   std_dev{ 0.0 };

    void analyze() noexcept;
};

struct alignas(64) RedjetNMethodDuelResult {
    uint32_t method_id{ 0 };
    uint16_t old_active_slot{ 0 };
    uint16_t winning_slot{ 0 };
    char     winning_isa_name[32]{ 0 };
    uint64_t baseline_cycles{ 0 };
    uint64_t winning_cycles{ 0 };
    uint64_t cycles_gained{ 0 };
    uint64_t baseline_inst_retired{ 0 };
    uint64_t winning_inst_retired{ 0 };
    uint64_t baseline_l1d_misses{ 0 };
    uint64_t winning_l1d_misses{ 0 };
    double   speedup{ 1.0 };
    bool     precision_verified{ false };
    bool     hotswap_performed{ false };
};

class RedjetPuzzleArenaDuelEngine {
public:
    static constexpr size_t DUEL_SAMPLE_ELEMS = 4096;
    static constexpr double MIN_WINNING_SPEEDUP = 1.05; // 5% minimum speedup required

private:
    static std::atomic<uint64_t> _total_duels_executed;
    static std::atomic<uint64_t> _total_hotswaps_performed;
    static std::atomic<uint64_t> _total_precision_rejections;

public:
    // Core NMethod Duel Execution: Runs 64-iteration TSC-Delta profiling across
    // candidate rungs in MIRC_Lvl1_Table, verifies precision, and performs atomic Slot 16 hot-swap
    RJ_API static bool execute_nmethod_duel(
        uint32_t method_id,
        MIRC_Lvl1_Table* lvl1_tab,
        const VAC_Table_Snapshot* vac_tab,
        mirc_hotswap_delta_t* out_delta = nullptr,
        RedjetNMethodDuelResult* out_result = nullptr);

    // 64-Iteration TSC-Delta Profiling Harness
    RJ_API static void profile_candidate_64(
        const char* isa_name,
        const float* src_a,
        const float* src_b,
        float* cand_out,
        size_t count,
        RedjetDuelProfile64* out_profile) noexcept;

    // Atomic Slot 16 Hot-Swap & 2-Point Rendezvous Synchronization
    RJ_API static bool perform_atomic_slot16_hotswap(
        MIRC_Lvl1_Table* lvl1_tab,
        const char* winner_isa_name,
        uint16_t old_active_idx,
        uint64_t baseline_tsc,
        uint64_t winning_tsc,
        double speedup,
        mirc_hotswap_delta_t* out_delta = nullptr);

    // Telemetry
    static uint64_t total_duels() noexcept { return _total_duels_executed.load(std::memory_order_relaxed); }
    static uint64_t total_hotswaps() noexcept { return _total_hotswaps_performed.load(std::memory_order_relaxed); }
    static uint64_t precision_rejections() noexcept { return _total_precision_rejections.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETPUZZLEARENADUEL_HPP
