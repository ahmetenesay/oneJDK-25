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

#ifndef SHARE_GC_REDLET_REDLETFLAGS_HPP
#define SHARE_GC_REDLET_REDLETFLAGS_HPP

#ifdef REDLET_STANDALONE_BUILD
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <algorithm>

#ifndef MIN2
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX2
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#endif

#if defined(_MSC_VER)
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#endif

#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif

struct DummyLog {
  template<typename... Args>
  void operator()(const char*, Args...) const {}
};
#ifndef log_info
#define log_info(...) DummyLog()
#endif
#ifndef log_warning
#define log_warning(...) DummyLog()
#endif
#ifndef log_error
#define log_error(...) DummyLog()
#endif

class AllStatic {};
#else
#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include <cstdint>
#include <cstring>
#endif

enum class RedletGCMode : uint8_t {
  HEURISTIC = 0,
  STATIC    = 1,
  SIMD      = 2,
  CYCLES    = 3
};

enum class RedletGCOperationFacility : uint8_t {
  CYCLES       = 0,
  SIMD         = 1,
  STATIC_FIXED = 2
};

enum class RedletCriticalSIMD : uint8_t {
  AUTO  = 0,
  ALLOW = 1,
  DENY  = 2
};

enum class RedletSnatchRebateMode : uint8_t {
  AUTO     = 0,
  RAPID    = 1,
  LAZY     = 2,
  GYC      = 3,
  DECOMMIT = 4
};

enum class RedletGCHeuristic : uint8_t {
  ADAPTIVE     = 0,
  AGGRESSIVE   = 1,
  BUREAUCRATIC = 2,
  DISABLED     = 3
};

enum class RedletRMRPMethod : uint8_t {
  PLUMP      = 0,
  SEQUENTIAL = 1
};

enum class RedletExecutionFocus : uint8_t {
  AUTO        = 0,
  SCP         = 1,
  MCP         = 2,
  HYBRID_DUAL = 3,
  SCP_HOMO    = 4,
  SCP_HYBRID  = 5,
  MCP_HOMO    = 6,
  MCP_HYBRID  = 7,
  DUAL_HOMO   = 8,
  DUAL_HYBRID = 9
};

enum class RedletProfile : uint8_t {
  NORMAX  = 0,
  SSIOLSC = 1
};

enum class RedletSCGCType : uint8_t {
  DEFAULT      = 0,
  PRIMITIVE_RW = 1
};

struct RedletStringArgs {
  char                      gc_mode_raw[32]               = "Heuristic";
  char                      gc_operation_facility_raw[32] = "CYCLES";
  char                      critical_simd_raw[32]         = "Auto";
  char                      snatch_rebate_raw[32]         = "Auto";
  char                      freelist_policy_raw[32]       = "LIFO";
  char                      gc_heuristic_raw[32]          = "Adaptive";
  char                      rmrp_method_raw[32]           = "Plump";
  char                      execution_focus_raw[32]       = "Auto";
  char                      profile_raw[32]               = "NORMAX";
  char                      scgc_type_raw[32]             = "Default";

  RedletGCMode              gc_mode               = RedletGCMode::HEURISTIC;
  RedletGCOperationFacility gc_operation_facility = RedletGCOperationFacility::CYCLES;
  RedletCriticalSIMD        critical_simd_mode    = RedletCriticalSIMD::AUTO;
  RedletSnatchRebateMode    snatch_rebate_mode    = RedletSnatchRebateMode::AUTO;
  RedletGCHeuristic         gc_heuristic          = RedletGCHeuristic::ADAPTIVE;
  enum RedletRMRPMethod     rmrp_method           = RedletRMRPMethod::PLUMP;
  RedletExecutionFocus      execution_focus       = RedletExecutionFocus::AUTO;
  enum RedletProfile        profile               = RedletProfile::NORMAX;
  enum RedletSCGCType       scgc_type             = RedletSCGCType::DEFAULT;

