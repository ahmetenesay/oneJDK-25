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

// ============================================================================
// Puzzler Mod 2: 5-Step Production Line Structures (PLAN.md Section 8)
// Step 1: Holistic Scope (NoAlias verification, scalar replacement)
// Step 2: Atomize SSA (uOP breakdown)
// Step 3: 17-Rung ISA Swap (TableGen ladder, synthetic Barrett, VZEROUPPER)
// Step 4: Reassembly & Port Saturation Weaving (4 ALU + 2 Load ports, PILF loop)
// Step 5: 64-run TSC-Delta Hotswap (duel, Bit-Exact IEEE 754, atomic Slot 16 patch)
// ============================================================================

enum class RedjetUOpKind : uint8_t {
  UOP_LOAD        = 0,
  UOP_ALU_ADD     = 1,
  UOP_ALU_SUB     = 2,
  UOP_ALU_AND     = 3,
  UOP_ALU_OR      = 4,
  UOP_ALU_XOR     = 5,
  UOP_MUL         = 6,
  UOP_DIV         = 7,
  UOP_BARRETT_MOD = 8, // Synthetic Barrett reduction
  UOP_STORE       = 9,
  UOP_BRANCH_PILF = 10, // sub rdx, 8; jnz loop_head
  UOP_VZEROUPPER  = 11  // AVX/SSE transition fence
};

struct RedjetUOp {
  RedjetUOpKind kind{ RedjetUOpKind::UOP_ALU_ADD };
  uint32_t      dst_reg{ 0 };
  uint32_t      src_reg1{ 0 };
  uint32_t      src_reg2{ 0 };
  uint64_t      immediate{ 0 };
  uint32_t      port_mask{ 0x01 | 0x02 | 0x20 | 0x40 }; // 4 ALU ports (P0, P1, P5, P6)
  uint16_t      latency{ 1 };
  uint16_t      simd_width_bytes{ 64 };
};

struct RedjetMethodScope {
  static constexpr size_t MAX_UOPS = 512;
  static constexpr size_t MAX_POINTERS = 64;

  uint32_t    method_id{ 0 };
  uintptr_t   pointer_bases[MAX_POINTERS]{ 0 };
  size_t      pointer_spans[MAX_POINTERS]{ 0 };
  size_t      pointer_count{ 0 };
  bool        no_alias_proven{ false };
  bool        scalar_replacement_applied{ false };

  RedjetUOp   uops[MAX_UOPS];
  size_t      uop_count{ 0 };

  uint16_t    chosen_isa_rung{ 0 };
  char        chosen_isa_name[32]{ 0 };
  bool        barrett_reduction_emitted{ false };
  bool        vzeroupper_emitted{ false };
  bool        port_saturation_woven{ false };
  bool        pilf_backedge_woven{ false };

  void reset() noexcept {
    method_id = 0;
    pointer_count = 0;
    no_alias_proven = false;
    scalar_replacement_applied = false;
    uop_count = 0;
    chosen_isa_rung = 0;
    chosen_isa_name[0] = '\0';
    barrett_reduction_emitted = false;
    vzeroupper_emitted = false;
    port_saturation_woven = false;
    pilf_backedge_woven = false;
  }
};

struct Redjet5StepResult {
  uint32_t method_id{ 0 };
  bool     step1_holistic_passed{ false };
  bool     step2_atomize_passed{ false };
  bool     step3_isa_swap_passed{ false };
  bool     step4_weaving_passed{ false };
  bool     step5_hotswap_passed{ false };
  uint16_t active_slot_before{ 0 };
  uint16_t active_slot_after{ 0 };
  double   speedup_achieved{ 1.0 };
};

class RedjetCoach {
private:
    std::atomic<uint64_t> _gc_coach_ticks{ 0 };
    std::atomic<uint64_t> _affinity_coach_ticks{ 0 };
    std::atomic<uint64_t> _self_advance_ticks{ 0 };

    // 5-Step Production Line Telemetry
    std::atomic<uint64_t> _total_pipeline_runs{ 0 };
    std::atomic<uint64_t> _step1_holistic_successes{ 0 };
    std::atomic<uint64_t> _step2_uops_atomized{ 0 };
    std::atomic<uint64_t> _step3_isa_swaps{ 0 };
    std::atomic<uint64_t> _step4_weavings_completed{ 0 };
    std::atomic<uint64_t> _step5_hotswaps_won{ 0 };

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

    // =========================================================================
    // 4. Puzzler Mod 2: 5-Step Production Line Engine
    // =========================================================================
    RJ_API bool execute_5step_pipeline(uint32_t method_id,
                                       RedjetMethodScope* scope,
                                       MIRC_Lvl1_Table* lvl1_tab,
                                       const VAC_Table_Snapshot* vac_tab,
                                       Redjet5StepResult* out_result);

    RJ_API bool step1_holistic_scope(RedjetMethodScope* scope);
    RJ_API bool step2_atomize_ssa(RedjetMethodScope* scope);
    RJ_API bool step3_17rung_isa_swap(RedjetMethodScope* scope, MIRC_Lvl1_Table* lvl1_tab);
    RJ_API bool step4_reassembly_port_weaving(RedjetMethodScope* scope);
    RJ_API bool step5_64run_tsc_hotswap(uint32_t method_id,
                                        MIRC_Lvl1_Table* lvl1_tab,
                                        const VAC_Table_Snapshot* vac_tab,
                                        Redjet5StepResult* out_result);

    uint64_t get_gc_coach_ticks() const noexcept {
        return _gc_coach_ticks.load(std::memory_order_relaxed);
    }

    uint64_t get_affinity_coach_ticks() const noexcept {
        return _affinity_coach_ticks.load(std::memory_order_relaxed);
    }

    uint64_t get_self_advance_ticks() const noexcept {
        return _self_advance_ticks.load(std::memory_order_relaxed);
    }

    uint64_t total_pipeline_runs() const noexcept {
        return _total_pipeline_runs.load(std::memory_order_relaxed);
    }

    uint64_t step1_successes() const noexcept {
        return _step1_holistic_successes.load(std::memory_order_relaxed);
    }

    uint64_t step2_uops() const noexcept {
        return _step2_uops_atomized.load(std::memory_order_relaxed);
    }

    uint64_t step3_swaps() const noexcept {
        return _step3_isa_swaps.load(std::memory_order_relaxed);
    }

    uint64_t step4_weavings() const noexcept {
        return _step4_weavings_completed.load(std::memory_order_relaxed);
    }

    uint64_t step5_hotswaps() const noexcept {
        return _step5_hotswaps_won.load(std::memory_order_relaxed);
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

RJ_API void HS_coach_gc_operations(void);
RJ_API void HS_coach_thread_affinity(void);
RJ_API void HS_coach_self_advance_evaluate(void);
RJ_API void HS_coach_reset(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_PUZZLER_REDJETCOACH_HPP