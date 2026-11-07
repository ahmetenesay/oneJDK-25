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

#include "gc/redjet/runtime/redjetIntentCLI.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/runtime/redjetUpclocker.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

#ifndef _WIN32
#include <strings.h>
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#endif

std::atomic<bool>    g_hs_rmrp_enabled{ true };
std::atomic<uint8_t> g_hs_rmrp_method{ static_cast<uint8_t>(hs_rmrp_method_e::PLUMP) };
std::atomic<uint8_t> g_hs_global_volatility{ static_cast<uint8_t>(hs_global_volatility_e::DRAM) };
std::atomic<uint8_t> g_hs_puzzler_morph_method{ static_cast<uint8_t>(hs_puzzler_morph_e::SHADOW_RCU) };
std::atomic<bool>    g_hs_entangle_execution_flag{ true };
std::atomic<bool>    g_hs_strict_vfs_validation{ true };
std::atomic<bool>    g_hs_vfs_panic_dialog_enabled{ false };
std::atomic<float>   g_hs_heat_gain_weight{ 1.0f };
std::atomic<float>   g_hs_heat_decay_factor{ 0.97f };

namespace {
static std::atomic<uint32_t> g_hs_iasm_threshold{ 5 };
static std::atomic<bool>     g_hs_strict_ring3{ false };
static std::atomic<bool>     g_hs_strict_flags{ false };
static std::atomic<bool>     g_hs_strict_flag_suppress_abort{ false };

static void sync_runtime_globals_from_config(const hs_vm_config_t& cfg) {
  g_hs_enable_huge_pages.store(cfg.strings.enable_huge_pages, std::memory_order_relaxed);
  g_hs_rmrp_enabled.store(cfg.strings.reduce_memory_resident_prop, std::memory_order_relaxed);
  g_hs_rmrp_method.store(static_cast<uint8_t>(cfg.strings.rmrp_method), std::memory_order_relaxed);
  g_hs_global_volatility.store(static_cast<uint8_t>(cfg.strings.global_volatility), std::memory_order_relaxed);
  g_hs_puzzler_morph_method.store(static_cast<uint8_t>(cfg.strings.puzzler_morph), std::memory_order_relaxed);
  g_hs_entangle_execution_flag.store(cfg.strings.entangle_execution, std::memory_order_relaxed);
  g_hs_strict_vfs_validation.store(cfg.strings.strict_vfs_branch_validation, std::memory_order_relaxed);
  g_hs_vfs_panic_dialog_enabled.store(cfg.strings.vfs_panic_dialog, std::memory_order_relaxed);
  g_hs_heat_gain_weight.store(cfg.floats.heat_gain_weight, std::memory_order_relaxed);
  g_hs_heat_decay_factor.store(cfg.floats.heat_decay_factor, std::memory_order_relaxed);

  hs_set_critical_simd_mode(static_cast<hs_critical_simd_e>(cfg.strings.critical_simd_mode));
  hs_upclocker_set_boost(cfg.strings.upclocker_enabled ? 1 : 0);
  HS_hotspot_set_allow_operation_dispatchings(cfg.strings.allow_operation_dispatchings ? 1u : 0u);
  HS_hotspot_set_execution_focus(static_cast<HS_hotspot_exec_focus_e>(cfg.strings.execution_focus));
  HS_hotspot_set_iasm_threshold(cfg.integers.inline_asm_threshold);
  HS_hotspot_set_strict_ring3(cfg.strings.strict_ring3_only ? 1 : 0);
  HS_hotspot_set_strict_flags(cfg.strings.strict_flags ? 1 : 0);
}
} // namespace

/* ========================================================================= */
/* 1. DELEGATED UNIT PARSERS & FUZZY DISTANCE METRICS (Single Source: Flags) */
/* ========================================================================= */

uint64_t parse_size_with_unit(const char* str) {
  return RedjetFlags::parse_size_with_unit(str);
}

uint64_t parse_time_with_unit_to_ns(const char* str) {
  return RedjetFlags::parse_time_with_unit_to_ns(str);
}

int hs_damerau_levenshtein_distance(const char* s1, const char* s2) {
  if (!s1 || !s2) return 99;
  return RedjetFlags::damerau_levenshtein_distance(s1, s2);
}

