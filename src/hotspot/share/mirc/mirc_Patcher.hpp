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

#ifndef SHARE_MIRC_MIRC_PATCHER_HPP
#define SHARE_MIRC_MIRC_PATCHER_HPP

#include <cstdint>
#include <cstddef>
#include "mirc/mirc_Lvl1.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

struct HS_hotspot_live_header_t;

// ============================================================================
// Canonical MIRC Method Descriptor for 17-Slot SIMD Ladder & Dynamic Patcher
// ============================================================================

struct MIRC_Method_Descriptor {
  uint32_t method_id;
  void*    code_entry_point;
  void*    slot16_trampoline_addr;
  uint8_t  active_slot_index;
  uint64_t tsc_execution_cycles;
  void*    simd_slots[17];
};

RJ_API bool MIRC_Atomic_Switch_Slot(MIRC_Method_Descriptor* desc, uint8_t target_slot_index);
RJ_API void Overwrite_Slot16(void* slot16_addr, void* new_target_slot);

// ============================================================================
// MIRC_Patcher: Non-STW Hot-Swap Binder & Object Sealer
// ============================================================================

class MIRC_Patcher {
public:
  // Applies mirc_hotswap_delta_t without Stop-The-World (zero STW),
  // updates simd_table[17] (including Slot 16 dynamic overwrite),
  // and transitions target object to Fledgling (has_optimized = 1).
  RJ_API static bool apply_hotswap(const mirc_hotswap_delta_t& delta, HS_hotspot_live_header_t* obj);
};

#endif // SHARE_MIRC_MIRC_PATCHER_HPP
