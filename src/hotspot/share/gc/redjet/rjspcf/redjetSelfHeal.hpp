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

#ifndef SHARE_GC_REDJET_REDJETSELFHEAL_HPP
#define SHARE_GC_REDJET_REDJETSELFHEAL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/redjetMotherline.hpp"
#include "gc/redjet/rjnrml/redjetEmptyBox.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

typedef struct HS_custominiVm_self_heal_stats_t {
    uint64_t heap_corruptions_healed;
    uint64_t brooks_cycles_broken;
    uint64_t header_magics_repaired;
    uint64_t emptybox_chains_rebuilt;
    uint64_t bitcode_exceptions_shielded;
    uint64_t code_errors_captured;
} HS_custominiVm_self_heal_stats_t;

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

    bool heal_header(hsLiveHeader* obj);
    size_t scan_and_heal_motherline(RedjetMotherline& ml);
    bool rebuild_emptybox_chain(HS_custominiVm_emptybox_header_t* head, size_t max_scan = 1024);
    bool shield_bitcode_exception(void* code_addr, uint32_t fault_code);
    void check_autonomous_rollback_trigger();
    HS_custominiVm_self_heal_stats_t query_stats() const;
    void reset();
};

extern sacculus_self_heal_engine_t g_hs_self_heal;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API uint64_t HS_custominiVm_self_heal_run_check(void);
MSIMD_API HS_custominiVm_self_heal_stats_t HS_custominiVm_self_heal_query_stats(void);
MSIMD_API void HS_custominiVm_diagnostic_corrupt_tensor(void* tensor_ptr, uint8_t bad_gen, int cyclic_fwd);
MSIMD_API int HS_custominiVm_self_heal_rebuild_emptyboxes(void* head_ptr, size_t max_scan);
MSIMD_API int HS_custominiVm_self_heal_shield_bitcode(void* code_addr, uint32_t fault_code);

#ifdef __cplusplus
}
#endif

namespace HS {
    class SelfHeal {
    public:
        static inline uint64_t RunCheck() {
            return HS_custominiVm_self_heal_run_check();
        }
        static inline HS_custominiVm_self_heal_stats_t QueryStats() {
            return HS_custominiVm_self_heal_query_stats();
        }
        static inline int RebuildEmptyBoxes(void* head, size_t max_scan = 1024) {
            return HS_custominiVm_self_heal_rebuild_emptyboxes(head, max_scan);
        }
        static inline int ShieldBitcode(void* code_addr, uint32_t fault_code) {
            return HS_custominiVm_self_heal_shield_bitcode(code_addr, fault_code);
        }
    };
}

using RedjetSelfHeal = sacculus_self_heal_engine_t;

#endif // SHARE_GC_REDJET_REDJETSELFHEAL_HPP