double hs_jaro_winkler_similarity(const char* s1, const char* s2) {
  return RedjetFlags::jaro_winkler_similarity(s1, s2);
}

void hs_cli_strip_prefix_and_split(const char* raw_token, const char* raw_val,
                                   char* clean_key, size_t key_sz,
                                   char* clean_val, size_t val_sz) {
  RedjetFlags::strip_prefix_and_split(raw_token, raw_val, clean_key, key_sz, clean_val, val_sz);
}

hs_cli_resolution_result_t hs_cli_resolve_argument(const char* input_key, const char* input_val) {
  return RedjetFlags::resolve_argument(input_key, input_val);
}

/* ========================================================================= */
/* 2. FLAG PARSING & CROSS-FLAG CONFLICT RECONCILIATION ENGINE               */
/* ========================================================================= */

void parse_single_hs_flag(hs_vm_config_t& cfg, const char* flag_str) {
  if (!flag_str || !flag_str[0]) return;

  char key[64] = { 0 };
  char val[64] = { 0 };
  hs_cli_strip_prefix_and_split(flag_str, nullptr, key, sizeof(key), val, sizeof(val));
  if (key[0] == '\0') return;

  const char* effective_val = val[0] ? val : "1";
  if (RedjetFlags::parse_single_flag_internal(cfg, key, effective_val)) {
    sync_runtime_globals_from_config(cfg);
    return;
  }

  hs_cli_resolution_result_t res = hs_cli_resolve_argument(key, val);
  if (res.matched) {
    char msg_buf[256];
    if (res.was_semantic_intent) {
      std::snprintf(msg_buf, sizeof(msg_buf),
                    "[RedjetGC CLI Semantic Engine] Recognized intent '%s' -> applied canonical flag '%s=%s' (confidence: %.2f)",
                    key, res.canonical_flag, res.canonical_value, res.confidence);
      hs_unified_log_emit(HS_Ebin0_SAFEPOINT, HS_Ebin1_INFO, 0, "cli_semantic_intent", msg_buf);
    } else if (res.was_fuzzy_typo) {
      std::snprintf(msg_buf, sizeof(msg_buf),
                    "[RedjetGC CLI Helper] Unknown argument '%s' -> auto-matched canonical '%s'",
                    key, res.canonical_flag);
      hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, 0, "cli_fuzzy_typo", msg_buf);
    }
    if (RedjetFlags::parse_single_flag_internal(cfg, res.canonical_flag, res.canonical_value)) {
      sync_runtime_globals_from_config(cfg);
    }
    return;
  }

  char msg_buf[256];
  std::snprintf(msg_buf, sizeof(msg_buf), "[RedjetGC CLI Helper] Unrecognized flag '%s'", key);
  hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, 0, "cli_unknown_flag", msg_buf);

  if (HS_hotspot_is_strict_flags() || cfg.strings.strict_flags) {
    cfg.has_flag_conflict = true;
    std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                  "[RedjetGC STRICT FLAG PANIC] Unrecognized CLI Flag: '%s' (Strict_Flags:true)", key);
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_FATAL, 0, "strict_flag_panic", cfg.conflict_msg);
    if (!g_hs_strict_flag_suppress_abort.load(std::memory_order_relaxed)) {
      (void)HS_hotspot_emergency_os_sweep();
      std::abort();
    }
  }
}

