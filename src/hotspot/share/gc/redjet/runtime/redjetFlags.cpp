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

#include "gc/redjet/runtime/redjetFlags.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <cstdlib>

RedjetConfig RedjetFlags::_config;

void RedjetFlags::init_defaults() {
  _config = RedjetConfig();
}

static const char* const REDJET_KNOWN_FLAGS[] = {
  "GC_Mode", "GC_Mod", "GCMode", "GCMod", "GC_MODE", "GC_MOD",
  "GC_Operation_Facility", "GC_OPERATION_FACILITY",
  "Use_Critical_SIMDly_Operations", "USE_CRITICAL_SIMDLY_OPERATIONS",
  "Snatch_and_Rebate_Mode", "SNATCH_AND_REBATE_MODE", "SnatchAndRebateMode", "Snatch_And_Rebate_Mode",
  "GC_Heuristic", "GC_HEURISTIC", "FreeList_Recycle_Policy", "FREELIST_RECYCLE_POLICY",
  "FreeListRecyclePolicy", "Freelist_Policy", "FreelistRecyclePolicy",
  "Enable_Huge_Pages", "ENABLE_HUGE_PAGES", "Advance_Payment", "ADVANCE_PAYMENT",
  "AlwaysPreTouch", "Always_PreTouch", "Pretouch", "PreTouch",
  "Upclocker", "UPCLOCKER", "Boost", "Turbo", "Upclock",
  "Always_Focus_Compactizing", "ALWAYS_FOCUS_COMPACTIZING",
  "No_hsGC", "NO_SFGC", "SCGC_Type", "SCGC_TYPE", "Profile", "PROFILE",
  "Forbid_Dynamic_Growth", "FORBID_DYNAMIC_GROWTH", "Forbid_Runtime_Heap", "ForbidRuntimeHeap",
  "Max_Bounded_Iterations", "MAX_BOUNDED_ITERATIONS", "Max_Iterations", "MaxIterations",
  "Allow_Operation_Dispatchings", "AllowOperationDispatchings", "ALLOW_OPERATION_DISPATCHINGS", "Operation_Dispatchings",
  "Reduce_Memory_Resident_Propagation", "ReduceMemoryResidentPropagation", "REDUCE_MEMORY_RESIDENT_PROPAGATION", "RMRP",
  "RMRP_Method", "RMRPMethod", "RMRP_METHOD",
  "Global_Volatility_Method", "GlobalVolatilityMethod", "GLOBAL_VOLATILITY_METHOD", "GlobalVolatility", "RedjetGlobalVolatility",
  "puzzler_Morph_Method", "puzzlerMorphMethod", "puzzler_MORPH_METHOD", "Morph_Method", "PuzzlerMorph", "RedjetPuzzlerMorph",
  "Experimental", "EXPERIMENTAL",
  "Batch_Register_and_Entangle_Execution", "Batch_Register_And_Entangle_Execution", "Entangle_Execution", "EntangleExecution", "RedjetEntangleExecution",
  "Runway_Handsel_Attempt_Value", "Landfill_Handsel_Attempt_Value",
  "Wakes_GC_Cycling_Interval", "Line_Compaction_Freq", "LineCompactionFreq", "LINE_COMPACTION_FREQ",
  "In_Heap_Forwarding_Rate", "InHeapForwardingRate", "IN_HEAP_FORWARDING_RATE",
  "Pressure_Threshold", "PressureThreshold", "PRESSURE_THRESHOLD",
  "Slab_Chunk_Size", "SlabChunkSize", "SLAB_CHUNK_SIZE",
  "Pressure_Mild_Ratio", "PressureMildRatio", "PRESSURE_MILD_RATIO",
  "Pressure_High_Ratio", "PressureHighRatio", "PRESSURE_HIGH_RATIO",
  "Pressure_Critical_Ratio", "PressureCriticalRatio", "PRESSURE_CRITICAL_RATIO",
  "Landfill_Critical_Ratio", "LandfillCriticalRatio", "LANDFILL_CRITICAL_RATIO", "RedjetLandfillCriticalRatio",
  "Falline_Compact_Ratio", "FallineCompactRatio", "FALLINE_COMPACT_RATIO",
  "Execution_Focus", "ExecutionFocus", "Focus", "EXECUTION_FOCUS",
  "Strict_VFS_Branch_Validation", "StrictVFSBranchValidation", "STRICT_VFS_BRANCH_VALIDATION",
  "Strict_VFS", "StrictVFS", "STRICT_VFS", "VFS_Strict", "VFSStrict", "VFS_STRICT",
  "Strict_VFS_Validation", "STRICT_VFS_VALIDATION",
  "VFS_Panic_Dialog", "VFSPanicDialog", "VFS_PANIC_DIALOG", "Panic_Dialog", "PANIC_DIALOG",
  "InlineASM_Threshold", "IASM_Threshold", "InlineAsmThreshold", "INLINEASM_THRESHOLD", "IASM_THRESHOLD", "RedjetInlineAsmThreshold",
  "Strict_Ring3_Only", "StrictRing3Only", "Strict_Ring3", "StrictRing3", "STRICT_RING3_ONLY",
  "Strict_Flags", "StrictFlags", "Strict_Flag_Validation", "StrictFlagValidation", "STRICT_FLAGS",
  "UseMLTCSolver", "Use_MLTC_Solver", "MLTCSolver", "MLTC_Solver", "USE_MLTC_SOLVER",
  "Code_Arena_Bytes", "CodeArenaBytes", "Code_Arena_Size", "Code_Heap_Size", "CODE_ARENA_BYTES",
  "Heat_Decay_Factor", "HeatDecayFactor", "HEAT_DECAY_FACTOR", "RedjetHeatDecayFactor",
  "Heat_Gain_Weight", "HeatGainWeight", "HEAT_GAIN_WEIGHT", "RedjetHeatGainWeight",
  "NORMAX", "SSIOLSC", "RedjetProfile", "RedjetSCMode",
  "SCGCType",
  "Enable_2Points", "Enable2Points", "RedjetEnable2Points",
  "Enable_4Points", "Enable4Points", "RedjetEnable4Points",
  "Parkpoint_Spin_Count", "ParkpointSpinCount", "RedjetParkpointSpinCount",
  "Pausepoint_Spin_Count", "PausepointSpinCount", "RedjetPausepointSpinCount",
  "Dockpoint_Pacing", "DockpointPacing", "RedjetDockpointPacing",
  "Controlpoint_Pacing", "ControlpointPacing", "RedjetControlpointPacing",
  "RedjetGCScheduler", "GCScheduler", "Scheduler",
  "RedjetGCAutoScalarOnSIMDContention", "RedjetDynamicSIMDFallback", "AutoScalarOnSIMDContention",
  "RedjetGCCriticalSIMDOnly", "RedjetCriticalSIMDOnly",
  "RedjetGCGlobalVolatility", "RedjetGCExecutionFocus",
  "RedjetGCAllowOperationDispatchings", "RedjetGCUseMLTCSolver",
  "RedjetGCPuzzlerMorph", "RedjetGCProfile", "RedjetGCSCMode",
  "RedjetGCForbidDynamicGrowth", "RedjetGCUpclockerQoS",
  "RedjetGCStrictVFS", "RedjetGCRMRP", "RedjetGCRMRPMethod"
};

struct RedjetCLIIntentEntry {
  const char* intent_keyword;
  const char* canonical_flag;
  const char* default_value;
  const char* technical_doc;
};

