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

#ifndef SHARE_GC_REDLET_REDLETINTENTCLI_HPP
#define SHARE_GC_REDLET_REDLETINTENTCLI_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletSIMD.hpp"
#include "gc/redlet/redletALU.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletHardware.hpp"

/* ========================================================================= */
/* 1. CONFIGURATION ENUMS & VM ARGUMENT STRUCTURES                           */
/* ========================================================================= */

enum class sf_gc_mode_e : uint8_t {
    STATIC_MODE    = 0,
    HEURISTIC_MODE = 1
};

enum class sf_gc_operation_facility_e : uint8_t {
    CYCLES       = 0,
    SIMD         = 1,
    STATIC_FIXED = 2
};

enum class sf_snatch_rebate_mode_e : uint8_t {
    AUTO     = 0,
    RAPID    = 1,
    LAZY     = 2,
    GYC      = 3,
    DECOMMIT = 4
};

enum class sf_gc_heuristic_e : uint8_t {
    ADAPTIVE     = 0,
    AGGRESSIVE   = 1,
    BUREAUCRATIC = 2,
    DISABLED     = 3
};

enum class sf_scgc_type_e : uint8_t {
    DEFAULT      = 0,
    PRIMITIVE_RW = 1
};

#ifndef SF_RMRP_METHOD_E_DEFINED
#define SF_RMRP_METHOD_E_DEFINED
enum class sf_rmrp_method_e : uint8_t {
    PLUMP      = 0, // Prune and coalesce before rebate
    SEQUENTIAL = 1  // Installment: rebate immediately per block
};
#endif

enum class sf_global_volatility_e : uint8_t {
    DRAM = 0,
    SRAM = 1
};

#ifndef SF_PUZZLER_MORPH_E_DEFINED
#define SF_PUZZLER_MORPH_E_DEFINED
enum class sf_puzzler_morph_e : uint8_t {
    EXCISION_EXCLUSIVE = 0,
    SHADOW_RCU         = 1
};
#endif

struct sf_vm_string_args_t {
    char                        gc_mode_raw[32]               = "Heuristic";
    char                        gc_operation_facility_raw[32] = "CYCLES";
    char                        critical_simd_raw[32]         = "Auto";
    char                        snatch_rebate_raw[32]         = "Auto";
    char                        freelist_policy_raw[32]       = "LIFO";
    char                        gc_heuristic_raw[32]          = "Adaptive";
    char                        rmrp_method_raw[32]           = "Plump";
    char                        global_volatility_raw[32]     = "DRAM";
    char                        puzzler_morph_raw[32]         = "Shadow";

    sf_gc_mode_e                gc_mode               = sf_gc_mode_e::HEURISTIC_MODE;
    sf_gc_operation_facility_e  gc_operation_facility = sf_gc_operation_facility_e::CYCLES;
    sf_critical_simd_e          critical_simd_mode    = sf_critical_simd_e::AUTO;
    sf_snatch_rebate_mode_e     snatch_rebate_mode    = sf_snatch_rebate_mode_e::AUTO;
    sf_gc_heuristic_e           gc_heuristic          = sf_gc_heuristic_e::ADAPTIVE;
    sf_scgc_type_e              scgc_type             = sf_scgc_type_e::DEFAULT;
    sf_rmrp_method_e            rmrp_method           = sf_rmrp_method_e::PLUMP;
    sf_global_volatility_e      global_volatility     = sf_global_volatility_e::DRAM;
    sf_puzzler_morph_e          puzzler_morph         = sf_puzzler_morph_e::SHADOW_RCU;

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

struct sf_vm_integer_args_t {
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

struct sf_vm_float_args_t {
    float pressure_mild_ratio     = 0.60f;
    float pressure_high_ratio     = 0.80f;
    float pressure_critical_ratio = 0.92f;
    float landfill_critical_ratio = 0.85f;
    float falline_compact_ratio   = 0.30f;
    float heat_decay_factor       = 0.97f;
    float heat_gain_weight        = 1.00f;
};

struct sf_vm_config_t {
    sf_vm_string_args_t              strings;
    sf_vm_integer_args_t             integers;
    sf_vm_float_args_t               floats;
    msimd_profile_mode_t             profile                    = MSIMD_PROFILE_STANDARD;
    bool                             is_safety_critical         = false;
    bool                             Forbid_Dynamic_Growth      = false;
    bool                             UnlockExperimentalVMOptions = false;
    bool                             UseMLTCSolver              = false;
    bool                             disable_jit_deopt          = false;
    uint32_t                         max_loop_iterations        = 0;
    SF_custominiVm_execution_focus_t execution_focus            = SF_FOCUS_AUTO;
    bool                             has_flag_conflict          = false;
    char                             conflict_msg[512]          = { 0 };
};

/* ========================================================================= */
/* 2. SEMANTIC INTENT REGISTRY & FUZZY RESOLUTION STRUCTURES                 */
/* ========================================================================= */

struct sf_cli_intent_entry_t {
    const char* intent_keyword;
    const char* canonical_flag;
    const char* default_value;
    const char* technical_doc;
};

struct sf_cli_resolution_result_t {
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
int      sf_damerau_levenshtein_distance(const char* s1, const char* s2);
double   sf_jaro_winkler_similarity(const char* s1, const char* s2);

void sf_cli_strip_prefix_and_split(const char* raw_token, const char* raw_val,
                                   char* clean_key, size_t key_sz,
                                   char* clean_val, size_t val_sz);

sf_cli_resolution_result_t sf_cli_resolve_argument(const char* input_key, const char* input_val);

void            parse_single_sf_flag(sf_vm_config_t& cfg, const char* flag_str);
bool            validate_and_reconcile_sf_flags(sf_vm_config_t& cfg);
sf_vm_config_t  parse_all_sf_flags(const char* custom_args, bool read_env = true);

extern std::atomic<bool>    g_sf_rmrp_enabled;
extern std::atomic<uint8_t> g_sf_rmrp_method;
extern std::atomic<uint8_t> g_sf_global_volatility;
extern std::atomic<uint8_t> g_sf_puzzler_morph_method;
extern std::atomic<bool>    g_sf_entangle_execution_flag;
extern std::atomic<bool>    g_sf_strict_vfs_validation;
extern std::atomic<bool>    g_sf_vfs_panic_dialog_enabled;
extern std::atomic<float>   g_sf_heat_gain_weight;
extern std::atomic<float>   g_sf_heat_decay_factor;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API int      SF_custominiVm_cli_resolve_intent(const char* input_token,
                                                     char* out_canonical_flag, size_t flag_buf_sz,
                                                     char* out_canonical_value, size_t val_buf_sz);

MSIMD_API void     SF_custominiVm_set_iasm_threshold(uint32_t threshold);
MSIMD_API uint32_t SF_custominiVm_get_iasm_threshold(void);
MSIMD_API int      SF_custominiVm_is_iasm_active(uint8_t op_priority);

MSIMD_API void     SF_custominiVm_set_strict_ring3(int enable);
MSIMD_API int      SF_custominiVm_is_strict_ring3(void);

MSIMD_API void     SF_custominiVm_set_strict_flags(int enable);
MSIMD_API int      SF_custominiVm_is_strict_flags(void);
MSIMD_API void     SF_custominiVm_set_strict_flag_suppress_abort(int suppress);

#ifdef __cplusplus
}
#endif

using RedletVMConfig = sf_vm_config_t;

#endif // SHARE_GC_REDLET_REDLETINTENTCLI_HPP
