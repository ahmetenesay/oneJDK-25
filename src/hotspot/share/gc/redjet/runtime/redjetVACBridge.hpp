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

#ifndef SHARE_GC_REDJET_C1_REDJETVACBRIDGE_HPP
#define SHARE_GC_REDJET_C1_REDJETVACBRIDGE_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class ciMethod;
class Compilation;
class VAC_Table_Snapshot;

// ============================================================================
// RedjetVACBridge: Connects C1 VAC (SortOpt) with MIRC Runtime Engine
// ============================================================================

class RedjetVACBridge : public AllStatic {
public:
  // Checks if VAC mode is active for the current JVM / compilation unit
  static bool is_vac_enabled();

  // Evaluates whether the specific target method should be compiled via VAC & MIRC
  static bool should_use_vac(ciMethod* method);

  // Core Handoff: Decouples C1 from native binary code emission;
  // invokes MIRC Level 1 runtime codegen (4-step simulation + 17-slot ladder synthesis),
  // MIRC Level 2 machine code emission (Egg state, has_optimized = 0),
  // and registers VAC_Table_Snapshot + MIRC_Lvl1_Table in RedjetPuzzleArena for background duels.
  // Returns compiled frame size, or -1 if compilation fell back to classic C1.
  static int compile_via_vac_and_mirc(Compilation* comp, VAC_Table_Snapshot* snapshot);
};

#endif // SHARE_GC_REDJET_C1_REDJETVACBRIDGE_HPP