static const RedjetCLIIntentEntry REDJET_INTENT_REGISTRY[] = {
  // 1. Clock Frequency & Boost
  {"boost",            "Upclocker", "1",     "Overclock / maximum frequency boost"},
  {"turbo",            "Upclocker", "1",     "Turbo execution mode"},
  {"speed",            "Upclocker", "1",     "Maximum execution performance clocking"},
  {"upclock",          "Upclocker", "1",     "Host CPU upclocking"},
  {"accelerate",       "Upclocker", "1",     "Intrinsic hardware execution acceleration"},

  // 2. Memory Resident Set Reclamation (RMRP)
  {"trim",             "Reduce_Memory_Resident_Propagation", "true", "Trim and reclaim dormant heap pages"},
  {"shrink",           "Reduce_Memory_Resident_Propagation", "true", "Shrink resident physical memory set"},
  {"reclaim",          "Reduce_Memory_Resident_Propagation", "true", "Reclaim dormant heap propagation buffers"},
  {"compact_memory",   "Reduce_Memory_Resident_Propagation", "true", "Compact resident working set"},
  {"clean_heap",       "Reduce_Memory_Resident_Propagation", "true", "Clean resident heap footprint"},
  {"prune",            "Reduce_Memory_Resident_Propagation", "true", "Prune unreferenced slab blocks"},
  {"plump",            "Reduce_Memory_Resident_Propagation", "true", "Plump coalescing compaction"},
  {"decommit",         "Reduce_Memory_Resident_Propagation", "true", "Decommit idle virtual memory pages"},
  {"sequential",       "RMRP_Method", "Sequential", "Sequential RMRP memory resident reclamation"},
  {"installment",      "RMRP_Method", "Sequential", "Installment RMRP memory resident reclamation"},

  // 3. Vector Acceleration & SIMD GC
  {"vector",           "GC_Mode", "SIMD",    "Vector-accelerated garbage collector"},
  {"vectorize",        "GC_Mode", "SIMD",    "Vectorized SIMD memory sweeps"},
  {"simd_gc",          "GC_Mode", "SIMD",    "SIMD-native allocation and sweep"},
  {"vector_gc",        "GC_Mode", "SIMD",    "Vector lane GC processing"},
  {"avx_gc",           "GC_Mode", "SIMD",    "AVX/AVX-512 accelerated GC"},
  {"scalar_gc",        "GC_Mode", "Cycles",  "Cycle-based scalar GC evaluation"},
  {"static_gc",        "GC_Mode", "Static",  "Fixed deterministic static GC"},

  // 4. Parallel Dispatching & Work Stealing
  {"parallel",         "Allow_Operation_Dispatchings", "true", "Enable parallel operation dispatching"},
  {"steal",            "Allow_Operation_Dispatchings", "true", "Enable work-stealing across execution lanes"},
  {"dispatch",         "Allow_Operation_Dispatchings", "true", "Enable autonomous load dispatching"},
  {"work_stealing",    "Allow_Operation_Dispatchings", "true", "Saturated lane work stealing"},

  // 5. Safety-Critical Profile
  {"safety",           "Profile", "SC",      "Safety-critical deterministic profile"},
  {"critical",         "Profile", "SC",      "Mission-critical hard real-time operation"},
  {"realtime",         "Profile", "SC",      "Hard real-time bounded latency mode"},
  {"deterministic",    "Profile", "SC",      "Disable dynamic deopt and JIT jitter"},

  // 6. Compaction, Defrag & Fusion
  {"compact",          "Always_Focus_Compactizing", "true", "Continuous background heap defragmentation"},
  {"compactize",       "Always_Focus_Compactizing", "true", "Active memory cell coalescing"},
  {"defrag",           "Always_Focus_Compactizing", "true", "Active memory cell coalescing and defrag"},
  {"hugepages",        "Enable_Huge_Pages", "true", "2MB/1GB hardware huge pages"},
  {"huge_pages",       "Enable_Huge_Pages", "true", "Direct OS huge page allocation"},

  // 7. Execution Focus Topology
  {"scp",              "Execution_Focus", "SCP",        "Single-core performance focus on highest turbo core"},
  {"mcp",              "Execution_Focus", "MCP",        "Multi-core wide parallel throughput focus"},
  {"hybrid_dual",      "Execution_Focus", "HybridDual", "Asymmetric P+E dual performance focus"},
  {"dual",             "Execution_Focus", "HybridDual", "Dual performance focus"},
  {"scp_homo",         "Execution_Focus", "SCP_Homo",   "Single-core homogeneous peak turbo focus"},
  {"scp_hybrid",       "Execution_Focus", "SCP_Hybrid", "Single-core asymmetric strict P-core pinned focus"},
  {"mcp_homo",         "Execution_Focus", "MCP_Homo",   "Multi-core homogeneous uniform chunking focus"},
  {"mcp_hybrid",       "Execution_Focus", "MCP_Hybrid", "Multi-core asymmetric weighted chunking focus"},
  {"dual_homo",        "Execution_Focus", "Dual_Homo",  "Dual focus homogeneous (1 turbo + N-1 workers)"},
  {"dual_hybrid",      "Execution_Focus", "Dual_Hybrid","Dual focus asymmetric (P compute + E gc)"},

  // 8. VFS Branch Safety
  {"strict_vfs",       "Strict_VFS_Branch_Validation", "true", "Enforce strict VFS branch validation"},
  {"vfs_strict",       "Strict_VFS_Branch_Validation", "true", "Enforce strict VFS branch validation"},
  {"vfs_panic",        "Strict_VFS_Branch_Validation", "true", "Immediate safe panic when VFS branch not found"},
  {"strict_branch",    "Strict_VFS_Branch_Validation", "true", "Enforce strict VFS directory branch validation"},
  {"vfs_dialog",       "VFS_Panic_Dialog",             "true", "Enable modal alert dialog popup on fatal VFS panics"},

  // 9. Hardware, Volatility, Morph & 4-Points
  {"strict_ring3",     "Strict_Ring3_Only",            "true",   "Lock execution to safe Ring 3 APIs"},
  {"strict_flags",     "Strict_Flags",                 "true",   "Fail-fast on unknown flags or conflicts"},
  {"mltc_solver",      "UseMLTCSolver",                "true",   "Engage MLTC upward trend solver"},
  {"sram",             "Global_Volatility_Method",     "SRAM",   "L1/L2/3D-VCache SRAM volatility discipline"},
  {"dram",             "Global_Volatility_Method",     "DRAM",   "Standard DRAM volatility discipline"},
  {"shadow_morph",     "puzzler_Morph_Method",         "Shadow", "Lock-free Shadow RCU Pitstop morphing"},
  {"excision_morph",   "puzzler_Morph_Method",         "Excision","Exclusive Pitstop bay excision morphing"},
  {"entangle",         "Entangle_Execution",           "true",   "Enable Batch Register and Entangle Execution"},
  {"four_points",      "Enable_4Points",               "true",   "Enable interdependent 4-Point Engine"},
  {"pacing",           "Controlpoint_Pacing",          "true",   "Enable Controlpoint mutator allocation pacing"},

  // 10. Operational Profile & Determinism (NORMAX vs SSIOLSC)
  {"normax",           "Profile", "NORMAX",  "Normal maximum high-throughput SIMD execution profile"},
  {"standard",         "Profile", "NORMAX",  "Standard high-throughput execution profile"},
  {"ssiolsc",          "Profile", "SC",      "Safety-critical isolated hard real-time deterministic profile"},
  {"sc",               "Profile", "SC",      "Safety-critical isolated hard real-time deterministic profile"},
  {"safety_critical",  "Profile", "SC",      "Mission-critical hard real-time bounded latency mode"},
  {"zero_jitter",      "Profile", "SC",      "Zero-jitter deterministic execution envelope"},

  // 11. Scheduler: Domestic vs TBB Mutual Exclusion
  {"domestic",         "RedjetGCScheduler", "Domestic", "Domestic deterministic in-order silicon pipeline scheduler"},
  {"domestic_sched",   "RedjetGCScheduler", "Domestic", "Domestic deterministic in-order silicon pipeline scheduler"},
  {"tbb",              "RedjetGCScheduler", "TBB",      "Intel oneTBB task arena work-stealing scheduler"},
  {"tbb_sched",        "RedjetGCScheduler", "TBB",      "Intel oneTBB task arena work-stealing scheduler"}
};

