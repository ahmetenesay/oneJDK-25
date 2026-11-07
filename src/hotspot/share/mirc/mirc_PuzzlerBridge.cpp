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

#include "mirc/mirc_PuzzlerBridge.hpp"
#include "gc/shared/gc_globals.hpp"
#include "memory/allocation.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_REDJETGC
#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/puzzler/redjetCoach.hpp"
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjetUnifiedLogger.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "memory/universe.hpp"
#endif

#include <cstring>

std::atomic<uint64_t> MIRC_PuzzlerBridge::_lvl1_pages_staged{ 0 };
std::atomic<uint64_t> MIRC_PuzzlerBridge::_lvl2_promotions_won{ 0 };
std::atomic<uint64_t> MIRC_PuzzlerBridge::_precision_shields_enforced{ 0 };
std::atomic<uint64_t> MIRC_PuzzlerBridge::_shadow_rcu_swaps{ 0 };

void* MIRC_PuzzlerBridge::stage_execution_ready_page(uint32_t method_id,
                                                     const void* machine_code,
                                                     size_t code_bytes,
                                                     bool is_lvl2_puzzler_winner) noexcept {
  if (code_bytes == 0) return nullptr;

  void* page_mem = nullptr;
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    page_mem = RedjetCompressedSpaces::instance().allocate_mirc_ready_page(code_bytes, is_lvl2_puzzler_winner);
    if (page_mem != nullptr && Universe::is_fully_initialized()) {
      const char* vfs_branch = is_lvl2_puzzler_winner
                                   ? "/heap/off_heap/code_boot/mirc_execution_ready_page/lvl2"
                                   : "/heap/off_heap/code_boot/mirc_execution_ready_page/lvl1";
      RedjetHeap::heap()->vfs_tree()->record_path_alloc(vfs_branch, code_bytes);
    }
  }
#endif

  if (page_mem == nullptr) {
    // Universal GC fallback when running G1 / ZGC / Shenandoah
    page_mem = NEW_C_HEAP_ARRAY(uint8_t, code_bytes, mtCompiler);
  }

  if (page_mem != nullptr && machine_code != nullptr) {
    std::memcpy(page_mem, machine_code, code_bytes);
  }
  if (!is_lvl2_puzzler_winner) {
    _lvl1_pages_staged.fetch_add(1, std::memory_order_relaxed);
  }
  (void)method_id;
  return page_mem;
}

