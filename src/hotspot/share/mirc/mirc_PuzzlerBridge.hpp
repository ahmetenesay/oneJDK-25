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

#ifndef SHARE_MIRC_MIRC_PUZZLERBRIDGE_HPP
#define SHARE_MIRC_MIRC_PUZZLERBRIDGE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "mirc/mirc_Lvl1.hpp"
#include "mirc/mirc_Patcher.hpp"

class VAC_Table_Snapshot;
struct HS_hotspot_live_header_t;

// ============================================================================
// MIRC_PuzzlerBridge
//
// Symbiotic Bridge Between MIRC (17-Slot SIMD Ladder + Ready Pages) and
// RedjetGC's VFS `/heap/on_heap/puzzle_arena/{stagnant,eval}` + `Pitstop`:
//
//   1.Isolated Code Boot Allocation:
//     Commits Level-1 (`mirc_execution_ready_page/lvl1`) and Puzzler-evolved
//     Level-2 (`mirc_execution_ready_page/lvl2`) code pages inside
//     `RedjetCompressedSpaces` (`hotspot_memory` off-heap domain), strictly
//     disjoint from legacy HotSpot `CodeCache`.
//
//   2. `@PrecisionTarget` IEEE-754 Financial Shield:
//     When `lvl1->is_strictfp && lvl1->target_epsilon == 0.0f`, locks the
//     method to its deterministic non-reassociative IEEE-754 slot (Slot 15
//     `Scalar_Safe` / non-FMA vector rung) and forbids fast-math Slot 16 swaps.
//
//   3. Zero-Pause `SHADOW_RCU` Dockpoint Swap (`-XX:RedjetGCPuzzlerMorph=Shadow`):
//     Executes `RedjetCoach::execute_5step_pipeline` (Holistic Scope -> SSA
//     Atomize -> 17-Rung ISA Swap -> Port Weaving -> 64-Run TSC Duel), stages
//     the winning machine code into `lvl2`, overwrites `simd_slots[16]`
//     Wildcard, and atomically flips the active trampoline at `Dockpoint`
//     while transitioning `Egg` -> `Fledgling` (`has_optimized = 1`).
// ============================================================================

struct MIRC_PuzzlerBridgeResult {
  uint32_t method_id{ 0 };
  bool     precision_shield_locked{ false };
  bool     duel_won_and_swapped{ false };
  bool     used_shadow_rcu{ true };
  uint16_t previous_slot{ 0 };
  uint16_t active_slot{ 0 };
  double   measured_speedup{ 1.0 };
  void*    ready_page_addr{ nullptr };
};

class MIRC_PuzzlerBridge {
private:
  static std::atomic<uint64_t> _lvl1_pages_staged;
  static std::atomic<uint64_t> _lvl2_promotions_won;
  static std::atomic<uint64_t> _precision_shields_enforced;
  static std::atomic<uint64_t> _shadow_rcu_swaps;

public:
  // Allocates an isolated execution-ready page in /heap/off_heap/code_boot/mirc_execution_ready_page/{lvl1,lvl2}
  RJ_API static void* stage_execution_ready_page(uint32_t method_id,
                                                 const void* machine_code,
                                                 size_t code_bytes,
                                                 bool is_lvl2_puzzler_winner) noexcept;

  // Submits a warm method + optional target object (`Egg`) to VFS `puzzle_arena`,
  // enforces `@PrecisionTarget`, runs the 5-Step Coach & 64-Run TSC Duel, and
  // performs Zero-Pause `SHADOW_RCU` Dockpoint trampoline swap if speedup >= 1.05x.
  RJ_API static MIRC_PuzzlerBridgeResult evaluate_and_morph(uint32_t method_id,
                                                            MIRC_Lvl1_Table* lvl1_tab,
                                                            const VAC_Table_Snapshot* vac_tab,
                                                            MIRC_Method_Descriptor* method_desc,
                                                            HS_hotspot_live_header_t* target_obj = nullptr) noexcept;

  // Telemetry accessors
  static uint64_t lvl1_pages_staged() noexcept          { return _lvl1_pages_staged.load(std::memory_order_relaxed); }
  static uint64_t lvl2_promotions_won() noexcept        { return _lvl2_promotions_won.load(std::memory_order_relaxed); }
  static uint64_t precision_shields_enforced() noexcept { return _precision_shields_enforced.load(std::memory_order_relaxed); }
  static uint64_t shadow_rcu_swaps() noexcept           { return _shadow_rcu_swaps.load(std::memory_order_relaxed); }
};

#endif // SHARE_MIRC_MIRC_PUZZLERBRIDGE_HPP