  bool is_safety_critical                = false;

  bool critical_simd_explicit            = false;
  bool enable_huge_pages                 = false;
  bool advance_payment                   = false; // AlwaysPreTouch
  bool always_focus_compactizing         = false;
  bool no_sfgc_overwrite_mode            = false;
  bool allow_operation_dispatchings      = false;
  bool reduce_memory_resident_prop       = true;  // RMRP
  bool rmrp_method_explicit              = false;
  bool experimental_options              = false;
  bool entangle_execution                = false;
  bool strict_vfs_branch_validation      = true;
  bool vfs_panic_dialog                  = false;
  bool upclocker_enabled                 = false; // Turbo/Boost
  bool strict_ring3_only                 = false;
  bool strict_flags                      = false;
};

struct RedletIntegerArgs {
  size_t   runway_handsel_bytes        = 1024ULL * 1024 * 1024;
  size_t   landfill_handsel_bytes      = 285ULL * 1024 * 1024;
  size_t   slab_chunk_size_bytes       = 2ULL * 1024 * 1024;
  size_t   pressure_threshold_bytes    = 256ULL * 1024 * 1024;
  size_t   code_arena_bytes            = 64ULL * 1024 * 1024;
  uint64_t wakes_gc_interval_ns        = 1000000ULL;
  bool     wakes_gc_interval_explicit  = false;
  uint64_t line_compaction_freq_ns     = 350ULL * 1000000ULL;
  uint32_t in_heap_forwarding_rate     = 7;
  uint32_t freelist_recycle_policy     = 0;
  uint32_t inline_asm_threshold        = 5;
  bool     inline_asm_threshold_explicit = false;
};

struct RedletFloatArgs {
  float pressure_mild_ratio     = 0.60f;
  float pressure_high_ratio     = 0.80f;
  float pressure_critical_ratio = 0.92f;
  float landfill_critical_ratio = 0.85f;
  float falline_compact_ratio   = 0.30f;
  float heat_decay_factor       = 0.97f;
  float heat_gain_weight        = 1.00f;
};

struct RedletConfig {
  RedletStringArgs  strings;
  RedletIntegerArgs integers;
  RedletFloatArgs   floats;
  bool              forbid_dynamic_growth = false;
  bool              unlock_experimental   = false;
  bool              use_mltc_solver       = false;
};

struct RedletCLIResolution {
  bool        matched              = false;
  bool        was_semantic_intent  = false;
  bool        was_fuzzy_typo       = false;
  char        canonical_flag[64]   = { 0 };
  char        canonical_value[64]  = { 0 };
  double      confidence           = 0.0;
  const char* technical_doc        = nullptr;
};

class RedletFlags : public AllStatic {
private:
  static RedletConfig _config;

public:
  static void init_defaults();

  static const RedletConfig& config() { return _config; }
  static RedletConfig& config_mut()   { return _config; }

  // Fuzzy string matching algorithms ported from SacFor_VM
  static int    damerau_levenshtein_distance(const char* s1, const char* s2);
  static double jaro_winkler_similarity(const char* s1, const char* s2);

  // Parsing & resolution pipeline
  static void   strip_prefix_and_split(const char* raw_token, const char* raw_val,
                                       char* clean_key, size_t key_sz,
                                       char* clean_val, size_t val_sz);

  static RedletCLIResolution resolve_argument(const char* input_key, const char* input_val = nullptr);

  static bool   parse_single_flag_internal(RedletConfig& cfg, const char* key, const char* val);
  static bool   parse_single_flag(const char* flag_str);
  static void   parse_flag_sequence(const char* sequence_str);

  static uint64_t parse_size_with_unit(const char* str);
  static uint64_t parse_time_with_unit_to_ns(const char* str);

  static void   print_on(outputStream* st);
};

#endif // SHARE_GC_REDLET_REDLETFLAGS_HPP