static inline bool parse_bool_token(const char* val) {
  return (!val || !val[0] ||
          strcasecmp(val, "1") == 0 ||
          strcasecmp(val, "true") == 0 ||
          strcasecmp(val, "yes") == 0 ||
          strcasecmp(val, "allow") == 0 ||
          strcasecmp(val, "enable") == 0 ||
          strcasecmp(val, "enabled") == 0);
}

int RedjetFlags::damerau_levenshtein_distance(const char* s1, const char* s2) {
  int len1 = (int)strlen(s1);
  int len2 = (int)strlen(s2);
  if (len1 > 48 || len2 > 48) return 99;
  int d[50][50];
  for (int i = 0; i <= len1; ++i) d[i][0] = i;
  for (int j = 0; j <= len2; ++j) d[0][j] = j;
  for (int i = 1; i <= len1; ++i) {
    for (int j = 1; j <= len2; ++j) {
      int cost = (tolower((unsigned char)s1[i - 1]) == tolower((unsigned char)s2[j - 1])) ? 0 : 1;
      int del = d[i - 1][j] + 1;
      int ins = d[i][j - 1] + 1;
      int sub = d[i - 1][j - 1] + cost;
      int m = (del < ins) ? del : ins;
      d[i][j] = (m < sub) ? m : sub;
      if (i > 1 && j > 1 &&
          tolower((unsigned char)s1[i - 1]) == tolower((unsigned char)s2[j - 2]) &&
          tolower((unsigned char)s1[i - 2]) == tolower((unsigned char)s2[j - 1])) {
        d[i][j] = MIN2(d[i][j], d[i - 2][j - 2] + 1);
      }
    }
  }
  return d[len1][len2];
}

double RedjetFlags::jaro_winkler_similarity(const char* s1, const char* s2) {
  if (!s1 || !s2) return 0.0;
  int l1 = MIN2((int)strlen(s1), 64);
  int l2 = MIN2((int)strlen(s2), 64);
  if (l1 == 0 && l2 == 0) return 1.0;
  if (l1 == 0 || l2 == 0) return 0.0;

  int match_distance = MAX2(l1, l2) / 2 - 1;
  if (match_distance < 0) match_distance = 0;

  bool s1_matches[64] = { false };
  bool s2_matches[64] = { false };
  int matches = 0;

  for (int i = 0; i < l1 && i < 64; ++i) {
    int start = MAX2(0, i - match_distance);
    int end = MIN2(i + match_distance + 1, l2);
    for (int j = start; j < end && j < 64; ++j) {
      if (s2_matches[j]) continue;
      if (tolower((unsigned char)s1[i]) == tolower((unsigned char)s2[j])) {
        s1_matches[i] = true;
        s2_matches[j] = true;
        matches++;
        break;
      }
    }
  }
  if (matches == 0) return 0.0;

  double transpositions = 0;
  int k = 0;
  for (int i = 0; i < l1 && i < 64; ++i) {
    if (!s1_matches[i]) continue;
    while (k < l2 && k < 64 && !s2_matches[k]) k++;
    if (k < l2 && tolower((unsigned char)s1[i]) != tolower((unsigned char)s2[k])) transpositions += 0.5;
    k++;
  }

  double jaro = (((double)matches / l1) +
                 ((double)matches / l2) +
                 (((double)matches - transpositions) / matches)) / 3.0;

  int prefix = 0;
  for (int i = 0; i < MIN2(l1, MIN2(l2, 4)); ++i) {
    if (tolower((unsigned char)s1[i]) == tolower((unsigned char)s2[i])) prefix++;
    else break;
  }
  return jaro + prefix * 0.1 * (1.0 - jaro);
}

void RedjetFlags::strip_prefix_and_split(const char* raw_token, const char* raw_val,
                                         char* clean_key, size_t key_sz,
                                         char* clean_val, size_t val_sz) {
  if (!raw_token || !clean_key || key_sz == 0) return;
  clean_key[0] = '\0';
  if (clean_val && val_sz > 0) clean_val[0] = '\0';

  const char* p = raw_token;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

  // Strip JVM / HSVM CLI prefixes (+#HS|, -XX:RedjetGC, -XX:Redjet, -XX:+, -XX:-, --sf:, etc.)
  if (strncasecmp(p, "+#HS|", 5) == 0 || strncasecmp(p, "-#HS|", 5) == 0) {
    p += 5;
  } else if (strncasecmp(p, "--sf|", 5) == 0 || strncasecmp(p, "++sf|", 5) == 0 ||
             strncasecmp(p, "--sf:", 5) == 0 || strncasecmp(p, "++sf:", 5) == 0 ||
             strncasecmp(p, "--sf-", 5) == 0 || strncasecmp(p, "++sf-", 5) == 0) {
    p += 5;
  } else if (strncasecmp(p, "-sf|", 4) == 0 || strncasecmp(p, "+sf|", 4) == 0 ||
             strncasecmp(p, "-sf:", 4) == 0 || strncasecmp(p, "+sf:", 4) == 0 ||
             strncasecmp(p, "-sf-", 4) == 0 || strncasecmp(p, "+sf-", 4) == 0) {
    p += 4;
  } else if (strncasecmp(p, "-XX:Redjet", 10) == 0) {
    p += 10;
  } else if (strncasecmp(p, "+XX:Redjet", 10) == 0) {
    p += 10;
  } else if (strncmp(p, "-XX:+", 5) == 0 || strncmp(p, "-XX:-", 5) == 0) {
    p += 5;
  } else if (strncmp(p, "-XX:", 4) == 0 || strncmp(p, "+XX:", 4) == 0) {
    p += 4;
  } else if (p[0] == '-' && p[1] == '-') {
    p += 2;
  } else if (p[0] == '+' && p[1] == '+') {
    p += 2;
  } else if (p[0] == '-' || p[0] == '+' || p[0] == '/') {
    p += 1;
  }

  const char* delim = nullptr;
  for (const char* c = p; *c; ++c) {
    if (*c == ':' || *c == '=') {
      delim = c;
      break;
    }
  }

  if (delim && (!raw_val || raw_val[0] == '\0')) {
    size_t klen = (size_t)(delim - p);
    if (klen >= key_sz) klen = key_sz - 1;
    strncpy(clean_key, p, klen);
    clean_key[klen] = '\0';

    const char* v = delim + 1;
    while (*v == ' ' || *v == '\t') v++;
    if (clean_val && val_sz > 0) {
      strncpy(clean_val, v, val_sz - 1);
      clean_val[val_sz - 1] = '\0';
      size_t vlen = strlen(clean_val);
      while (vlen > 0 && (clean_val[vlen - 1] == ' ' || clean_val[vlen - 1] == '\t' ||
             clean_val[vlen - 1] == '"' || clean_val[vlen - 1] == '\'')) {
        clean_val[--vlen] = '\0';
      }
      if (clean_val[0] == '"' || clean_val[0] == '\'') {
        memmove(clean_val, clean_val + 1, vlen);
      }
    }
  } else {
    strncpy(clean_key, p, key_sz - 1);
    clean_key[key_sz - 1] = '\0';
    if (raw_val && raw_val[0] && clean_val && val_sz > 0) {
      const char* rv = raw_val;
      while (*rv == ' ' || *rv == '\t') rv++;
      strncpy(clean_val, rv, val_sz - 1);
      clean_val[val_sz - 1] = '\0';
      size_t vlen = strlen(clean_val);
      while (vlen > 0 && (clean_val[vlen - 1] == ' ' || clean_val[vlen - 1] == '\t' ||
             clean_val[vlen - 1] == '"' || clean_val[vlen - 1] == '\'')) {
        clean_val[--vlen] = '\0';
      }
      if (clean_val[0] == '"' || clean_val[0] == '\'') {
        memmove(clean_val, clean_val + 1, vlen);
      }
    }
  }

  size_t klen = strlen(clean_key);
  while (klen > 0 && (clean_key[klen - 1] == ' ' || clean_key[klen - 1] == '\t')) {
    clean_key[--klen] = '\0';
  }
}