bool validate_and_reconcile_hs_flags(hs_vm_config_t& cfg) {
  if (cfg.strings.gc_operation_facility == hs_gc_operation_facility_e::SIMD) {
    if (cfg.strings.critical_simd_explicit) {
      cfg.has_flag_conflict = true;
      if (cfg.strings.critical_simd_mode == RedjetCriticalSIMD::DENY) {
        std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                      "FATAL FLAG CONFLICT: GC_Operation_Facility:SIMD is incompatible with Use_Critical_SIMDly_Operations:Deny!");
        hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "flag_conflict_simd", cfg.conflict_msg);
        return false;
      } else {
        std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                      "NOTICE: 'Use_Critical_SIMDly_Operations' is superseded because GC_Operation_Facility:SIMD is fully SIMD-native.");
        hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, 0, "flag_warn_simd", cfg.conflict_msg);
      }
    }
  }

  if (cfg.strings.rmrp_method_explicit && !cfg.strings.reduce_memory_resident_prop) {
    cfg.has_flag_conflict = true;
    std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                  "FATAL FLAG CONFLICT: 'RMRP_Method' cannot be used without 'Reduce_Memory_Resident_Propagation:true'!");
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "flag_conflict_rmrp", cfg.conflict_msg);
    return false;
  }

  if (cfg.strings.no_sfgc_overwrite_mode) {
    if (cfg.strings.always_focus_compactizing || cfg.strings.gc_operation_facility == hs_gc_operation_facility_e::SIMD) {
      cfg.has_flag_conflict = true;
      std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                    "FATAL FLAG CONFLICT: PrimitiveRW disables collector sweeps; incompatible with Always_Focus_Compactizing or GC_Mode:SIMD.");
      hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "flag_conflict_nogc", cfg.conflict_msg);
      return false;
    }
  }

  if (cfg.strings.allow_operation_dispatchings) {
    std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                  "RedjetGC Operational Notice: Allow_Operation_Dispatchings:true restricts High Priority (8-10) ops to protect locality.");
    hs_unified_log_emit(HS_Ebin0_ALU_DISPATCH_DIRECT, HS_Ebin1_INFO, 0, "alu_dispatch_notice", cfg.conflict_msg);
  }

  if (cfg.strings.is_safety_critical || cfg.strings.profile == RedjetProfile::SSIOLSC) {
    cfg.strings.profile          = RedjetProfile::SSIOLSC;
    cfg.strings.is_safety_critical = true;
    cfg.forbid_dynamic_growth    = true;
    cfg.strings.advance_payment  = true;
    cfg.strings.gc_mode          = hs_gc_mode_e::STATIC;
    std::strncpy(cfg.strings.gc_mode_raw, "Static", sizeof(cfg.strings.gc_mode_raw) - 1);
    if (!cfg.strings.no_sfgc_overwrite_mode && cfg.strings.scgc_type != hs_scgc_type_e::PRIMITIVE_RW) {
      cfg.strings.gc_heuristic = hs_gc_heuristic_e::BUREAUCRATIC;
      std::strncpy(cfg.strings.gc_heuristic_raw, "Bureaucratic", sizeof(cfg.strings.gc_heuristic_raw) - 1);
    }
    if (cfg.strings.allow_operation_dispatchings) {
      cfg.strings.allow_operation_dispatchings = false;
      HS_hotspot_set_allow_operation_dispatchings(0u);
    }
  }

  if (cfg.integers.wakes_gc_interval_explicit) {
    bool is_static_gc = (cfg.strings.gc_mode == hs_gc_mode_e::STATIC) ||
                        (cfg.strings.gc_operation_facility == hs_gc_operation_facility_e::STATIC_FIXED) ||
                        cfg.strings.is_safety_critical ||
                        (cfg.strings.profile == RedjetProfile::SSIOLSC);
    if (!is_static_gc) {
      cfg.has_flag_conflict = true;
      std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                    "FATAL FLAG CONFLICT: 'Wakes_GC_Cycling_Interval' is valid only in Static GC or SSIOLSC mode!");
      hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "flag_conflict_wakes_interval", cfg.conflict_msg);
      return false;
    }
  }

  if (cfg.strings.strict_ring3_only) {
    if (cfg.integers.inline_asm_threshold_explicit && cfg.integers.inline_asm_threshold < 10) {
      cfg.has_flag_conflict = true;
      std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                    "[RedjetGC Security Warning] Strict_Ring3_Only:true active while InlineASM_Threshold (%u < 10) requested! Clamping to 10.",
                    cfg.integers.inline_asm_threshold);
      hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, 0, "flag_conflict_strict_ring3", cfg.conflict_msg);
    }
    cfg.integers.inline_asm_threshold = 10;
    HS_hotspot_set_iasm_threshold(10);
  }

  sync_runtime_globals_from_config(cfg);
  return true;
}

