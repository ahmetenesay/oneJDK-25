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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETMIRCBRIDGE_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETMIRCBRIDGE_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class ciMethod;
class Compile;
class MIRC_Lvl1_Table;

// ============================================================================
// RedjetMIRCBridge: Connects C2/MIRC Compiler Engine with Redjet Puzzler Arena
// ============================================================================

class RedjetMIRCBridge : public AllStatic {
public:
  static bool is_mirc_enabled();
  static bool should_use_mirc(ciMethod* method);

  // Bridges MIRC Level 1 & Level 2 compilation output with RedjetPuzzleArena
  // on-heap duel optimizer located at /heap/on_heap/puzzle_arena
  static bool register_mirc_duel(ciMethod* method, MIRC_Lvl1_Table* lvl1_tab, Compile* C);

  // Synchronizes MIRC compiled nmethod with 2-Point coordination
  static void notify_mirc_nmethod_installed(address code_begin, size_t code_size, uint32_t method_id);

  // Triggers background duel optimization via Puzzler Coach
  static bool trigger_puzzler_duel(uint32_t method_id, void* old_entry, void* new_entry);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETMIRCBRIDGE_HPP
