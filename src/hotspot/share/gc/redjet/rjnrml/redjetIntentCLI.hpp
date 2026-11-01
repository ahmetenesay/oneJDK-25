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
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/redjetSIMD.hpp"
#include "gc/redjet/rjspcf/redjetALU.hpp"
#include "gc/redjet/rjnrml/redjetPagefile.hpp"
#include "gc/redjet/rjspcf/redjetHardware.hpp"

/* ========================================================================= */
/* 1. CONFIGURATION ENUMS & VM ARGUMENT STRUCTURES                           */
/* ========================================================================= */

enum class hs_gc_mode_e : uint8_t {
    STATIC_MODE    = 0,
    HEURISTIC_MODE = 1
};

enum class hs_gc_operation_facility_e : uint8_t {
    CYCLES       = 0,
    SIMD         = 1,
    STATIC_FIXED = 2
};

enum class hs_snatch_rebate_mode_e : uint8_t {
    AUTO     = 0,
    RAPID    = 1,
    LAZY     = 2,
    GYC      = 3,
    DECOMMIT = 4
};

enum class hs_gc_heuristic_e : uint8_t {
    ADAPTIVE     = 0,
    AGGRESSIVE   = 1,
    BUREAUCRATIC = 2,
    DISABLED     = 3
};

enum class hs_scgc_type_e : uint8_t {
    DEFAULT      = 0,
    PRIMITIVE_RW = 1
};

#ifndef HS_RMRP_METHOD_E_DEFINED
#define HS_RMRP_METHOD_E_DEFINED
enum class hs_rmrp_method_e : uint8_t {
    PLUMP      = 0, // Prune and coalesce before rebate
    SEQUENTIAL = 1  // Installment: rebate immediately per block
};
#endif

enum class hs_global_volatility_e : uint8_t {
    DRAM = 0,
    SRAM = 1
};

#ifndef HS_PUZZLER_MORPH_E_DEFINED
#define HS_PUZZLER_MORPH_E_DEFINED
enum class hs_puzzler_morph_e : uint8_t {
    EXCISION_EXCLUSIVE = 0,
    SHADOW_RCU         = 1
};
#endif

struct hs_vm_string_args_t {
    char                        gc_mode_raw[32]               = "Heuristic";
    char                        gc_operation_facility_raw[32] = "CYCLES";
    char                        critical_simd_raw[32]         = "Auto";
    char                        snatch_rebate_raw[32]         = "Auto";
    char                        freelist_policy_raw[32]       = "LIFO";
    char                        gc_heuristic_raw[32]          = "Adaptive";
    char                        rmrp_method_raw[32]           = "Plump";
    char                        global_volatility_raw[32]     = "DRAM";
    char                        puzzler_morph_raw[32]         = "Shadow";

    hs_gc_mode_e                gc_mode               = hs_gc_mode_e::HEURISTIC_MODE;
    hs_gc_operation_facility_e  gc_operation_facility = hs_gc_operation_facility_e::CYCLES;
    hs_critical_simd_e          critical_simd_mode    = hs_critical_simd_e::AUTO;
    hs_snatch_rebate_mode_e     snatch_rebate_mode    = hs_snatch_rebate_mode_e::AUTO;
    hs_gc_heuristic_e           gc_heuristic          = hs_gc_heuristic_e::ADAPTIVE;
    hs_scgc_type_e              scgc_type             = hs_scgc_type_e::DEFAULT;
    hs_rmrp_method_e            rmrp_method           = hs_rmrp_method_e::PLUMP;
    hs_global_volatility_e      global_volatility     = hs_global_volatility_e::DRAM;
    hs_puzzler_morph_e          puzzler_morph         = hs_puzzler_morph_e::SHADOW_RCU;

    bool                        critical_simd_explicit             = false;
    bool                        enable_huge_pages                  = false;
    bool                        Advance_Payment                    = false;
    bool                        always_focus_compactizing          = false;
    bool                        no_sfgc_overwrite_mode             = false;
    bool                        allow_operation_dispatchings       = false;
    bool                        reduce_memory_resident_propagation = false;
    bool                        rmrp_method_explicit               = false;
    bool                        experimental_options               = false;
    bool                        entangle_execution                 = false;
    bool                        strict_vfs_branch_validation       = true;
    bool                        vfs_panic_dialog                   = true;
    bool                        upclocker_enabled                  = false;
    bool                        strict_ring3_only                  = false;
    bool                        strict_flags                       = false;
};

