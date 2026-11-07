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

#include "gc/redjet/runtime/redjetVACBridge.hpp"
#include "vac/c1_Compilation.hpp"
#include "vac/c1_SortOpt.hpp"
#include "vac/c1_globals.hpp"
#include "ci/ciMethod.hpp"
#include "mirc/mirc.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/shared/gc_globals.hpp"

bool RedjetVACBridge::is_vac_enabled() {
  return UseVAC && UseRedjetGC;
}

bool RedjetVACBridge::should_use_vac(ciMethod* method) {
  if (!is_vac_enabled()) {
    return false;
  }
  if (method == nullptr || !method->is_loaded()) {
    return false;
  }
  // All methods or batch/nested/bypass methods in oneJDK-25 with RedjetGC
  return true;
}

int RedjetVACBridge::compile_via_vac_and_mirc(Compilation* comp, VAC_Table_Snapshot* snapshot) {
  if (comp == nullptr || snapshot == nullptr) {
    return -1;
  }

  // 1. MIRC Level 1: Synthesize 17-Slot Ladder & Elastic Loop Template
  MIRC_Lvl1_Table* lvl1_tab = MIRC_Lvl1::synthesize(snapshot);
  if (lvl1_tab == nullptr) {
    return -1;
  }

  // 2. MIRC Level 2: Directly Emit Initial Native Machine Code (Egg State, has_optimized = 0)
  int frame_size = MIRC_Lvl2::emit_machine_code(comp, lvl1_tab, comp->code());
  if (frame_size < 0) {
    return -1;
  }

  // 3. Register Duel with Puzzler Arena (Dual Inputs: VAC_Table + MIRC_Lvl1_Table)
  mirc_hotswap_delta_t delta;
  RedjetPuzzleArena* arena = hs_get_default_puzzle_arena();
  if (arena != nullptr) {
    if (arena->submit_duel(snapshot, lvl1_tab, &delta)) {
      arena->apply_hotswap_delta_2points(delta);
    }
  }

  // 4. Log VAC & MIRC Compilation Telemetry
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, lvl1_tab->method_id,
                 lvl1_tab->active_idx, 0, 0, 0, 0,
                 (uint32_t)lvl1_tab->simulation_cycles, lvl1_tab->method_name);

  return frame_size;
}
