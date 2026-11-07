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

#ifndef SHARE_MIRC_MIRC_LVL2_HPP
#define SHARE_MIRC_MIRC_LVL2_HPP

#include <cstdint>
#include <cstddef>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

class Compilation;
class Compile;
class CodeBuffer;
struct MIRC_Lvl1_Table;

// ============================================================================
// MIRC_Lvl2: Direct Native Binary Machine Code Emitter
// ============================================================================

class MIRC_Lvl2 {
public:
  // Directly emits initial native binary machine code (nmethod CodeBuffer)
  // from MIRC_Lvl1_Table for mutator execution under Egg state (has_optimized = 0).
  // Configures entry offsets on Compilation, builds stack frame, emits vector loop,
  // flushes icache, and returns compiled frame size in 32-bit words.
  RJ_API static int emit_machine_code(Compilation* comp, const MIRC_Lvl1_Table* lvl1, CodeBuffer* cbuf);

  // Directly emits C2 native binary machine code from MIRC_Lvl1_Table into C2 CodeBuffer
  RJ_API static int emit_c2_machine_code(Compile* C, const MIRC_Lvl1_Table* lvl1, CodeBuffer* cbuf);
};

#endif // SHARE_MIRC_MIRC_LVL2_HPP
