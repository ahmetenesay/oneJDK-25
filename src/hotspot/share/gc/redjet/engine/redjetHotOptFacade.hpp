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

#ifndef SHARE_REDJET_HOTOPT_REDJETHOTOPTFACADE_HPP
#define SHARE_REDJET_HOTOPT_REDJETHOTOPTFACADE_HPP

/*
 * Architectural Facade for Redjet Hot Optimization: RedjetCoach Self-Advancement,
 * Egg -> Pitstop -> Fledgling Non-Blocking ISA Hot-Swap, and Batch Egg Promotion.
 * Strictly C++14-safe (avoids transitive inclusion of C++20-only or raw malloc headers).
 */

#ifdef REDJET_STANDALONE_BUILD
#include <stddef.h>
#include <stdint.h>
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include <stddef.h>
#include <stdint.h>
#endif

#include "gc/redjet/engine/redjetBatchScope.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

struct HS_hotspot_live_header_t;
class RedjetPitstop;
class RedjetCoach;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void HS_coach_gc_operations(void);
RJ_API void HS_coach_thread_affinity(void);
RJ_API void HS_coach_self_advance_evaluate(void);
RJ_API void HS_coach_reset(void);

#ifdef __cplusplus
}
#endif

#ifdef REDJET_STANDALONE_BUILD
class RedjetHotOptFacade {
#else
class RedjetHotOptFacade : public AllStatic {
#endif
public:
  static inline void coach_gc_operations() {
    HS_coach_gc_operations();
  }

  static inline void coach_thread_affinity() {
    HS_coach_thread_affinity();
  }

  static inline void evaluate_self_advance() {
    HS_coach_self_advance_evaluate();
  }

  static inline void reset_coach() {
    HS_coach_reset();
  }

  static inline size_t batch_promote_eggs_to_fledglings(void** egg_hdrs, size_t count) {
    return RedjetBatchScopeManager::batch_optimize_egg_objects(egg_hdrs, count);
  }

  static inline size_t promote_single_egg(void* egg_hdr) {
    if (egg_hdr == NULL) return 0u;
    void* arr[1] = { egg_hdr };
    return RedjetBatchScopeManager::batch_optimize_egg_objects(arr, 1u);
  }
};

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
#include "gc/redjet/engine/redjetPitstop.hpp"
#include "gc/redjet/puzzler/redjetCoach.hpp"

class RedjetHotOptDirect {
public:
  static inline RedjetCoach& coach() noexcept {
    return RedjetCoach::instance();
  }

  static inline bool pitstop_morph_to_fledgling(RedjetPitstop& pitstop,
                                                HS_hotspot_live_header_t* obj,
                                                uint16_t target_simd_idx,
                                                uint64_t cycles_gained = 0ULL) {
    return pitstop.morph_isa_to_fledgling(obj, target_simd_idx, cycles_gained);
  }
};
#endif

#endif // SHARE_REDJET_HOTOPT_REDJETHOTOPTFACADE_HPP
