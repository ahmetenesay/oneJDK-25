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

#ifndef SHARE_GC_REDLET_REDLETSELFHEAL_HPP
#define SHARE_GC_REDLET_REDLETSELFHEAL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletMotherline.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

typedef struct SF_custominiVm_self_heal_stats_t {
    uint64_t heap_corruptions_healed;
    uint64_t brooks_cycles_broken;
    uint64_t header_magics_repaired;
    uint64_t emptybox_chains_rebuilt;
    uint64_t bitcode_exceptions_shielded;
    uint64_t code_errors_captured;
} SF_custominiVm_self_heal_stats_t;

/* ========================================================================= */
/* 1. SELF-HEALING ENGINE (sacculus_self_heal_engine_t)                      */
/* ========================================================================= */

struct sacculus_self_heal_engine_t {
    std::atomic<uint64_t> heap_corruptions_healed{ 0 };
    std::atomic<uint64_t> brooks_cycles_broken{ 0 };
    std::atomic<uint64_t> header_magics_repaired{ 0 };
    std::atomic<uint64_t> emptybox_chains_rebuilt{ 0 };
    std::atomic<uint64_t> bitcode_exceptions_shielded{ 0 };
    std::atomic<uint64_t> code_errors_captured{ 0 };

    bool heal_header(sfLiveHeader* obj);
    size_t scan_and_heal_motherline(RedletMotherline& ml);
    SF_custominiVm_self_heal_stats_t query_stats() const;
    void reset();
};

extern sacculus_self_heal_engine_t g_sf_self_heal;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API uint64_t SF_custominiVm_self_heal_run_check(void);
MSIMD_API SF_custominiVm_self_heal_stats_t SF_custominiVm_self_heal_query_stats(void);
MSIMD_API void SF_custominiVm_diagnostic_corrupt_tensor(void* tensor_ptr, uint8_t bad_gen, int cyclic_fwd);

#ifdef __cplusplus
}
#endif

namespace SF {
    class SelfHeal {
    public:
        static inline uint64_t RunCheck() {
            return SF_custominiVm_self_heal_run_check();
        }
        static inline SF_custominiVm_self_heal_stats_t QueryStats() {
            return SF_custominiVm_self_heal_query_stats();
        }
    };
}

using RedletSelfHeal = sacculus_self_heal_engine_t;

#endif // SHARE_GC_REDLET_REDLETSELFHEAL_HPP