struct hs_vm_integer_args_t {
    size_t   runway_handsel_bytes          = 1024ULL * 1024 * 1024;
    size_t   landfill_handsel_bytes        = 285ULL * 1024 * 1024;
    size_t   slab_chunk_size_bytes         = 2ULL * 1024 * 1024;
    size_t   pressure_threshold_bytes      = 256ULL * 1024 * 1024;
    size_t   code_arena_bytes              = 64ULL * 1024 * 1024;
    uint64_t wakes_gc_interval_ns          = 1000000ULL;
    bool     wakes_gc_interval_explicit    = false;
    uint64_t line_compaction_freq_ns       = 350ULL * 1000000ULL;
    uint32_t in_heap_forwarding_rate       = 7;
    uint32_t freelist_recycle_policy       = 0;
    uint32_t inline_asm_threshold          = 5;
    bool     inline_asm_threshold_explicit = false;
};

struct hs_vm_float_args_t {
    float pressure_mild_ratio     = 0.60f;
    float pressure_high_ratio     = 0.80f;
    float pressure_critical_ratio = 0.92f;
    float landfill_critical_ratio = 0.85f;
    float falline_compact_ratio   = 0.30f;
    float heat_decay_factor       = 0.97f;
    float heat_gain_weight        = 1.00f;
};

struct hs_vm_config_t {
    hs_vm_string_args_t              strings;
    hs_vm_integer_args_t             integers;
    hs_vm_float_args_t               floats;
    msimd_profile_mode_t             profile                    = MSIMD_PROFILE_STANDARD;
    bool                             is_safety_critical         = false;
    bool                             Forbid_Dynamic_Growth      = false;
    bool                             UnlockExperimentalVMOptions = false;
    bool                             UseMLTCSolver              = false;
    bool                             disable_jit_deopt          = false;
    uint32_t                         max_loop_iterations        = 0;
    HS_custominiVm_execution_focus_t execution_focus            = HS_FOCUS_AUTO;
    bool                             has_flag_conflict          = false;
    char                             conflict_msg[512]          = { 0 };
};

/* ========================================================================= */
/* 2. SEMANTIC INTENT REGISTRY & FUZZY RESOLUTION STRUCTURES                 */
/* ========================================================================= */

struct hs_cli_intent_entry_t {
    const char* intent_keyword;
    const char* canonical_flag;
    const char* default_value;
    const char* technical_doc;
};

struct hs_cli_resolution_result_t {
    bool        matched{ false };
    bool        was_semantic_intent{ false };
    bool        was_fuzzy_typo{ false };
    char        canonical_flag[64]{ 0 };
    char        canonical_value[64]{ 0 };
    double      confidence{ 0.0 };
    const char* technical_doc{ nullptr };
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

MSIMD_API int      HS_custominiVm_cli_resolve_intent(const char* input_token,
                                                     char* out_canonical_flag, size_t flag_buf_sz,
                                                     char* out_canonical_value, size_t val_buf_sz);

MSIMD_API void     HS_custominiVm_set_iasm_threshold(uint32_t threshold);
MSIMD_API uint32_t HS_custominiVm_get_iasm_threshold(void);
MSIMD_API int      HS_custominiVm_is_iasm_active(uint8_t op_priority);

MSIMD_API void     HS_custominiVm_set_strict_ring3(int enable);
MSIMD_API int      HS_custominiVm_is_strict_ring3(void);

MSIMD_API void     HS_custominiVm_set_strict_flags(int enable);
MSIMD_API int      HS_custominiVm_is_strict_flags(void);
MSIMD_API void     HS_custominiVm_set_strict_flag_suppress_abort(int suppress);

#ifdef __cplusplus
}
#endif

using RedjetVMConfig = hs_vm_config_t;

#endif // SHARE_GC_REDJET_REDJETINTENTCLI_HPP