RedjetCLIResolution RedjetFlags::resolve_argument(const char* input_key, const char* input_val) {
  RedjetCLIResolution res{};
  if (!input_key || !input_key[0]) return res;

  char clean_k[64] = { 0 };
  char clean_v[64] = { 0 };
  strip_prefix_and_split(input_key, input_val, clean_k, sizeof(clean_k), clean_v, sizeof(clean_v));
  if (clean_k[0] == '\0') return res;

  // 1. Direct semantic intent lookup in technical English registry
  for (const auto& entry : REDJET_INTENT_REGISTRY) {
    if (strcasecmp(clean_k, entry.intent_keyword) == 0) {
      res.matched = true;
      res.was_semantic_intent = true;
      strncpy(res.canonical_flag, entry.canonical_flag, sizeof(res.canonical_flag) - 1);
      if (clean_v[0]) {
        strncpy(res.canonical_value, clean_v, sizeof(res.canonical_value) - 1);
      } else {
        strncpy(res.canonical_value, entry.default_value, sizeof(res.canonical_value) - 1);
      }
      res.confidence = 1.0;
      res.technical_doc = entry.technical_doc;
      return res;
    }
  }

  // 2. Direct exact match against known canonical flags
  for (const char* known : REDJET_KNOWN_FLAGS) {
    if (strcasecmp(clean_k, known) == 0) {
      res.matched = true;
      strncpy(res.canonical_flag, known, sizeof(res.canonical_flag) - 1);
      if (clean_v[0]) {
        strncpy(res.canonical_value, clean_v, sizeof(res.canonical_value) - 1);
      } else {
        strncpy(res.canonical_value, "1", sizeof(res.canonical_value) - 1);
      }
      res.confidence = 1.0;
      res.technical_doc = "Exact canonical flag match";
      return res;
    }
  }

  // 3. Fuzzy match against technical English semantic intents
  double best_intent_score = 0.0;
  const RedjetCLIIntentEntry* best_intent = nullptr;
  for (const auto& entry : REDJET_INTENT_REGISTRY) {
    double sim = jaro_winkler_similarity(clean_k, entry.intent_keyword);
    int dist = damerau_levenshtein_distance(clean_k, entry.intent_keyword);
    if ((sim >= 0.85 || dist <= 1) && sim > best_intent_score) {
      best_intent_score = sim;
      best_intent = &entry;
    }
  }
  if (best_intent && best_intent_score >= 0.80) {
    res.matched = true;
    res.was_semantic_intent = true;
    strncpy(res.canonical_flag, best_intent->canonical_flag, sizeof(res.canonical_flag) - 1);
    if (clean_v[0]) {
      strncpy(res.canonical_value, clean_v, sizeof(res.canonical_value) - 1);
    } else {
      strncpy(res.canonical_value, best_intent->default_value, sizeof(res.canonical_value) - 1);
    }
    res.confidence = best_intent_score;
    res.technical_doc = best_intent->technical_doc;
    return res;
  }

  // 4. Fuzzy match against canonical known flags
  double best_flag_score = 0.0;
  const char* best_flag = nullptr;
  for (const char* known : REDJET_KNOWN_FLAGS) {
    double sim = jaro_winkler_similarity(clean_k, known);
    int dist = damerau_levenshtein_distance(clean_k, known);
    if ((sim >= 0.82 || dist <= 2) && sim > best_flag_score) {
      best_flag_score = sim;
      best_flag = known;
    }
  }
  if (best_flag && best_flag_score >= 0.78) {
    res.matched = true;
    res.was_fuzzy_typo = true;
    strncpy(res.canonical_flag, best_flag, sizeof(res.canonical_flag) - 1);
    if (clean_v[0]) {
      strncpy(res.canonical_value, clean_v, sizeof(res.canonical_value) - 1);
    } else {
      strncpy(res.canonical_value, "1", sizeof(res.canonical_value) - 1);
    }
    res.confidence = best_flag_score;
    res.technical_doc = "Fuzzy typo matched to canonical flag";
    return res;
  }

  return res;
}

