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

#ifndef SHARE_OPTO_MIRC_MIRC_PATCHER_HPP
#define SHARE_OPTO_MIRC_MIRC_PATCHER_HPP

#include <cstdint>
#include <cstddef>
#include "opto/mirc/mirc_Lvl1.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

struct HS_custominiVm_live_header_t;

// ============================================================================
// MIRC_Patcher: Non-STW Hot-Swap Binder & Object Sealer
// ============================================================================

class MIRC_Patcher {
public:
  // Applies mirc_hotswap_delta_t without Stop-The-World (zero STW),
  // updates simd_table[17] (including Slot 16 dynamic overwrite),
  // and transitions target object to Fledgling (has_optimized = 1).
  MSIMD_API static bool apply_hotswap(const mirc_hotswap_delta_t& delta, HS_custominiVm_live_header_t* obj);
};

#endif // SHARE_OPTO_MIRC_MIRC_PATCHER_HPP