hs_vm_config_t parse_all_hs_flags(const char* custom_args, bool read_env) {
  hs_vm_config_t cfg;
  HS_hotspot_set_strict_flags(0);

  if (read_env) {
    const char* env_sc = std::getenv("RJ_PROFILE");
    if (!env_sc) env_sc = std::getenv("REDJET_PROFILE");
    if (env_sc && (_stricmp(env_sc, "SC") == 0 || _stricmp(env_sc, "SSIOLSC") == 0 ||
                   _stricmp(env_sc, "SAFETY_CRITICAL") == 0 || _stricmp(env_sc, "1") == 0)) {
      cfg.strings.profile            = RedjetProfile::SSIOLSC;
      cfg.strings.is_safety_critical = true;
      cfg.forbid_dynamic_growth      = true;
      cfg.strings.advance_payment    = true;
    }
  }

  auto parse_flags_loop = [&](const char* src) {
    if (!src) return;
    const char* p = src;
    while (*p) {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',' || *p == ';') p++;
      if (!*p) break;
      const char* start = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != ',' && *p != ';') {
        p++;
      }
      size_t len = static_cast<size_t>(p - start);
      if (len > 0) {
        char token[128] = { 0 };
        if (len >= sizeof(token)) len = sizeof(token) - 1;
        std::strncpy(token, start, len);
        token[len] = '\0';
        parse_single_hs_flag(cfg, token);
      }
    }
  };

  if (read_env) {
    const char* env_args = std::getenv("REDJET_GC_OPTS");
    if (!env_args) env_args = std::getenv("HS_VM_ARGS");
    if (env_args) {
      parse_flags_loop(env_args);
    }
  }

  if (custom_args) {
    parse_flags_loop(custom_args);
  }

  (void)validate_and_reconcile_hs_flags(cfg);
  return cfg;
}

/* ========================================================================= */
/* 3. C ABI EXPORTS                                                          */
/* ========================================================================= */

extern "C" {

RJ_API int HS_hotspot_cli_resolve_intent(const char* input_token,
                                         char* out_canonical_flag, size_t flag_buf_sz,
                                         char* out_canonical_value, size_t val_buf_sz) {
  if (!input_token || !out_canonical_flag || flag_buf_sz == 0) return 0;
  hs_cli_resolution_result_t res = hs_cli_resolve_argument(input_token, nullptr);
  if (!res.matched) return 0;

  std::strncpy(out_canonical_flag, res.canonical_flag, flag_buf_sz - 1);
  out_canonical_flag[flag_buf_sz - 1] = '\0';
  if (out_canonical_value && val_buf_sz > 0) {
    std::strncpy(out_canonical_value, res.canonical_value, val_buf_sz - 1);
    out_canonical_value[val_buf_sz - 1] = '\0';
  }
  return 1;
}

RJ_API void HS_hotspot_set_iasm_threshold(uint32_t threshold) {
  g_hs_iasm_threshold.store(threshold > 10 ? 10 : threshold, std::memory_order_release);
}

RJ_API uint32_t HS_hotspot_get_iasm_threshold(void) {
  return g_hs_iasm_threshold.load(std::memory_order_acquire);
}

RJ_API int HS_hotspot_is_iasm_active(uint8_t op_priority) {
  if (g_hs_strict_ring3.load(std::memory_order_acquire)) return 0;
  uint32_t thresh = g_hs_iasm_threshold.load(std::memory_order_acquire);
  if (thresh >= 10) return 0;
  uint8_t eff_prio = (op_priority == 0) ? 5 : op_priority;
  return (eff_prio >= thresh) ? 1 : 0;
}

RJ_API void HS_hotspot_set_strict_ring3(int enable) {
  g_hs_strict_ring3.store(enable != 0, std::memory_order_release);
  if (enable != 0) {
    g_hs_iasm_threshold.store(10, std::memory_order_release);
  }
}

RJ_API int HS_hotspot_is_strict_ring3(void) {
  return g_hs_strict_ring3.load(std::memory_order_acquire) ? 1 : 0;
}

RJ_API void HS_hotspot_set_strict_flags(int enable) {
  g_hs_strict_flags.store(enable != 0, std::memory_order_release);
}

RJ_API int HS_hotspot_is_strict_flags(void) {
  return g_hs_strict_flags.load(std::memory_order_acquire) ? 1 : 0;
}

RJ_API void HS_hotspot_set_strict_flag_suppress_abort(int suppress) {
  g_hs_strict_flag_suppress_abort.store(suppress != 0, std::memory_order_release);
}

} // extern "C"
