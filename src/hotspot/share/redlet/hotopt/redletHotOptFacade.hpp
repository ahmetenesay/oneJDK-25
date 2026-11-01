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

#ifndef SHARE_REDLET_HOTOPT_REDLETHOTOPTFACADE_HPP
#define SHARE_REDLET_HOTOPT_REDLETHOTOPTFACADE_HPP

/*
 * Architectural Facade for Redlet Hot Optimization: RedletCoach Self-Advancement,
 * Egg -> Pitstop -> Fledgling Non-Blocking ISA Hot-Swap, and Batch Egg Promotion.
 * Strictly C++14-safe (avoids transitive inclusion of C++20-only or raw malloc headers).
 */

#ifdef REDLET_STANDALONE_BUILD
#include <stddef.h>
#include <stdint.h>
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include <stddef.h>
#include <stdint.h>
#endif

#include "gc/redlet/redletBatchScope.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

struct SF_custominiVm_live_header_t;
class RedletPitstop;
class RedletCoach;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API void SF_coach_gc_operations(void);
MSIMD_API void SF_coach_thread_affinity(void);
MSIMD_API void SF_coach_self_advance_evaluate(void);
MSIMD_API void SF_coach_reset(void);

#ifdef __cplusplus
}
#endif

#ifdef REDLET_STANDALONE_BUILD
class RedletHotOptFacade {
#else
class RedletHotOptFacade : public AllStatic {
#endif
public:
  static inline void coach_gc_operations() {
    SF_coach_gc_operations();
  }

  static inline void coach_thread_affinity() {
    SF_coach_thread_affinity();
  }

  static inline void evaluate_self_advance() {
    SF_coach_self_advance_evaluate();
  }

  static inline void reset_coach() {
    SF_coach_reset();
  }

  static inline size_t batch_promote_eggs_to_fledglings(void** egg_hdrs, size_t count) {
    return RedletBatchScopeManager::batch_optimize_egg_objects(egg_hdrs, count);
  }

  static inline size_t promote_single_egg(void* egg_hdr) {
    if (egg_hdr == NULL) return 0u;
    void* arr[1] = { egg_hdr };
    return RedletBatchScopeManager::batch_optimize_egg_objects(arr, 1u);
  }
};

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
#include "gc/redlet/redletPitstop.hpp"
#include "gc/redlet/self_advance/redletCoach.hpp"

class RedletHotOptDirect {
public:
  static inline RedletCoach& coach() noexcept {
    return RedletCoach::instance();
  }

  static inline bool pitstop_morph_to_fledgling(RedletPitstop& pitstop,
                                                SF_custominiVm_live_header_t* obj,
                                                uint16_t target_simd_idx,
                                                uint64_t cycles_gained = 0ULL) {
    return pitstop.morph_isa_to_fledgling(obj, target_simd_idx, cycles_gained);
  }
};
#endif

#endif // SHARE_REDLET_HOTOPT_REDLETHOTOPTFACADE_HPP
