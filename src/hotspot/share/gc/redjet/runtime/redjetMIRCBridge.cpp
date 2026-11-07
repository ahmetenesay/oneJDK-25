/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation. Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
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

#include "gc/redjet/runtime/redjetMIRCBridge.hpp"
#include "ci/ciMethod.hpp"
#include "mirc/mirc.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "gc/redjet/puzzler/redjetCoach.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/shared/gc_globals.hpp"

bool RedjetMIRCBridge::is_mirc_enabled() {
  return UseMIRC && UseRedjetGC;
}

bool RedjetMIRCBridge::should_use_mirc(ciMethod* method) {
  if (!is_mirc_enabled()) {
    return false;
  }
  if (method == nullptr || !method->is_loaded()) {
    return false;
  }
  return true;
}

bool RedjetMIRCBridge::register_mirc_duel(ciMethod* method, MIRC_Lvl1_Table* lvl1_tab, Compile* C) {
  if (method == nullptr || lvl1_tab == nullptr) {
    return false;
  }

  RedjetPuzzleArena* arena = hs_get_default_puzzle_arena();
  if (arena == nullptr) {
    return false;
  }

  mirc_hotswap_delta_t delta;
  if (arena->submit_duel(nullptr, lvl1_tab, &delta)) {
    arena->apply_hotswap_delta_2points(delta);
    return true;
  }
  return false;
}

void RedjetMIRCBridge::notify_mirc_nmethod_installed(address code_begin, size_t code_size, uint32_t method_id) {
  // Ensure ICache coherence across all cores via PointCoordinator 2-Point rendezvous
  RedjetPointCoordinator::sync_point();
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, method_id, 0,
                 (uint32_t)code_size, 0, 0, 0, 0, "MIRC_NMETHOD_INSTALLED");
}

bool RedjetMIRCBridge::trigger_puzzler_duel(uint32_t method_id, void* old_entry, void* new_entry) {
  RedjetCoach coach;
  coach.init(hs_get_default_puzzle_arena());
  return coach.evaluate_and_duel(method_id, old_entry, new_entry);
}