uint64_t RedjetFlags::parse_size_with_unit(const char* str) {
  if (!str || !*str) return 0;
  char* end = nullptr;
  double val = std::strtod(str, &end);
  if (val < 0) val = 0;
  if (end) {
    while (*end == ' ') end++;
    if (strcasecmp(end, "gb") == 0 || strcasecmp(end, "g") == 0) {
      return (uint64_t)(val * 1024.0 * 1024.0 * 1024.0);
    } else if (strcasecmp(end, "tb") == 0 || strcasecmp(end, "t") == 0) {
      return (uint64_t)(val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
    } else if (strcasecmp(end, "mb") == 0 || strcasecmp(end, "m") == 0) {
      return (uint64_t)(val * 1024.0 * 1024.0);
    } else if (strcasecmp(end, "kb") == 0 || strcasecmp(end, "k") == 0) {
      return (uint64_t)(val * 1024.0);
    } else if (strcasecmp(end, "b") == 0) {
      return (uint64_t)val;
    }
  }
  return (uint64_t)val;
}

uint64_t RedjetFlags::parse_time_with_unit_to_ns(const char* str) {
  if (!str || !*str) return 0;
  char* end = nullptr;
  double val = std::strtod(str, &end);
  if (val < 0) val = 0;
  if (end) {
    while (*end == ' ') end++;
    if (strcasecmp(end, "s") == 0 || strcasecmp(end, "sec") == 0 ||
        strcasecmp(end, "m") == 0 || strcasecmp(end, "min") == 0) {
      return (uint64_t)(val * 1000000000.0);
    }
    if (strcasecmp(end, "ms") == 0) {
      return (uint64_t)(val * 1000000.0);
    }
    if (strcasecmp(end, "us") == 0) {
      return (uint64_t)(val * 1000.0);
    }
    if (strcasecmp(end, "ns") == 0) {
      return (uint64_t)val;
    }
  }
  return (uint64_t)(val * 1000000.0);
}

bool RedjetFlags::parse_single_flag_internal(RedjetConfig& cfg, const char* key, const char* val) {
  if (!key || !val) return false;

  if (strcasecmp(key, "GC_Mode") == 0 || strcasecmp(key, "GCMode") == 0 ||
      strcasecmp(key, "GC_Mod") == 0 || strcasecmp(key, "GCMod") == 0) {
    strncpy(cfg.strings.gc_mode_raw, val, sizeof(cfg.strings.gc_mode_raw) - 1);
    if (strcasecmp(val, "Static") == 0) {
      cfg.strings.gc_mode = RedjetGCMode::STATIC;
      cfg.strings.gc_operation_facility = RedjetGCOperationFacility::STATIC_FIXED;
    } else if (strcasecmp(val, "SIMD") == 0 || strcasecmp(val, "Vector") == 0) {
      cfg.strings.gc_mode = RedjetGCMode::SIMD;
      cfg.strings.gc_operation_facility = RedjetGCOperationFacility::SIMD;
    } else if (strcasecmp(val, "Cycles") == 0 || strcasecmp(val, "Scalar") == 0) {
      cfg.strings.gc_mode = RedjetGCMode::CYCLES;
      cfg.strings.gc_operation_facility = RedjetGCOperationFacility::CYCLES;
    } else {
      cfg.strings.gc_mode = RedjetGCMode::HEURISTIC;
    }
  } else if (strcasecmp(key, "GC_Operation_Facility") == 0) {
    strncpy(cfg.strings.gc_operation_facility_raw, val, sizeof(cfg.strings.gc_operation_facility_raw) - 1);
    if (strcasecmp(val, "SIMD") == 0) {
      cfg.strings.gc_operation_facility = RedjetGCOperationFacility::SIMD;
    } else if (strcasecmp(val, "Static") == 0 || strcasecmp(val, "STATIC_FIXED") == 0) {
      cfg.strings.gc_operation_facility = RedjetGCOperationFacility::STATIC_FIXED;
    } else {
      cfg.strings.gc_operation_facility = RedjetGCOperationFacility::CYCLES;
    }
  } else if (strcasecmp(key, "Use_Critical_SIMDly_Operations") == 0 ||
             strcasecmp(key, "Use_Critical_SIMD_Operations") == 0 ||
             strcasecmp(key, "Critical_SIMDly_Operations") == 0) {
    cfg.strings.critical_simd_explicit = true;
    strncpy(cfg.strings.critical_simd_raw, val, sizeof(cfg.strings.critical_simd_raw) - 1);
    if (strcasecmp(val, "Allow") == 0 || strcasecmp(val, "Allowed") == 0 ||
        strcasecmp(val, "1") == 0 || strcasecmp(val, "true") == 0) {
      cfg.strings.critical_simd_mode = RedjetCriticalSIMD::ALLOW;
    } else if (strcasecmp(val, "Deny") == 0 || strcasecmp(val, "Denied") == 0 ||
               strcasecmp(val, "0") == 0 || strcasecmp(val, "false") == 0) {
      cfg.strings.critical_simd_mode = RedjetCriticalSIMD::DENY;
    } else {
      cfg.strings.critical_simd_mode = RedjetCriticalSIMD::AUTO;
    }
  } else if (strcasecmp(key, "Snatch_and_Rebate_Mode") == 0 || strcasecmp(key, "SnatchAndRebateMode") == 0) {
    strncpy(cfg.strings.snatch_rebate_raw, val, sizeof(cfg.strings.snatch_rebate_raw) - 1);
    if (strcasecmp(val, "Rapid") == 0) {
      cfg.strings.snatch_rebate_mode = RedjetSnatchRebateMode::RAPID;
    } else if (strcasecmp(val, "Lazy") == 0) {
      cfg.strings.snatch_rebate_mode = RedjetSnatchRebateMode::LAZY;
    } else if (strcasecmp(val, "GYC") == 0) {
      cfg.strings.snatch_rebate_mode = RedjetSnatchRebateMode::GYC;
    } else if (strcasecmp(val, "Decommit") == 0) {
      cfg.strings.snatch_rebate_mode = RedjetSnatchRebateMode::DECOMMIT;
    } else {
      cfg.strings.snatch_rebate_mode = RedjetSnatchRebateMode::AUTO;
    }
  } else if (strcasecmp(key, "GC_Heuristic") == 0 || strcasecmp(key, "Heuristic") == 0) {
    strncpy(cfg.strings.gc_heuristic_raw, val, sizeof(cfg.strings.gc_heuristic_raw) - 1);
    if (strcasecmp(val, "Aggressive") == 0) {
      cfg.strings.gc_heuristic = RedjetGCHeuristic::AGGRESSIVE;
    } else if (strcasecmp(val, "Bureaucratic") == 0) {
      cfg.strings.gc_heuristic = RedjetGCHeuristic::BUREAUCRATIC;
    } else if (strcasecmp(val, "Disabled") == 0 || strcasecmp(val, "None") == 0 ||
               strcasecmp(val, "No_hsGC") == 0) {
      cfg.strings.gc_heuristic = RedjetGCHeuristic::DISABLED;
      cfg.strings.no_sfgc_overwrite_mode = true;
    } else {
      cfg.strings.gc_heuristic = RedjetGCHeuristic::ADAPTIVE;
    }
  } else if (strcasecmp(key, "FreeList_Recycle_Policy") == 0 ||
             strcasecmp(key, "FreeListRecyclePolicy") == 0 ||
             strcasecmp(key, "Freelist_Policy") == 0) {
    strncpy(cfg.strings.freelist_policy_raw, val, sizeof(cfg.strings.freelist_policy_raw) - 1);
    if (strcasecmp(val, "FIFO") == 0) {
      cfg.integers.freelist_recycle_policy = 1;
    } else if (strcasecmp(val, "LIFO") == 0) {
      cfg.integers.freelist_recycle_policy = 0;
    } else {
      cfg.integers.freelist_recycle_policy = (uint32_t)std::strtoul(val, nullptr, 10);
    }
  } else if (strcasecmp(key, "Enable_Huge_Pages") == 0 || strcasecmp(key, "EnableHugePages") == 0) {
    cfg.strings.enable_huge_pages = parse_bool_token(val);
  } else if (strcasecmp(key, "Advance_Payment") == 0 || strcasecmp(key, "AlwaysPreTouch") == 0 ||
             strcasecmp(key, "Always_PreTouch") == 0 || strcasecmp(key, "PreTouch") == 0) {
    cfg.strings.advance_payment = parse_bool_token(val);
  } else if (strcasecmp(key, "No_hsGC") == 0 || strcasecmp(key, "Zero_GC") == 0) {
    bool en = parse_bool_token(val);
    cfg.strings.no_sfgc_overwrite_mode = en;
    if (en) {
      cfg.strings.gc_heuristic = RedjetGCHeuristic::DISABLED;
    }
  } else if (strcasecmp(key, "Allow_Operation_Dispatchings") == 0 ||
             strcasecmp(key, "AllowOperationDispatchings") == 0 ||
             strcasecmp(key, "Operation_Dispatchings") == 0) {
    cfg.strings.allow_operation_dispatchings = parse_bool_token(val);
  } else if (strcasecmp(key, "Reduce_Memory_Resident_Propagation") == 0 ||
             strcasecmp(key, "ReduceMemoryResidentPropagation") == 0 ||
             strcasecmp(key, "RMRP") == 0) {
    cfg.strings.reduce_memory_resident_prop = parse_bool_token(val);
  } else if (strcasecmp(key, "RMRP_Method") == 0 || strcasecmp(key, "RMRPMethod") == 0) {
    cfg.strings.rmrp_method_explicit = true;
    strncpy(cfg.strings.rmrp_method_raw, val, sizeof(cfg.strings.rmrp_method_raw) - 1);
    if (strcasecmp(val, "Sequential") == 0 || strcasecmp(val, "Installment") == 0) {
      cfg.strings.rmrp_method = RedjetRMRPMethod::SEQUENTIAL;
    } else {
      cfg.strings.rmrp_method = RedjetRMRPMethod::PLUMP;
    }
  } else if (strcasecmp(key, "Global_Volatility_Method") == 0 ||
             strcasecmp(key, "GlobalVolatilityMethod") == 0 ||
             strcasecmp(key, "GlobalVolatility") == 0 ||
             strcasecmp(key, "RedjetGlobalVolatility") == 0) {
    strncpy(cfg.strings.global_volatility_raw, val, sizeof(cfg.strings.global_volatility_raw) - 1);
    if (strcasecmp(val, "SRAM") == 0 || strcasecmp(val, "Cache") == 0 || strcasecmp(val, "L1L2") == 0) {
      cfg.strings.global_volatility = RedjetGlobalVolatility::SRAM;
    } else {
      cfg.strings.global_volatility = RedjetGlobalVolatility::DRAM;
    }
  } else if (strcasecmp(key, "puzzler_Morph_Method") == 0 ||
             strcasecmp(key, "puzzlerMorphMethod") == 0 ||
             strcasecmp(key, "Morph_Method") == 0 ||
             strcasecmp(key, "PuzzlerMorph") == 0 ||
             strcasecmp(key, "RedjetPuzzlerMorph") == 0) {
    strncpy(cfg.strings.puzzler_morph_raw, val, sizeof(cfg.strings.puzzler_morph_raw) - 1);
    if (strcasecmp(val, "Excision") == 0 || strcasecmp(val, "Exclusive") == 0 || strcasecmp(val, "Busy") == 0) {
      cfg.strings.puzzler_morph = RedjetPuzzlerMorphMode::EXCISION_EXCLUSIVE;
    } else {
      cfg.strings.puzzler_morph = RedjetPuzzlerMorphMode::SHADOW_RCU;
    }
  } else if (strcasecmp(key, "Batch_Register_and_Entangle_Execution") == 0 ||
             strcasecmp(key, "Batch_Register_And_Entangle_Execution") == 0 ||
             strcasecmp(key, "Entangle_Execution") == 0 ||
             strcasecmp(key, "EntangleExecution") == 0 ||
             strcasecmp(key, "RedjetEntangleExecution") == 0) {
    cfg.strings.entangle_execution = parse_bool_token(val);
  } else if (strcasecmp(key, "Experimental") == 0) {
    bool en = parse_bool_token(val);
    cfg.strings.experimental_options = en;
    cfg.unlock_experimental = en;
  } else if (strcasecmp(key, "UseMLTCSolver") == 0 || strcasecmp(key, "Use_MLTC_Solver") == 0 ||
             strcasecmp(key, "MLTCSolver") == 0 || strcasecmp(key, "MLTC_Solver") == 0) {
    cfg.use_mltc_solver = parse_bool_token(val);
  } else if (strcasecmp(key, "Forbid_Dynamic_Growth") == 0 || strcasecmp(key, "ForbidDynamicGrowth") == 0 ||
             strcasecmp(key, "Forbid_Runtime_Heap") == 0 || strcasecmp(key, "ForbidRuntimeHeap") == 0) {
    cfg.forbid_dynamic_growth = parse_bool_token(val);
  } else if (strcasecmp(key, "Max_Bounded_Iterations") == 0 || strcasecmp(key, "Max_Iterations") == 0 ||
             strcasecmp(key, "MaxIterations") == 0) {
    cfg.integers.max_loop_iterations = (uint32_t)std::strtoul(val, nullptr, 10);
  } else if (strcasecmp(key, "Upclocker") == 0 || strcasecmp(key, "Boost") == 0 ||
             strcasecmp(key, "Turbo") == 0 || strcasecmp(key, "Upclock") == 0) {
    bool en = (parse_bool_token(val) || strcasecmp(val, "max") == 0);
    cfg.strings.upclocker_enabled = en;
  } else if (strcasecmp(key, "Always_Focus_Compactizing") == 0 || strcasecmp(key, "Always_Compact") == 0) {
    cfg.strings.always_focus_compactizing = parse_bool_token(val);
  } else if (strcasecmp(key, "Execution_Focus") == 0 || strcasecmp(key, "ExecutionFocus") == 0 ||
             strcasecmp(key, "Focus") == 0) {
    strncpy(cfg.strings.execution_focus_raw, val, sizeof(cfg.strings.execution_focus_raw) - 1);
    if (strcasecmp(val, "SCP") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::SCP;
    } else if (strcasecmp(val, "MCP") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::MCP;
    } else if (strcasecmp(val, "HybridDual") == 0 || strcasecmp(val, "Dual") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::HYBRID_DUAL;
    } else if (strcasecmp(val, "SCP_Homo") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::SCP_HOMO;
    } else if (strcasecmp(val, "SCP_Hybrid") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::SCP_HYBRID;
    } else if (strcasecmp(val, "MCP_Homo") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::MCP_HOMO;
    } else if (strcasecmp(val, "MCP_Hybrid") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::MCP_HYBRID;
    } else if (strcasecmp(val, "Dual_Homo") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::DUAL_HOMO;
    } else if (strcasecmp(val, "Dual_Hybrid") == 0) {
      cfg.strings.execution_focus = RedjetExecutionFocus::DUAL_HYBRID;
    } else {
      cfg.strings.execution_focus = RedjetExecutionFocus::AUTO;
    }
  } else if (strcasecmp(key, "Strict_VFS_Branch_Validation") == 0 ||
             strcasecmp(key, "StrictVFSBranchValidation") == 0 ||
             strcasecmp(key, "Strict_VFS") == 0 || strcasecmp(key, "StrictVFS") == 0 ||
             strcasecmp(key, "VFS_Strict") == 0 || strcasecmp(key, "VFSStrict") == 0 ||
             strcasecmp(key, "Strict_VFS_Validation") == 0) {
    cfg.strings.strict_vfs_branch_validation = parse_bool_token(val);
  } else if (strcasecmp(key, "VFS_Panic_Dialog") == 0 || strcasecmp(key, "VFSPanicDialog") == 0 ||
             strcasecmp(key, "Panic_Dialog") == 0) {
    cfg.strings.vfs_panic_dialog = parse_bool_token(val);
  } else if (strcasecmp(key, "Runway_Handsel_Attempt_Value") == 0) {
    cfg.integers.runway_handsel_bytes = (size_t)parse_size_with_unit(val);
  } else if (strcasecmp(key, "Landfill_Handsel_Attempt_Value") == 0) {
    cfg.integers.landfill_handsel_bytes = (size_t)parse_size_with_unit(val);
  } else if (strcasecmp(key, "Wakes_GC_Cycling_Interval") == 0) {
    cfg.integers.wakes_gc_interval_ns = parse_time_with_unit_to_ns(val);
    cfg.integers.wakes_gc_interval_explicit = true;
  } else if (strcasecmp(key, "Line_Compaction_Freq") == 0 || strcasecmp(key, "LineCompactionFreq") == 0) {
    cfg.integers.line_compaction_freq_ns = parse_time_with_unit_to_ns(val);
  } else if (strcasecmp(key, "In_Heap_Forwarding_Rate") == 0 || strcasecmp(key, "InHeapForwardingRate") == 0) {
    cfg.integers.in_heap_forwarding_rate = (uint32_t)std::strtoul(val, nullptr, 10);
  } else if (strcasecmp(key, "Code_Arena_Bytes") == 0 || strcasecmp(key, "CodeArenaBytes") == 0 ||
             strcasecmp(key, "Code_Arena_Size") == 0 || strcasecmp(key, "Code_Heap_Size") == 0) {
    cfg.integers.code_arena_bytes = (size_t)parse_size_with_unit(val);
  } else if (strcasecmp(key, "InlineASM_Threshold") == 0 || strcasecmp(key, "IASM_Threshold") == 0 ||
             strcasecmp(key, "InlineAsmThreshold") == 0 || strcasecmp(key, "RedjetInlineAsmThreshold") == 0) {
    uint32_t thr = (uint32_t)std::strtoul(val, nullptr, 10);
    if (thr > 10) thr = 10;
    cfg.integers.inline_asm_threshold = thr;
    cfg.integers.inline_asm_threshold_explicit = true;
  } else if (strcasecmp(key, "Strict_Ring3_Only") == 0 || strcasecmp(key, "StrictRing3Only") == 0 ||
             strcasecmp(key, "Strict_Ring3") == 0 || strcasecmp(key, "StrictRing3") == 0) {
    cfg.strings.strict_ring3_only = parse_bool_token(val);
    if (cfg.strings.strict_ring3_only) {
      cfg.integers.inline_asm_threshold = 10;
    }
  } else if (strcasecmp(key, "Pressure_Mild_Ratio") == 0 || strcasecmp(key, "PressureMildRatio") == 0) {
    cfg.floats.pressure_mild_ratio = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Pressure_High_Ratio") == 0 || strcasecmp(key, "PressureHighRatio") == 0) {
    cfg.floats.pressure_high_ratio = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Pressure_Critical_Ratio") == 0 || strcasecmp(key, "PressureCriticalRatio") == 0) {
    cfg.floats.pressure_critical_ratio = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Landfill_Critical_Ratio") == 0 || strcasecmp(key, "LandfillCriticalRatio") == 0 ||
             strcasecmp(key, "RedjetLandfillCriticalRatio") == 0) {
    cfg.floats.landfill_critical_ratio = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Falline_Compact_Ratio") == 0 || strcasecmp(key, "FallineCompactRatio") == 0) {
    cfg.floats.falline_compact_ratio = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Heat_Decay_Factor") == 0 || strcasecmp(key, "HeatDecayFactor") == 0 ||
             strcasecmp(key, "RedjetHeatDecayFactor") == 0) {
    cfg.floats.heat_decay_factor = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Heat_Gain_Weight") == 0 || strcasecmp(key, "HeatGainWeight") == 0 ||
             strcasecmp(key, "RedjetHeatGainWeight") == 0) {
    cfg.floats.heat_gain_weight = (float)std::strtod(val, nullptr);
  } else if (strcasecmp(key, "Pressure_Threshold") == 0 || strcasecmp(key, "PressureThreshold") == 0) {
    cfg.integers.pressure_threshold_bytes = (size_t)parse_size_with_unit(val);
  } else if (strcasecmp(key, "Slab_Chunk_Size") == 0 || strcasecmp(key, "SlabChunkSize") == 0) {
    cfg.integers.slab_chunk_size_bytes = (size_t)parse_size_with_unit(val);
  } else if (strcasecmp(key, "Strict_Flags") == 0 || strcasecmp(key, "StrictFlags") == 0 ||
             strcasecmp(key, "Strict_Flag_Validation") == 0 || strcasecmp(key, "StrictFlagValidation") == 0) {
    cfg.strings.strict_flags = parse_bool_token(val);
  } else if (strcasecmp(key, "Enable_4Points") == 0 || strcasecmp(key, "Enable4Points") == 0 ||
             strcasecmp(key, "RedjetEnable4Points") == 0) {
    cfg.enable_4points = parse_bool_token(val);
  } else if (strcasecmp(key, "Pausepoint_Spin_Count") == 0 || strcasecmp(key, "PausepointSpinCount") == 0 ||
             strcasecmp(key, "RedjetPausepointSpinCount") == 0) {
    uint32_t spins = (uint32_t)std::strtoul(val, nullptr, 10);
    cfg.integers.pausepoint_spin_count = (spins > 0) ? spins : 256u;
  } else if (strcasecmp(key, "Controlpoint_Pacing") == 0 || strcasecmp(key, "ControlpointPacing") == 0 ||
             strcasecmp(key, "RedjetControlpointPacing") == 0) {
    cfg.controlpoint_pacing = parse_bool_token(val);
  } else if (strcasecmp(key, "Profile") == 0 || strcasecmp(key, "RedjetProfile") == 0 ||
             strcasecmp(key, "RedjetSCMode") == 0 || strcasecmp(key, "NORMAX") == 0 ||
             strcasecmp(key, "SSIOLSC") == 0) {
    const char* effective_val = val;
    if (strcasecmp(key, "NORMAX") == 0) {
      effective_val = "NORMAX";
    } else if (strcasecmp(key, "SSIOLSC") == 0) {
      effective_val = "SSIOLSC";
    } else if (strcasecmp(key, "RedjetSCMode") == 0) {
      effective_val = parse_bool_token(val) ? "SSIOLSC" : "NORMAX";
    }
    strncpy(cfg.strings.profile_raw, effective_val, sizeof(cfg.strings.profile_raw) - 1);
    if (strcasecmp(effective_val, "SC") == 0 || strcasecmp(effective_val, "SSIOLSC") == 0 ||
        strcasecmp(effective_val, "SafetyCritical") == 0 || strcasecmp(effective_val, "Safety_Critical") == 0) {
      cfg.strings.profile = RedjetProfile::SSIOLSC;
      cfg.strings.is_safety_critical = true;
      cfg.forbid_dynamic_growth = true;
      cfg.strings.advance_payment = true; // AlwaysPreTouch
      cfg.strings.allow_operation_dispatchings = false; // eliminate scheduling jitter
      cfg.strings.gc_heuristic = RedjetGCHeuristic::BUREAUCRATIC; // predictable phase STW
    } else {
      cfg.strings.profile = RedjetProfile::NORMAX;
      cfg.strings.is_safety_critical = false;
      cfg.forbid_dynamic_growth = false;
    }
  } else if (strcasecmp(key, "SCGC_Type") == 0 || strcasecmp(key, "SCGCType") == 0) {
    strncpy(cfg.strings.scgc_type_raw, val, sizeof(cfg.strings.scgc_type_raw) - 1);
    if (strcasecmp(val, "PrimitiveRW") == 0 || strcasecmp(val, "Primitive_RW") == 0) {
      cfg.strings.scgc_type = RedjetSCGCType::PRIMITIVE_RW;
      cfg.strings.no_sfgc_overwrite_mode = true;
      cfg.strings.gc_heuristic = RedjetGCHeuristic::DISABLED;
    } else {
      cfg.strings.scgc_type = RedjetSCGCType::DEFAULT;
      cfg.strings.no_sfgc_overwrite_mode = false;
      cfg.strings.gc_heuristic = RedjetGCHeuristic::BUREAUCRATIC;
    }
  } else if (strcasecmp(key, "RedjetGCScheduler") == 0 ||
             strcasecmp(key, "GCScheduler") == 0 ||
             strcasecmp(key, "Scheduler") == 0) {
    strncpy(cfg.strings.scheduler_raw, val, sizeof(cfg.strings.scheduler_raw) - 1);
    if (strcasecmp(val, "TBB") == 0) {
      cfg.strings.scheduler_type = RedjetGCSchedulerType::TBB;
    } else {
      cfg.strings.scheduler_type = RedjetGCSchedulerType::DOMESTIC;
    }
  } else if (strcasecmp(key, "RedjetGCAutoScalarOnSIMDContention") == 0 ||
             strcasecmp(key, "RedjetDynamicSIMDFallback") == 0 ||
             strcasecmp(key, "AutoScalarOnSIMDContention") == 0) {
    cfg.strings.auto_scalar_on_simd_contention = parse_bool_token(val);
  } else if (strcasecmp(key, "RedjetGCCriticalSIMDOnly") == 0 ||
             strcasecmp(key, "RedjetCriticalSIMDOnly") == 0) {
    cfg.strings.critical_simd_explicit = true;
    cfg.strings.critical_simd_mode = parse_bool_token(val) ? RedjetCriticalSIMD::ALLOW : RedjetCriticalSIMD::AUTO;
  } else if (strcasecmp(key, "RedjetGCGlobalVolatility") == 0) {
    return parse_single_flag_internal(cfg, "Global_Volatility_Method", val);
  } else if (strcasecmp(key, "RedjetGCExecutionFocus") == 0) {
    return parse_single_flag_internal(cfg, "Execution_Focus", val);
  } else if (strcasecmp(key, "RedjetGCAllowOperationDispatchings") == 0) {
    return parse_single_flag_internal(cfg, "Allow_Operation_Dispatchings", val);
  } else if (strcasecmp(key, "RedjetGCUseMLTCSolver") == 0) {
    return parse_single_flag_internal(cfg, "UseMLTCSolver", val);
  } else if (strcasecmp(key, "RedjetGCPuzzlerMorph") == 0) {
    return parse_single_flag_internal(cfg, "puzzler_Morph_Method", val);
  } else if (strcasecmp(key, "RedjetGCProfile") == 0) {
    return parse_single_flag_internal(cfg, "Profile", val);
  } else if (strcasecmp(key, "RedjetGCSCMode") == 0) {
    return parse_single_flag_internal(cfg, "RedjetSCMode", val);
  } else if (strcasecmp(key, "RedjetGCForbidDynamicGrowth") == 0) {
    return parse_single_flag_internal(cfg, "Forbid_Dynamic_Growth", val);
  } else if (strcasecmp(key, "RedjetGCUpclockerQoS") == 0) {
    return parse_single_flag_internal(cfg, "Upclocker", val);
  } else if (strcasecmp(key, "RedjetGCStrictVFS") == 0) {
    return parse_single_flag_internal(cfg, "Strict_VFS_Branch_Validation", val);
  } else if (strcasecmp(key, "RedjetGCRMRP") == 0) {
    return parse_single_flag_internal(cfg, "RMRP", val);
  } else if (strcasecmp(key, "RedjetGCRMRPMethod") == 0) {
    return parse_single_flag_internal(cfg, "RMRP_Method", val);
  } else {
    return false;
  }
  return true;
}

bool RedjetFlags::parse_single_flag(const char* flag_str) {
  if (!flag_str || !flag_str[0]) return false;

  char key[64] = { 0 };
  char val[64] = { 0 };
  strip_prefix_and_split(flag_str, nullptr, key, sizeof(key), val, sizeof(val));
  if (key[0] == '\0') return false;

  if (!parse_single_flag_internal(_config, key, val)) {
    RedjetCLIResolution res = resolve_argument(key, val);
    if (res.matched) {
      log_info(gc, init)("Redjet flag resolved [%s -> %s=%s] (confidence: %.2f, reason: %s)",
                         key, res.canonical_flag, res.canonical_value, res.confidence,
                         res.was_semantic_intent ? "Intent Registry" : "Fuzzy Match");
      return parse_single_flag_internal(_config, res.canonical_flag, res.canonical_value);
    } else {
      if (_config.strings.strict_flags) {
        log_error(gc, init)("Redjet strict flag error: unknown flag '%s'", flag_str);
      } else {
        log_warning(gc, init)("Redjet unrecognized flag: '%s'", flag_str);
      }
      return false;
    }
  }
  return true;
}

void RedjetFlags::parse_flag_sequence(const char* sequence_str) {
  if (!sequence_str || !sequence_str[0]) return;

  const char* p = sequence_str;
  while (*p != '\0') {
    while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') p++;
    if (*p == '\0') break;

    const char* start = p;
    while (*p != '\0' && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') p++;
    size_t len = p - start;
    if (len > 0 && len < 128) {
      char token[128];
      strncpy(token, start, len);
      token[len] = '\0';
      parse_single_flag(token);
    }
  }
}

void RedjetFlags::print_on(outputStream* st) {
  st->print_cr("==========================================================================");
  st->print_cr("                     RedjetGC Active Configuration                        ");
  st->print_cr("==========================================================================");
  st->print_cr("  GC Mode:            %s (%s)", _config.strings.gc_mode_raw, _config.strings.gc_operation_facility_raw);
  st->print_cr("  Snatch & Rebate:    %s", _config.strings.snatch_rebate_raw);
  st->print_cr("  RMRP Reclamation:   %s (Method: %s)", _config.strings.reduce_memory_resident_prop ? "Enabled" : "Disabled", _config.strings.rmrp_method_raw);
  st->print_cr("  Execution Focus:    %s", _config.strings.execution_focus_raw);
  st->print_cr("  Global Volatility:  %s", _config.strings.global_volatility_raw);
  st->print_cr("  Puzzler Morph:      %s", _config.strings.puzzler_morph_raw);
  st->print_cr("  4-Point Engine:     %s (Spin: %u, Pacing: %s)",
               _config.enable_4points ? "Enabled" : "Disabled",
               _config.integers.pausepoint_spin_count,
               _config.controlpoint_pacing ? "Enabled" : "Disabled");
  st->print_cr("  Landfill Critical:  %.2f ratio", _config.floats.landfill_critical_ratio);
  st->print_cr("  Falline Compact:    %.2f ratio", _config.floats.falline_compact_ratio);
  st->print_cr("  Heat Gain / Decay:  %.2f / %.2f", _config.floats.heat_gain_weight, _config.floats.heat_decay_factor);
  st->print_cr("  InlineASM Thresh:   %u", _config.integers.inline_asm_threshold);
  st->print_cr("  Entangle Exec:      %s", _config.strings.entangle_execution ? "Enabled" : "Disabled");
  st->print_cr("  Strict VFS Check:   %s", _config.strings.strict_vfs_branch_validation ? "True" : "False");
  st->print_cr("  Hardware Turbo:     %s", _config.strings.upclocker_enabled ? "Engaged" : "Default");
  st->print_cr("==========================================================================");
}
