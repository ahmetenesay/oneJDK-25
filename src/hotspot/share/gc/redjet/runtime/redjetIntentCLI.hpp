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

#ifndef SHARE_GC_REDJET_REDJETINTENTCLI_HPP
#define SHARE_GC_REDJET_REDJETINTENTCLI_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/simd/redjetSIMD.hpp"
#include "gc/redjet/simd/redjetALU.hpp"
#include "gc/redjet/allocator/redjetPagefile.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"

#include "gc/redjet/runtime/redjetFlags.hpp"

/* ========================================================================= */
/* 1. CANONICAL CONFIGURATION ENUMS & STRUCTURE ALIASES                      */
/* ========================================================================= */

using hs_gc_mode_e               = RedjetGCMode;
using hs_gc_operation_facility_e = RedjetGCOperationFacility;
using hs_snatch_rebate_mode_e    = RedjetSnatchRebateMode;
using hs_gc_heuristic_e          = RedjetGCHeuristic;
using hs_scgc_type_e             = RedjetSCGCType;
using hs_rmrp_method_e           = RedjetRMRPMethod;
using hs_global_volatility_e     = RedjetGlobalVolatility;
using hs_puzzler_morph_e         = RedjetPuzzlerMorphMode;
using hs_vm_string_args_t        = RedjetStringArgs;
using hs_vm_integer_args_t       = RedjetIntegerArgs;
using hs_vm_float_args_t         = RedjetFloatArgs;
using hs_vm_config_t             = RedjetConfig;
using hs_cli_resolution_result_t = RedjetCLIResolution;

/* ========================================================================= */
/* 2. SEMANTIC INTENT REGISTRY & RESOLUTION DECLARATIONS                     */
/* ========================================================================= */

struct hs_cli_intent_entry_t {
    const char* intent_keyword;
    const char* canonical_flag;
    const char* default_value;
    const char* technical_doc;
};

uint64_t parse_size_with_unit(const char* str);
uint64_t parse_time_with_unit_to_ns(const char* str);
int      hs_damerau_levenshtein_distance(const char* s1, const char* s2);
double   hs_jaro_winkler_similarity(const char* s1, const char* s2);

void hs_cli_strip_prefix_and_split(const char* raw_token, const char* raw_val,
                                   char* clean_key, size_t key_sz,
                                   char* clean_val, size_t val_sz);

hs_cli_resolution_result_t hs_cli_resolve_argument(const char* input_key, const char* input_val);

void            parse_single_hs_flag(hs_vm_config_t& cfg, const char* flag_str);
bool            validate_and_reconcile_hs_flags(hs_vm_config_t& cfg);
hs_vm_config_t  parse_all_hs_flags(const char* custom_args, bool read_env = true);

extern std::atomic<bool>    g_hs_rmrp_enabled;
extern std::atomic<uint8_t> g_hs_rmrp_method;
extern std::atomic<uint8_t> g_hs_global_volatility;
extern std::atomic<uint8_t> g_hs_puzzler_morph_method;
extern std::atomic<bool>    g_hs_entangle_execution_flag;
extern std::atomic<bool>    g_hs_strict_vfs_validation;
extern std::atomic<bool>    g_hs_vfs_panic_dialog_enabled;
extern std::atomic<float>   g_hs_heat_gain_weight;
extern std::atomic<float>   g_hs_heat_decay_factor;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API int      HS_hotspot_cli_resolve_intent(const char* input_token,
                                                     char* out_canonical_flag, size_t flag_buf_sz,
                                                     char* out_canonical_value, size_t val_buf_sz);

RJ_API void     HS_hotspot_set_iasm_threshold(uint32_t threshold);
RJ_API uint32_t HS_hotspot_get_iasm_threshold(void);
RJ_API int      HS_hotspot_is_iasm_active(uint8_t op_priority);

RJ_API void     HS_hotspot_set_strict_ring3(int enable);
RJ_API int      HS_hotspot_is_strict_ring3(void);

RJ_API void     HS_hotspot_set_strict_flags(int enable);
RJ_API int      HS_hotspot_is_strict_flags(void);
RJ_API void     HS_hotspot_set_strict_flag_suppress_abort(int suppress);

#ifdef __cplusplus
}
#endif

using RedjetVMConfig = hs_vm_config_t;

#endif // SHARE_GC_REDJET_REDJETINTENTCLI_HPP
