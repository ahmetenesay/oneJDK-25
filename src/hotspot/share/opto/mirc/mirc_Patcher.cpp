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

#include "opto/mirc/mirc_Patcher.hpp"
#include "gc/redjet/rjspcf/redjetMotherline.hpp"
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "runtime/icache.hpp"
#include <cstring>

// ============================================================================
// MIRC_Patcher Implementation: Non-STW Hot-Swap Binder
// ============================================================================

bool MIRC_Patcher::apply_hotswap(const mirc_hotswap_delta_t& delta, HS_custominiVm_live_header_t* obj) {
  // 1. Binary Patch Code Buffer if patch target is specified
  if (delta.patch_target_pc != nullptr && delta.new_code_entry != nullptr && delta.patch_size > 0) {
    memcpy(delta.patch_target_pc, delta.new_code_entry, delta.patch_size);
    ICache::invalidate_range(static_cast<address>(delta.patch_target_pc), delta.patch_size);
  }

  if (obj == nullptr || obj->hsMN != HS_LIVE_MAGIC) {
    return true; // Patch applied at code level
  }

  // 2. Apply Hot-Swap Patch & SIMD Ladder Update
  if (delta.slot16_overwrite) {
    // Dynamic candidate discovered by Puzzler written directly to Slot 16
    strncpy(obj->simd_table[16].name, delta.winner_isa_name, sizeof(obj->simd_table[16].name) - 1);
    obj->simd_table[16].name[sizeof(obj->simd_table[16].name) - 1] = '\0';
    obj->simd_table[16].ladder_level = 16;
    obj->simd_table[16].is_active = 1;
    obj->active_simd_idx = 16;
    for (uint16_t i = 0; i < 16; ++i) {
      obj->simd_table[i].is_active = 0;
    }
  } else {
    // Existing candidate rung selected
    obj->active_simd_idx = delta.new_simd_idx;
    for (uint16_t i = 0; i < 17; ++i) {
      obj->simd_table[i].is_active = (i == delta.new_simd_idx) ? 1 : 0;
    }
  }

  // 3. Reseal as Fledgling (has_optimized = 1)
  obj->cycles_gained.store(delta.cycles_gained, std::memory_order_release);
  obj->last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
  obj->opt_success = 1;
  obj->has_optimized.store(1, std::memory_order_release);

  // 4. Verify and write Tail Canary
  if (obj->memory_footprint.load(std::memory_order_relaxed) > 0 && !obj->isTailCanaryValid()) {
    obj->writeTailCanary();
  }

  // 5. Telemetry Emission
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, delta.method_id,
                 obj->active_simd_idx, obj->generation, obj->region,
                 0, 0, (uint32_t)delta.cycles_gained, delta.winner_isa_name);

  return true;
}