MIRC_PuzzlerBridgeResult MIRC_PuzzlerBridge::evaluate_and_morph(uint32_t method_id,
                                                                MIRC_Lvl1_Table* lvl1_tab,
                                                                const VAC_Table_Snapshot* vac_tab,
                                                                MIRC_Method_Descriptor* method_desc,
                                                                HS_hotspot_live_header_t* target_obj) noexcept {
  MIRC_PuzzlerBridgeResult res{};
  res.method_id = method_id;

  if (lvl1_tab == nullptr) {
    return res;
  }

  res.previous_slot = lvl1_tab->active_idx;
  res.active_slot   = lvl1_tab->active_idx;

  // 1. Enforce @PrecisionTarget / strictfp IEEE-754 Financial & Scientific Shield
  if (lvl1_tab->is_strictfp && lvl1_tab->target_epsilon == 0.0f) {
    res.precision_shield_locked = true;
    // Pin strictly to deterministic IEEE-754 baseline rung (Slot 15 Scalar_Safe)
    const uint16_t safe_slot = 15u;
    lvl1_tab->active_idx     = safe_slot;
    for (uint16_t s = 0; s < 17; ++s) {
      lvl1_tab->simd_ladder[s].is_active = (s == safe_slot) ? 1 : 0;
    }
    if (method_desc != nullptr) {
      (void)MIRC_Atomic_Switch_Slot(method_desc, static_cast<uint8_t>(safe_slot));
    }
    res.active_slot = safe_slot;
    _precision_shields_enforced.fetch_add(1, std::memory_order_relaxed);
    return res;
  }

#if INCLUDE_REDJETGC
  if (UseRedjetGC && !RedjetSCMode && !RedjetGCSCMode) {
    const bool use_shadow_rcu =
        (RedjetFlags::config().strings.puzzler_morph == RedjetPuzzlerMorphMode::SHADOW_RCU);
    res.used_shadow_rcu = use_shadow_rcu;

    // Account evaluation staging inside VFS /heap/on_heap/puzzle_arena/eval
    if (Universe::is_fully_initialized()) {
      RedjetHeap::heap()->vfs_tree()->record_path_alloc(
          "/heap/on_heap/puzzle_arena/eval", sizeof(RedjetMethodScope));
    }

    RedjetMethodScope scope{};
    scope.method_id = method_id;
    scope.no_alias_proven = lvl1_tab->no_alias;

    Redjet5StepResult coach_res{};
    bool pipeline_ok = RedjetCoach::instance().execute_5step_pipeline(
        method_id, &scope, lvl1_tab, vac_tab, &coach_res);

    if (Universe::is_fully_initialized()) {
      RedjetHeap::heap()->vfs_tree()->record_path_free(
          "/heap/on_heap/puzzle_arena/eval", sizeof(RedjetMethodScope));
    }

    if (pipeline_ok && coach_res.step5_hotswap_passed && coach_res.speedup_achieved >= 1.05) {
      // Stage specialized Level-2 code descriptor into isolated VFS mirc_execution_ready_page/lvl2
      void* lvl2_page = stage_execution_ready_page(method_id, scope.uops,
                                                   scope.uop_count * sizeof(RedjetUOp), true);
      res.ready_page_addr = lvl2_page;

      // Populate Slot 16 Puzzler Overwrite Wildcard & Dockpoint RCU swap
      mirc_hotswap_delta_t delta{};
      delta.method_id        = method_id;
      delta.old_simd_idx     = coach_res.active_slot_before;
      delta.new_simd_idx     = 16u; // Slot 16 Puzzler Wildcard
      delta.speedup          = coach_res.speedup_achieved;
      delta.new_code_entry   = lvl2_page;
      delta.slot16_overwrite = true;
      std::strncpy(delta.winner_isa_name, scope.chosen_isa_name, sizeof(delta.winner_isa_name) - 1);

      if (use_shadow_rcu) {
        // Zero-Pause SHADOW_RCU Dockpoint Trampoline Flip
        if (method_desc != nullptr && lvl2_page != nullptr) {
          if (method_desc->slot16_trampoline_addr != nullptr) {
            Overwrite_Slot16(method_desc->slot16_trampoline_addr, lvl2_page);
          }
          method_desc->simd_slots[16] = lvl2_page;
          (void)MIRC_Atomic_Switch_Slot(method_desc, 16u);
        }
        _shadow_rcu_swaps.fetch_add(1, std::memory_order_relaxed);
      }

      // Apply patcher & promote object from Egg -> Fledgling (has_optimized = 1)
      (void)MIRC_Patcher::apply_hotswap(delta, target_obj);

      lvl1_tab->simd_ladder[16].ladder_level = 16u;
      lvl1_tab->simd_ladder[16].is_active    = 1;
      std::strncpy(lvl1_tab->simd_ladder[16].name, scope.chosen_isa_name,
                   sizeof(lvl1_tab->simd_ladder[16].name) - 1);
      lvl1_tab->active_idx = 16u;

      res.duel_won_and_swapped = true;
      res.active_slot          = 16u;
      res.measured_speedup     = coach_res.speedup_achieved;
      _lvl2_promotions_won.fetch_add(1, std::memory_order_relaxed);

      char detail[128];
      snprintf(detail, sizeof(detail),
               "Method %u promoted to MIRC Lvl2 Slot-16 (%s, %.2fx speedup, mode=%s)",
               method_id, scope.chosen_isa_name, coach_res.speedup_achieved,
               use_shadow_rcu ? "SHADOW_RCU" : "EXCISION");
      RedjetUnifiedLogger::instance().log_ergo_decision(
          0, RedjetLogSubsystem::PUZZLER, "mirc_puzzler_swap", detail);
    } else if (Universe::is_fully_initialized()) {
      // Stagnant candidate parked in /heap/on_heap/puzzle_arena/stagnant ledger
      RedjetHeap::heap()->vfs_tree()->record_path_alloc("/heap/on_heap/puzzle_arena/stagnant", 64);
    }
  }
#else
  (void)vac_tab;
  (void)target_obj;
#endif

  return res;
}
