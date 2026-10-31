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

#include "gc/redlet/redletIntentCLI.hpp"
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

std::atomic<bool>    g_sf_rmrp_enabled{ false };
std::atomic<uint8_t> g_sf_rmrp_method{ static_cast<uint8_t>(sf_rmrp_method_e::PLUMP) };
std::atomic<uint8_t> g_sf_global_volatility{ static_cast<uint8_t>(sf_global_volatility_e::DRAM) };
std::atomic<uint8_t> g_sf_puzzler_morph_method{ static_cast<uint8_t>(sf_puzzler_morph_e::SHADOW_RCU) };
std::atomic<bool>    g_sf_entangle_execution_flag{ false };
std::atomic<bool>    g_sf_strict_vfs_validation{ true };
std::atomic<bool>    g_sf_vfs_panic_dialog_enabled{ true };
std::atomic<float>   g_sf_heat_gain_weight{ 1.0f };
std::atomic<float>   g_sf_heat_decay_factor{ 0.97f };

namespace {
static std::atomic<uint32_t> g_sf_iasm_threshold{ 5 };
static std::atomic<bool>     g_sf_strict_ring3{ false };
static std::atomic<bool>     g_sf_strict_flags{ false };
static std::atomic<bool>     g_sf_strict_flag_suppress_abort{ false };
} // namespace

/* ========================================================================= */
/* 1. CANONICAL FLAG LIST & 13-CATEGORY SEMANTIC INTENT REGISTRY             */
/* ========================================================================= */

static const char* const SF_KNOWN_FLAGS[] = {
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
    "No_sfGC", "NO_SFGC", "SCGC_Type", "SCGC_TYPE", "Profile", "PROFILE",
    "Forbid_Dynamic_Growth", "FORBID_DYNAMIC_GROWTH", "Forbid_Runtime_Heap", "ForbidRuntimeHeap",
    "Max_Bounded_Iterations", "MAX_BOUNDED_ITERATIONS", "Max_Iterations", "MaxIterations",
    "Allow_Operation_Dispatchings", "AllowOperationDispatchings", "ALLOW_OPERATION_DISPATCHINGS", "Operation_Dispatchings",
    "Reduce_Memory_Resident_Propagation", "ReduceMemoryResidentPropagation", "REDUCE_MEMORY_RESIDENT_PROPAGATION", "RMRP",
    "RMRP_Method", "RMRPMethod", "RMRP_METHOD", "Global_Volatility_Method", "GlobalVolatilityMethod", "GLOBAL_VOLATILITY_METHOD",
    "puzzler_Morph_Method", "puzzlerMorphMethod", "puzzler_MORPH_METHOD", "Morph_Method",
    "Experimental", "EXPERIMENTAL", "Batch_Register_and_Entangle_Execution", "Batch_Register_And_Entangle_Execution",
    "BATCH_REGISTER_AND_ENTANGLE_EXECUTION", "Batch_Register_Entangle", "Entangle_Execution", "ENTANGLE_EXECUTION",
    "Runway_Handsel_Attempt_Value", "Landfill_Handsel_Attempt_Value",
    "Wakes_GC_Cycling_Interval", "Line_Compaction_Freq", "LineCompactionFreq", "LINE_COMPACTION_FREQ",
    "In_Heap_Forwarding_Rate", "InHeapForwardingRate", "IN_HEAP_FORWARDING_RATE",
    "Pressure_Threshold", "PressureThreshold", "PRESSURE_THRESHOLD",
    "Slab_Chunk_Size", "SlabChunkSize", "SLAB_CHUNK_SIZE",
    "Prune", "PRUNE", "Plump", "PLUMP", "Sequential", "SEQUENTIAL", "Installment", "INSTALLMENT",
    "Pressure_Mild_Ratio", "PressureMildRatio", "PRESSURE_MILD_RATIO",
    "Pressure_High_Ratio", "PressureHighRatio", "PRESSURE_HIGH_RATIO",
    "Pressure_Critical_Ratio", "PressureCriticalRatio", "PRESSURE_CRITICAL_RATIO",
    "Landfill_Critical_Ratio", "LandfillCriticalRatio", "LANDFILL_CRITICAL_RATIO",
    "Falline_Compact_Ratio", "FallineCompactRatio", "FALLINE_COMPACT_RATIO",
    "Execution_Focus", "ExecutionFocus", "Focus", "EXECUTION_FOCUS",
    "Strict_VFS_Branch_Validation", "StrictVFSBranchValidation", "STRICT_VFS_BRANCH_VALIDATION",
    "Strict_VFS", "StrictVFS", "STRICT_VFS", "VFS_Strict", "VFSStrict", "VFS_STRICT",
    "Strict_VFS_Validation", "STRICT_VFS_VALIDATION",
    "VFS_Panic_Dialog", "VFSPanicDialog", "VFS_PANIC_DIALOG", "Panic_Dialog", "PANIC_DIALOG",
    "InlineASM_Threshold", "IASM_Threshold", "InlineAsmThreshold", "INLINEASM_THRESHOLD", "IASM_THRESHOLD",
    "Strict_Ring3_Only", "StrictRing3Only", "Strict_Ring3", "StrictRing3", "STRICT_RING3_ONLY",
    "Strict_Flags", "StrictFlags", "Strict_Flag_Validation", "StrictFlagValidation", "STRICT_FLAGS",
    "UseMLTCSolver", "Use_MLTC_Solver", "MLTCSolver", "MLTC_Solver", "USE_MLTC_SOLVER",
    "Code_Arena_Bytes", "CodeArenaBytes", "Code_Arena_Size", "Code_Heap_Size", "CODE_ARENA_BYTES",
    "Heat_Decay_Factor", "HeatDecayFactor", "HEAT_DECAY_FACTOR",
    "Heat_Gain_Weight", "HeatGainWeight", "HEAT_GAIN_WEIGHT"
};

static const sf_cli_intent_entry_t SF_CLI_INTENT_REGISTRY[] = {
    // 1. Clock Frequency & Intrinsic Hardware Boost (Upclocker)
    {"boost",            "Upclocker", "1",     "Overclock / maximum frequency boost"},
    {"turbo",            "Upclocker", "1",     "Turbo execution mode"},
    {"speed",            "Upclocker", "1",     "Maximum execution performance clocking"},
    {"overclock",        "Upclocker", "1",     "Host CPU frequency scaling override"},
    {"upclock",          "Upclocker", "1",     "Host CPU upclocking"},
    {"accelerate",       "Upclocker", "1",     "Intrinsic silicon execution acceleration"},
    {"high_perf",        "Upclocker", "1",     "High-performance execution state"},

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

    // 3. Vector Acceleration & SIMD GC (GC_Mode)
    {"vector",           "GC_Mode", "SIMD",    "Vector-accelerated garbage collector"},
    {"vectorize",        "GC_Mode", "SIMD",    "Vectorized SIMD memory sweeps"},
    {"simd_gc",          "GC_Mode", "SIMD",    "SIMD-native allocation and sweep"},
    {"vector_gc",        "GC_Mode", "SIMD",    "Vector lane GC processing"},
    {"avx_gc",           "GC_Mode", "SIMD",    "AVX/AVX-512 accelerated GC"},
    {"scalar_gc",        "GC_Mode", "Cycles",  "Cycle-based scalar GC evaluation"},
    {"static_gc",        "GC_Mode", "Static",  "Fixed deterministic static GC"},

    // 4. Parallel Dispatching & ALU Lane Stealing (Allow_Operation_Dispatchings)
    {"parallel",         "Allow_Operation_Dispatchings", "true", "Enable parallel ALU operation dispatching"},
    {"steal",            "Allow_Operation_Dispatchings", "true", "Enable work-stealing across ALU execution lanes"},
    {"dispatch",         "Allow_Operation_Dispatchings", "true", "Enable autonomous ALU load dispatching"},
    {"dispatching",      "Allow_Operation_Dispatchings", "true", "Dynamic multi-lane operation dispatch"},
    {"work_stealing",    "Allow_Operation_Dispatchings", "true", "Saturated ALU lane work stealing"},
    {"concurrent",       "Allow_Operation_Dispatchings", "true", "Concurrent pipeline queueing"},

    // 5. Safety-Critical Profile & Real-Time Isolation (Profile)
    {"safety",           "Profile", "SC",      "Safety-critical deterministic profile"},
    {"critical",         "Profile", "SC",      "Mission-critical hard real-time operation"},
    {"isolated",         "Profile", "SC",      "Isolated deterministic static boundaries"},
    {"realtime",         "Profile", "SC",      "Hard real-time bounded latency mode"},
    {"deterministic",    "Profile", "SC",      "Disable dynamic deopt and JIT jitter"},
    {"secure",           "Profile", "SC",      "Zero-growth secured execution envelope"},

    // 6. SRAM Cacheline Volatility & Scratchpad (Global_Volatility_Method)
    {"sram",             "Global_Volatility_Method", "SRAM", "Direct on-chip SRAM cacheline volatility"},
    {"cache",            "Global_Volatility_Method", "SRAM", "Hierarchical cache line persistence"},
    {"fast_ram",         "Global_Volatility_Method", "SRAM", "Low-latency on-chip scratchpad volatility"},
    {"dram",             "Global_Volatility_Method", "DRAM", "Standard DRAM memory consistency model"},

    // 7. Compaction, Defrag & Kernel Fusion
    {"compact",          "Always_Focus_Compactizing", "true", "Continuous background heap defragmentation"},
    {"compactize",       "Always_Focus_Compactizing", "true", "Active memory cell coalescing"},
    {"defrag",           "Always_Focus_Compactizing", "true", "Active memory cell coalescing and heap defragmentation"},
    {"defragment",       "Always_Focus_Compactizing", "true", "Heap compaction and defragmentation"},
    {"entangle",         "Entangle_Execution", "true", "Batch kernel fusion transactional scratchpad"},
    {"fusion",           "Entangle_Execution", "true", "Transactional kernel instruction fusion"},
    {"fuse_ops",         "Entangle_Execution", "true", "Transactional kernel instruction fusion"},
    {"fuse",             "Entangle_Execution", "true", "Kernel instruction fusion"},
    {"hugepages",        "Enable_Huge_Pages", "true", "2MB/1GB hardware huge pages"},
    {"huge_pages",       "Enable_Huge_Pages", "true", "Direct OS huge page allocation"},
    {"experimental",     "Experimental", "true", "Unlock experimental VM subsystems"},
    {"simdly",           "GC_Mode", "SIMD", "Vector-accelerated garbage collector"},
    {"sram_cache",       "Global_Volatility_Method", "SRAM", "Direct on-chip SRAM cacheline volatility"},
    {"scratchpad",       "Global_Volatility_Method", "SRAM", "Low-latency on-chip scratchpad volatility"},
    {"low_latency",      "Profile", "SC", "Hard real-time bounded latency mode"},
    {"zero_jitter",      "Profile", "SC", "Safety-critical deterministic profile"},

    // 8. New Era Execution Focus Topology (SCP, MCP, Dual Hybrid/Homo)
    {"scp",              "Execution_Focus", "SCP",        "Single-core performance focus on highest turbo core"},
    {"single_core",      "Execution_Focus", "SCP",        "Single-core maximum turbo IPC focus"},
    {"mcp",              "Execution_Focus", "MCP",        "Multi-core wide parallel throughput focus"},
    {"multi_core",       "Execution_Focus", "MCP",        "Multi-core wide parallel throughput focus"},
    {"hybrid_dual",      "Execution_Focus", "HybridDual", "Asymmetric P+E dual single-and-multi performance focus"},
    {"hybrid-dual",      "Execution_Focus", "HybridDual", "Asymmetric P+E dual single-and-multi performance focus"},
    {"dual",             "Execution_Focus", "HybridDual", "Dual single and multi core performance focus"},
    {"dual_performance", "Execution_Focus", "HybridDual", "Dual single and multi core performance focus"},
    {"scp_homo",         "Execution_Focus", "SCP_Homo",   "Single-core homogeneous peak turbo focus"},
    {"scp-homo",         "Execution_Focus", "SCP_Homo",   "Single-core homogeneous peak turbo focus"},
    {"scp_hybrid",       "Execution_Focus", "SCP_Hybrid", "Single-core asymmetric strict P-core pinned focus"},
    {"scp-hybrid",       "Execution_Focus", "SCP_Hybrid", "Single-core asymmetric strict P-core pinned focus"},
    {"mcp_homo",         "Execution_Focus", "MCP_Homo",   "Multi-core homogeneous uniform chunking focus"},
    {"mcp-homo",         "Execution_Focus", "MCP_Homo",   "Multi-core homogeneous uniform chunking focus"},
    {"mcp_hybrid",       "Execution_Focus", "MCP_Hybrid", "Multi-core asymmetric weighted chunking focus"},
    {"mcp-hybrid",       "Execution_Focus", "MCP_Hybrid", "Multi-core asymmetric weighted chunking focus"},
    {"dual_homo",        "Execution_Focus", "Dual_Homo",  "Dual focus homogeneous (1 turbo + N-1 workers)"},
    {"dual-homo",        "Execution_Focus", "Dual_Homo",  "Dual focus homogeneous (1 turbo + N-1 workers)"},
    {"dual_hybrid",      "Execution_Focus", "Dual_Hybrid","Dual focus asymmetric (P compute + E gc/logs)"},
    {"dual-hybrid",      "Execution_Focus", "Dual_Hybrid","Dual focus asymmetric (P compute + E gc/logs)"},

    // 9. puzzler JIT/ISA Morph Paradigms
    {"morph_shadow",     "puzzler_Morph_Method", "Shadow",   "Zero-pause shadow clone morphing (RCU)"},
    {"morph-shadow",     "puzzler_Morph_Method", "Shadow",   "Zero-pause shadow clone morphing (RCU)"},
    {"shadow_morph",     "puzzler_Morph_Method", "Shadow",   "Zero-pause shadow clone morphing (RCU)"},
    {"shadow-morph",     "puzzler_Morph_Method", "Shadow",   "Zero-pause shadow clone morphing (RCU)"},
    {"morph_excision",   "puzzler_Morph_Method", "Excision", "Exclusive excision with busy-wait notifications"},
    {"morph-excision",   "puzzler_Morph_Method", "Excision", "Exclusive excision with busy-wait notifications"},
    {"exclusive_morph",  "puzzler_Morph_Method", "Excision", "Exclusive excision with busy-wait notifications"},
    {"exclusive-morph",  "puzzler_Morph_Method", "Excision", "Exclusive excision with busy-wait notifications"},

    // 10. VFS Branch Safety & Fail-Fast Protection
    {"strict_vfs",       "Strict_VFS_Branch_Validation", "true", "Enforce strict VFS branch validation and panic on missing branches"},
    {"vfs_strict",       "Strict_VFS_Branch_Validation", "true", "Enforce strict VFS branch validation and panic on missing branches"},
    {"vfs_panic",        "Strict_VFS_Branch_Validation", "true", "Immediate safe panic when VFS branch is not found"},
    {"strict_branch",    "Strict_VFS_Branch_Validation", "true", "Enforce strict VFS directory branch validation"},
    {"vfs_safety",       "Strict_VFS_Branch_Validation", "true", "Fail-fast protection against missing VFS branches"},
    {"vfs_dialog",       "VFS_Panic_Dialog",             "true", "Enable modal alert dialog popup on fatal VFS panics"},
    {"vfs_popup",        "VFS_Panic_Dialog",             "true", "Enable modal alert dialog popup on fatal VFS panics"},

    // 11. Inline Assembly (iasm) Threshold & Strict Ring 3 Mode
    {"iasm_fast",        "InlineASM_Threshold",          "5",    "Engage hardware inline assembly and non-temporal streaming"},
    {"iasm_safe",        "InlineASM_Threshold",          "10",   "High safety threshold for inline assembly"},
    {"strict_ring3",     "Strict_Ring3_Only",            "true", "Forbid inline assembly and lock execution to safe Ring 3 APIs"},
    {"ring3_only",       "Strict_Ring3_Only",            "true", "Forbid inline assembly and lock execution to safe Ring 3 APIs"},
    {"strict_flags",     "Strict_Flags",                 "true", "Fail-fast on unknown flags, unreferenced params, or conflicts"},

    // 12. MLTC Diagnostic & Emergency Solver
    {"mltc_solver",      "UseMLTCSolver",                "true", "Engage MLTC continuous upward trend and congestion solver"},
    {"mltc",             "UseMLTCSolver",                "true", "Engage MLTC continuous upward trend and congestion solver"},

    // 13. Batch Register and Entangle Execution (Kernel Fusion)
    {"batch_register_and_entangle_execution", "Batch_Register_and_Entangle_Execution", "true", "Enable kernel fusion and multi-register entangled execution"},
    {"batch_register_entangle",               "Batch_Register_and_Entangle_Execution", "true", "Enable kernel fusion and multi-register entangled execution"},
    {"entangle_execution",                    "Batch_Register_and_Entangle_Execution", "true", "Enable kernel fusion and multi-register entangled execution"}
};

/* ========================================================================= */
/* 2. UNIT PARSERS & FUZZY STRING DISTANCE METRICS                           */
/* ========================================================================= */

uint64_t parse_size_with_unit(const char* str) {
    if (!str || !*str) return 0;
    char* end = nullptr;
    double val = std::strtod(str, &end);
    if (val < 0) val = 0;
    if (end) {
        while (*end == ' ') end++;
        if (_stricmp(end, "tb") == 0 || _stricmp(end, "t") == 0) {
            return static_cast<uint64_t>(val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
        }
        if (_stricmp(end, "gb") == 0 || _stricmp(end, "g") == 0) {
            return static_cast<uint64_t>(val * 1024.0 * 1024.0 * 1024.0);
        }
        if (_stricmp(end, "mb") == 0 || _stricmp(end, "m") == 0) {
            return static_cast<uint64_t>(val * 1024.0 * 1024.0);
        }
        if (_stricmp(end, "kb") == 0 || _stricmp(end, "k") == 0) {
            return static_cast<uint64_t>(val * 1024.0);
        }
        if (_stricmp(end, "b") == 0) {
            return static_cast<uint64_t>(val);
        }
    }
    return static_cast<uint64_t>(val);
}

uint64_t parse_time_with_unit_to_ns(const char* str) {
    if (!str || !*str) return 0;
    char* end = nullptr;
    double val = std::strtod(str, &end);
    if (val < 0) val = 0;
    if (end) {
        while (*end == ' ') end++;
        if (_stricmp(end, "m") == 0 || _stricmp(end, "min") == 0) {
            return static_cast<uint64_t>(val * 60.0 * 1000000000.0);
        }
        if (_stricmp(end, "s") == 0 || _stricmp(end, "sec") == 0) {
            return static_cast<uint64_t>(val * 1000000000.0);
        }
        if (_stricmp(end, "ms") == 0) {
            return static_cast<uint64_t>(val * 1000000.0);
        }
        if (_stricmp(end, "us") == 0) {
            return static_cast<uint64_t>(val * 1000.0);
        }
        if (_stricmp(end, "ns") == 0) {
            return static_cast<uint64_t>(val);
        }
    }
    return static_cast<uint64_t>(val * 1000000.0);
}

int sf_damerau_levenshtein_distance(const char* s1, const char* s2) {
    if (!s1 || !s2) return 99;
    int len1 = static_cast<int>(std::strlen(s1));
    int len2 = static_cast<int>(std::strlen(s2));
    if (len1 > 48 || len2 > 48) return 99;
    int d[50][50];
    for (int i = 0; i <= len1; ++i) d[i][0] = i;
    for (int j = 0; j <= len2; ++j) d[0][j] = j;
    for (int i = 1; i <= len1; ++i) {
        for (int j = 1; j <= len2; ++j) {
            int cost = (std::tolower(static_cast<unsigned char>(s1[i - 1])) ==
                        std::tolower(static_cast<unsigned char>(s2[j - 1]))) ? 0 : 1;
            int del = d[i - 1][j] + 1;
            int ins = d[i][j - 1] + 1;
            int sub = d[i - 1][j - 1] + cost;
            int m   = (del < ins) ? del : ins;
            d[i][j] = (m < sub) ? m : sub;
            if (i > 1 && j > 1 &&
                std::tolower(static_cast<unsigned char>(s1[i - 1])) == std::tolower(static_cast<unsigned char>(s2[j - 2])) &&
                std::tolower(static_cast<unsigned char>(s1[i - 2])) == std::tolower(static_cast<unsigned char>(s2[j - 1]))) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }
    return d[len1][len2];
}

double sf_jaro_winkler_similarity(const char* s1, const char* s2) {
    if (!s1 || !s2) return 0.0;
    int l1 = std::min(static_cast<int>(std::strlen(s1)), 64);
    int l2 = std::min(static_cast<int>(std::strlen(s2)), 64);
    if (l1 == 0 && l2 == 0) return 1.0;
    if (l1 == 0 || l2 == 0) return 0.0;

    int match_distance = std::max(l1, l2) / 2 - 1;
    if (match_distance < 0) match_distance = 0;

    bool s1_matches[64] = { false };
    bool s2_matches[64] = { false };
    int matches = 0;

    for (int i = 0; i < l1 && i < 64; ++i) {
        int start = std::max(0, i - match_distance);
        int end   = std::min(i + match_distance + 1, l2);
        for (int j = start; j < end && j < 64; ++j) {
            if (s2_matches[j]) continue;
            if (std::tolower(static_cast<unsigned char>(s1[i])) ==
                std::tolower(static_cast<unsigned char>(s2[j]))) {
                s1_matches[i] = true;
                s2_matches[j] = true;
                matches++;
                break;
            }
        }
    }
    if (matches == 0) return 0.0;

    double transpositions = 0.0;
    int k = 0;
    for (int i = 0; i < l1 && i < 64; ++i) {
        if (!s1_matches[i]) continue;
        while (k < l2 && k < 64 && !s2_matches[k]) k++;
        if (k < l2 && std::tolower(static_cast<unsigned char>(s1[i])) !=
                      std::tolower(static_cast<unsigned char>(s2[k]))) {
            transpositions += 0.5;
        }
        k++;
    }

    double jaro = ((static_cast<double>(matches) / l1) +
                   (static_cast<double>(matches) / l2) +
                   ((static_cast<double>(matches) - transpositions) / matches)) / 3.0;

    int prefix = 0;
    for (int i = 0; i < std::min(l1, std::min(l2, 4)); ++i) {
        if (std::tolower(static_cast<unsigned char>(s1[i])) ==
            std::tolower(static_cast<unsigned char>(s2[i]))) {
            prefix++;
        } else {
            break;
        }
    }
    return jaro + prefix * 0.1 * (1.0 - jaro);
}

/* ========================================================================= */
/* 3. PREFIX STRIPPER & SEMANTIC / FUZZY ARGUMENT RESOLVER                   */
/* ========================================================================= */

void sf_cli_strip_prefix_and_split(const char* raw_token, const char* raw_val,
                                   char* clean_key, size_t key_sz,
                                   char* clean_val, size_t val_sz) {
    if (!raw_token || !clean_key || key_sz == 0) return;
    clean_key[0] = '\0';
    if (clean_val && val_sz > 0) clean_val[0] = '\0';

    const char* p = raw_token;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    if (std::strncmp(p, "+#SF|", 5) == 0 || std::strncmp(p, "-#SF|", 5) == 0) {
        p += 5;
    } else if (_strnicmp(p, "--sf|", 5) == 0 || _strnicmp(p, "-sf|", 4) == 0 ||
               _strnicmp(p, "++sf|", 5) == 0 || _strnicmp(p, "+sf|", 4) == 0) {
        const char* bar = std::strchr(p, '|');
        p = bar ? (bar + 1) : p;
    } else if (_strnicmp(p, "--sf:", 5) == 0 || _strnicmp(p, "-sf:", 4) == 0 ||
               _strnicmp(p, "++sf:", 5) == 0 || _strnicmp(p, "+sf:", 4) == 0) {
        const char* col = std::strchr(p, ':');
        p = col ? (col + 1) : p;
    } else if (_strnicmp(p, "--sf-", 5) == 0 || _strnicmp(p, "-sf-", 4) == 0 ||
               _strnicmp(p, "++sf-", 5) == 0 || _strnicmp(p, "+sf-", 4) == 0) {
        const char* hyph = std::strchr(p, '-');
        if (hyph && std::tolower(static_cast<unsigned char>(hyph[1])) == 's' &&
            std::tolower(static_cast<unsigned char>(hyph[2])) == 'f' && hyph[3] == '-') {
            p = hyph + 4;
        } else {
            p += 5;
        }
    } else if ((p[0] == '-' && p[1] == '-') || (p[0] == '+' && p[1] == '+') || (p[0] == '/' && p[1] == '/')) {
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
        size_t klen = static_cast<size_t>(delim - p);
        if (klen >= key_sz) klen = key_sz - 1;
        std::strncpy(clean_key, p, klen);
        clean_key[klen] = '\0';

        const char* v = delim + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (clean_val && val_sz > 0) {
            std::strncpy(clean_val, v, val_sz - 1);
            clean_val[val_sz - 1] = '\0';
            size_t vlen = std::strlen(clean_val);
            while (vlen > 0 && (clean_val[vlen - 1] == ' ' || clean_val[vlen - 1] == '\t' ||
                                clean_val[vlen - 1] == '"' || clean_val[vlen - 1] == '\'')) {
                clean_val[--vlen] = '\0';
            }
            if (clean_val[0] == '"' || clean_val[0] == '\'') {
                std::memmove(clean_val, clean_val + 1, vlen);
            }
        }
    } else {
        std::strncpy(clean_key, p, key_sz - 1);
        clean_key[key_sz - 1] = '\0';
        if (raw_val && raw_val[0] && clean_val && val_sz > 0) {
            const char* rv = raw_val;
            while (*rv == ' ' || *rv == '\t') rv++;
            std::strncpy(clean_val, rv, val_sz - 1);
            clean_val[val_sz - 1] = '\0';
            size_t vlen = std::strlen(clean_val);
            while (vlen > 0 && (clean_val[vlen - 1] == ' ' || clean_val[vlen - 1] == '\t' ||
                                clean_val[vlen - 1] == '"' || clean_val[vlen - 1] == '\'')) {
                clean_val[--vlen] = '\0';
            }
            if (clean_val[0] == '"' || clean_val[0] == '\'') {
                std::memmove(clean_val, clean_val + 1, vlen);
            }
        }
    }

    size_t klen = std::strlen(clean_key);
    while (klen > 0 && (clean_key[klen - 1] == ' ' || clean_key[klen - 1] == '\t')) {
        clean_key[--klen] = '\0';
    }
}

sf_cli_resolution_result_t sf_cli_resolve_argument(const char* input_key, const char* input_val) {
    sf_cli_resolution_result_t res{};
    if (!input_key || !input_key[0]) return res;

    char clean_k[64] = { 0 };
    char clean_v[64] = { 0 };
    sf_cli_strip_prefix_and_split(input_key, input_val, clean_k, sizeof(clean_k), clean_v, sizeof(clean_v));
    if (clean_k[0] == '\0') return res;

    // 1. Direct semantic intent lookup
    for (const auto& entry : SF_CLI_INTENT_REGISTRY) {
        if (_stricmp(clean_k, entry.intent_keyword) == 0) {
            res.matched             = true;
            res.was_semantic_intent = true;
            std::strncpy(res.canonical_flag, entry.canonical_flag, sizeof(res.canonical_flag) - 1);
            std::strncpy(res.canonical_value, clean_v[0] ? clean_v : entry.default_value, sizeof(res.canonical_value) - 1);
            res.confidence    = 1.0;
            res.technical_doc = entry.technical_doc;
            return res;
        }
    }

    // 2. Direct exact match against known canonical flags
    for (const char* known : SF_KNOWN_FLAGS) {
        if (_stricmp(clean_k, known) == 0) {
            res.matched = true;
            std::strncpy(res.canonical_flag, known, sizeof(res.canonical_flag) - 1);
            std::strncpy(res.canonical_value, clean_v[0] ? clean_v : "1", sizeof(res.canonical_value) - 1);
            res.confidence    = 1.0;
            res.technical_doc = "Exact canonical flag match";
            return res;
        }
    }

    // 3. Fuzzy match against semantic intents
    double best_intent_score = 0.0;
    const sf_cli_intent_entry_t* best_intent = nullptr;
    for (const auto& entry : SF_CLI_INTENT_REGISTRY) {
        double sim = sf_jaro_winkler_similarity(clean_k, entry.intent_keyword);
        int dist   = sf_damerau_levenshtein_distance(clean_k, entry.intent_keyword);
        if ((sim >= 0.85 || dist <= 1) && sim > best_intent_score) {
            best_intent_score = sim;
            best_intent       = &entry;
        }
    }
    if (best_intent && best_intent_score >= 0.80) {
        res.matched             = true;
        res.was_semantic_intent = true;
        std::strncpy(res.canonical_flag, best_intent->canonical_flag, sizeof(res.canonical_flag) - 1);
        std::strncpy(res.canonical_value, clean_v[0] ? clean_v : best_intent->default_value, sizeof(res.canonical_value) - 1);
        res.confidence    = best_intent_score;
        res.technical_doc = best_intent->technical_doc;
        return res;
    }

    // 4. Fuzzy match against canonical known flags
    double best_flag_score = 0.0;
    const char* best_flag  = nullptr;
    for (const char* known : SF_KNOWN_FLAGS) {
        double sim = sf_jaro_winkler_similarity(clean_k, known);
        int dist   = sf_damerau_levenshtein_distance(clean_k, known);
        if ((sim >= 0.82 || dist <= 2) && sim > best_flag_score) {
            best_flag_score = sim;
            best_flag       = known;
        }
    }
    if (best_flag && best_flag_score >= 0.78) {
        res.matched        = true;
        res.was_fuzzy_typo = true;
        std::strncpy(res.canonical_flag, best_flag, sizeof(res.canonical_flag) - 1);
        std::strncpy(res.canonical_value, clean_v[0] ? clean_v : "1", sizeof(res.canonical_value) - 1);
        res.confidence    = best_flag_score;
        res.technical_doc = "Fuzzy typo matched to canonical flag";
        return res;
    }

    return res;
}

/* ========================================================================= */
/* 4. FLAG PARSING & CROSS-FLAG CONFLICT RECONCILIATION ENGINE               */
/* ========================================================================= */

namespace {
static inline bool sf_cli_parse_bool(const char* val) noexcept {
    if (!val || !val[0]) return true;
    return (_stricmp(val, "1") == 0 || _stricmp(val, "true") == 0 ||
            _stricmp(val, "yes") == 0 || _stricmp(val, "allow") == 0 ||
            _stricmp(val, "enable") == 0 || _stricmp(val, "enabled") == 0);
}

static bool parse_single_sf_flag_internal(sf_vm_config_t& cfg, const char* key, const char* val) {
    if (!key || !val) return false;

    if (_stricmp(key, "GC_Mode") == 0 || _stricmp(key, "GC_Mod") == 0 ||
        _stricmp(key, "GCMode") == 0 || _stricmp(key, "GCMod") == 0) {
        std::strncpy(cfg.strings.gc_mode_raw, val, sizeof(cfg.strings.gc_mode_raw) - 1);
        if (_stricmp(val, "Static") == 0 || _stricmp(val, "Statik") == 0) {
            cfg.strings.gc_mode = sf_gc_mode_e::STATIC_MODE;
        } else if (_stricmp(val, "SIMD") == 0 || _stricmp(val, "Vectoral") == 0 || _stricmp(val, "Vector") == 0) {
            cfg.strings.gc_mode               = sf_gc_mode_e::HEURISTIC_MODE;
            cfg.strings.gc_operation_facility = sf_gc_operation_facility_e::SIMD;
        } else if (_stricmp(val, "Cycles") == 0 || _stricmp(val, "Scalar") == 0) {
            cfg.strings.gc_mode               = sf_gc_mode_e::HEURISTIC_MODE;
            cfg.strings.gc_operation_facility = sf_gc_operation_facility_e::CYCLES;
        } else {
            cfg.strings.gc_mode = sf_gc_mode_e::HEURISTIC_MODE;
        }
    } else if (_stricmp(key, "GC_Operation_Facility") == 0) {
        std::strncpy(cfg.strings.gc_operation_facility_raw, val, sizeof(cfg.strings.gc_operation_facility_raw) - 1);
        if (_stricmp(val, "SIMD") == 0) {
            cfg.strings.gc_operation_facility = sf_gc_operation_facility_e::SIMD;
        } else if (_stricmp(val, "Static") == 0 || _stricmp(val, "Statik") == 0) {
            cfg.strings.gc_operation_facility = sf_gc_operation_facility_e::STATIC_FIXED;
        } else {
            cfg.strings.gc_operation_facility = sf_gc_operation_facility_e::CYCLES;
        }
    } else if (_stricmp(key, "Use_Critical_SIMDly_Operations") == 0 ||
               _stricmp(key, "Use_Critical_SIMD_Operations") == 0 ||
               _stricmp(key, "Critical_SIMDly_Operations") == 0 ||
               _stricmp(key, "Critical_SIMD_Operations") == 0) {
        cfg.strings.critical_simd_explicit = true;
        std::strncpy(cfg.strings.critical_simd_raw, val, sizeof(cfg.strings.critical_simd_raw) - 1);
        if (_stricmp(val, "Allow") == 0 || _stricmp(val, "Allowed") == 0 || _stricmp(val, "1") == 0 || _stricmp(val, "True") == 0) {
            cfg.strings.critical_simd_mode = sf_critical_simd_e::ALLOW;
        } else if (_stricmp(val, "Deny") == 0 || _stricmp(val, "Denied") == 0 || _stricmp(val, "0") == 0 || _stricmp(val, "False") == 0) {
            cfg.strings.critical_simd_mode = sf_critical_simd_e::DENY;
        } else {
            cfg.strings.critical_simd_mode = sf_critical_simd_e::AUTO;
        }
        sf_set_critical_simd_mode(cfg.strings.critical_simd_mode);
    } else if (_stricmp(key, "Snatch_and_Rebate_Mode") == 0 || _stricmp(key, "SnatchAndRebateMode") == 0) {
        std::strncpy(cfg.strings.snatch_rebate_raw, val, sizeof(cfg.strings.snatch_rebate_raw) - 1);
        if (_stricmp(val, "Rapid") == 0) {
            cfg.strings.snatch_rebate_mode = sf_snatch_rebate_mode_e::RAPID;
        } else if (_stricmp(val, "Lazy") == 0) {
            cfg.strings.snatch_rebate_mode = sf_snatch_rebate_mode_e::LAZY;
        } else if (_stricmp(val, "GYC") == 0 || std::strstr(val, "generous") != nullptr || std::strstr(val, "cautious") != nullptr) {
            cfg.strings.snatch_rebate_mode = sf_snatch_rebate_mode_e::GYC;
        } else if (_stricmp(val, "Decommit") == 0 || _stricmp(val, "OS_Return") == 0) {
            cfg.strings.snatch_rebate_mode = sf_snatch_rebate_mode_e::DECOMMIT;
        } else {
            cfg.strings.snatch_rebate_mode = sf_snatch_rebate_mode_e::AUTO;
        }
    } else if (_stricmp(key, "GC_Heuristic") == 0 || _stricmp(key, "GC_Heuristics") == 0 || _stricmp(key, "Heuristic") == 0) {
        std::strncpy(cfg.strings.gc_heuristic_raw, val, sizeof(cfg.strings.gc_heuristic_raw) - 1);
        if (_stricmp(val, "Aggressive") == 0 || _stricmp(val, "Agresif") == 0) {
            cfg.strings.gc_heuristic = sf_gc_heuristic_e::AGGRESSIVE;
        } else if (_stricmp(val, "Bureaucratic") == 0 || _stricmp(val, "Burokratik") == 0) {
            cfg.strings.gc_heuristic = sf_gc_heuristic_e::BUREAUCRATIC;
        } else if (_stricmp(val, "Disabled") == 0 || _stricmp(val, "None") == 0 || _stricmp(val, "Zero_GC") == 0 ||
                   _stricmp(val, "No_sfGC") == 0 || _stricmp(val, "No_GC") == 0 || _stricmp(val, "Overwrite") == 0) {
            cfg.strings.gc_heuristic           = sf_gc_heuristic_e::DISABLED;
            cfg.strings.no_sfgc_overwrite_mode = true;
        } else {
            cfg.strings.gc_heuristic = sf_gc_heuristic_e::ADAPTIVE;
        }
    } else if (_stricmp(key, "FreeList_Recycle_Policy") == 0 || _stricmp(key, "FreeListRecyclePolicy") == 0 ||
               _stricmp(key, "Freelist_Policy") == 0 || _stricmp(key, "FreelistRecyclePolicy") == 0) {
        std::strncpy(cfg.strings.freelist_policy_raw, val, sizeof(cfg.strings.freelist_policy_raw) - 1);
        if (_stricmp(val, "FIFO") == 0) {
            cfg.integers.freelist_recycle_policy = 1;
        } else if (_stricmp(val, "LIFO") == 0) {
            cfg.integers.freelist_recycle_policy = 0;
        } else {
            cfg.integers.freelist_recycle_policy = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
        }
    } else if (_stricmp(key, "Enable_Huge_Pages") == 0 || _stricmp(key, "EnableHugePages") == 0 ||
               _stricmp(key, "Huge_Pages") == 0 || _stricmp(key, "HugePages") == 0) {
        cfg.strings.enable_huge_pages = sf_cli_parse_bool(val);
        g_sf_enable_huge_pages.store(cfg.strings.enable_huge_pages, std::memory_order_relaxed);
    } else if (_stricmp(key, "Advance_Payment") == 0 || _stricmp(key, "AdvancePayment") == 0 ||
               _stricmp(key, "AlwaysPreTouch") == 0 || _stricmp(key, "Always_PreTouch") == 0 ||
               _stricmp(key, "PreTouch") == 0 || _stricmp(key, "Pretouch") == 0) {
        cfg.strings.Advance_Payment = sf_cli_parse_bool(val);
    } else if (_stricmp(key, "Upclocker") == 0 || _stricmp(key, "Upclock") == 0 ||
               _stricmp(key, "Boost") == 0 || _stricmp(key, "Turbo") == 0) {
        bool en = (!val[0] || _stricmp(val, "Max") == 0 || _stricmp(val, "Boost") == 0 || sf_cli_parse_bool(val));
        cfg.strings.upclocker_enabled = en;
        sf_upclocker_set_boost(en ? 1 : 0);
    } else if (_stricmp(key, "Always_Focus_Compactizing") == 0 || _stricmp(key, "AlwaysFocusCompactizing") == 0 ||
               _stricmp(key, "Always_Compactize") == 0 || _stricmp(key, "Always_Compact") == 0) {
        cfg.strings.always_focus_compactizing = sf_cli_parse_bool(val);
    } else if (_stricmp(key, "No_sfGC") == 0 || _stricmp(key, "No_GC") == 0 ||
               _stricmp(key, "Zero_GC") == 0 || _stricmp(key, "Overwrite_Heap") == 0 ||
               _stricmp(key, "OverwriteHeap") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.no_sfgc_overwrite_mode = en;
        if (en) {
            cfg.strings.gc_heuristic = sf_gc_heuristic_e::DISABLED;
        }
    } else if (_stricmp(key, "SCGC_Type") == 0 || _stricmp(key, "SC_GC_Type") == 0 ||
               _stricmp(key, "NoGC_Type") == 0 || _stricmp(key, "PrimitiveRW") == 0) {
        if (_stricmp(val, "PrimitiveRW") == 0 || _stricmp(val, "Primitive_RW") == 0 ||
            _stricmp(val, "Primitive") == 0 || _stricmp(val, "RawRW") == 0 ||
            _stricmp(val, "Raw") == 0 || _stricmp(val, "1") == 0) {
            cfg.strings.scgc_type              = sf_scgc_type_e::PRIMITIVE_RW;
            cfg.strings.no_sfgc_overwrite_mode = true;
            cfg.strings.gc_heuristic           = sf_gc_heuristic_e::DISABLED;
        } else {
            cfg.strings.scgc_type = sf_scgc_type_e::DEFAULT;
        }
    } else if (_stricmp(key, "Runway_Handsel_Attempt_Value") == 0) {
        cfg.integers.runway_handsel_bytes = static_cast<size_t>(parse_size_with_unit(val));
    } else if (_stricmp(key, "Landfill_Handsel_Attempt_Value") == 0) {
        cfg.integers.landfill_handsel_bytes = static_cast<size_t>(parse_size_with_unit(val));
    } else if (_stricmp(key, "Wakes_GC_Cycling_Interval") == 0) {
        cfg.integers.wakes_gc_interval_ns       = parse_time_with_unit_to_ns(val);
        cfg.integers.wakes_gc_interval_explicit = true;
    } else if (_stricmp(key, "Line_Compaction_Freq") == 0 || _stricmp(key, "LineCompactionFreq") == 0) {
        cfg.integers.line_compaction_freq_ns = parse_time_with_unit_to_ns(val);
    } else if (_stricmp(key, "In_Heap_Forwarding_Rate") == 0 || _stricmp(key, "InHeapForwardingRate") == 0) {
        cfg.integers.in_heap_forwarding_rate = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
    } else if (_stricmp(key, "Pressure_Threshold") == 0 || _stricmp(key, "PressureThreshold") == 0) {
        cfg.integers.pressure_threshold_bytes = static_cast<size_t>(parse_size_with_unit(val));
    } else if (_stricmp(key, "Slab_Chunk_Size") == 0 || _stricmp(key, "SlabChunkSize") == 0) {
        cfg.integers.slab_chunk_size_bytes = static_cast<size_t>(parse_size_with_unit(val));
    } else if (_stricmp(key, "Pressure_Mild_Ratio") == 0 || _stricmp(key, "PressureMildRatio") == 0) {
        cfg.floats.pressure_mild_ratio = static_cast<float>(std::strtod(val, nullptr));
    } else if (_stricmp(key, "Pressure_High_Ratio") == 0 || _stricmp(key, "PressureHighRatio") == 0) {
        cfg.floats.pressure_high_ratio = static_cast<float>(std::strtod(val, nullptr));
    } else if (_stricmp(key, "Pressure_Critical_Ratio") == 0 || _stricmp(key, "PressureCriticalRatio") == 0) {
        cfg.floats.pressure_critical_ratio = static_cast<float>(std::strtod(val, nullptr));
    } else if (_stricmp(key, "Landfill_Critical_Ratio") == 0 || _stricmp(key, "LandfillCriticalRatio") == 0) {
        cfg.floats.landfill_critical_ratio = static_cast<float>(std::strtod(val, nullptr));
    } else if (_stricmp(key, "Falline_Compact_Ratio") == 0 || _stricmp(key, "FallineCompactRatio") == 0) {
        cfg.floats.falline_compact_ratio = static_cast<float>(std::strtod(val, nullptr));
    } else if (_stricmp(key, "Heat_Decay_Factor") == 0 || _stricmp(key, "HeatDecayFactor") == 0) {
        cfg.floats.heat_decay_factor = static_cast<float>(std::strtod(val, nullptr));
        g_sf_heat_decay_factor.store(cfg.floats.heat_decay_factor, std::memory_order_relaxed);
    } else if (_stricmp(key, "Heat_Gain_Weight") == 0 || _stricmp(key, "HeatGainWeight") == 0) {
        cfg.floats.heat_gain_weight = static_cast<float>(std::strtod(val, nullptr));
        g_sf_heat_gain_weight.store(cfg.floats.heat_gain_weight, std::memory_order_relaxed);
    } else if (_stricmp(key, "Code_Arena_Bytes") == 0 || _stricmp(key, "CodeArenaBytes") == 0 ||
               _stricmp(key, "Code_Arena_Size") == 0 || _stricmp(key, "Code_Heap_Size") == 0) {
        cfg.integers.code_arena_bytes = static_cast<size_t>(parse_size_with_unit(val));
    } else if (_stricmp(key, "Profile") == 0 || _stricmp(key, "Mode") == 0 || _stricmp(key, "Profile_Mode") == 0) {
        if (_stricmp(val, "SC") == 0 || _stricmp(val, "SSIOLSC") == 0 ||
            _stricmp(val, "SafetyCritical") == 0 || _stricmp(val, "Safety_Critical") == 0 ||
            _stricmp(val, "1") == 0) {
            cfg.profile               = MSIMD_PROFILE_SAFETY_CRITICAL;
            cfg.is_safety_critical    = true;
            cfg.Forbid_Dynamic_Growth = true;
            cfg.disable_jit_deopt     = true;
        } else {
            cfg.profile            = MSIMD_PROFILE_STANDARD;
            cfg.is_safety_critical = false;
        }
    } else if (_stricmp(key, "Forbid_Dynamic_Growth") == 0 || _stricmp(key, "ForbidDynamicGrowth") == 0 ||
               _stricmp(key, "Forbid_Runtime_Heap") == 0 || _stricmp(key, "ForbidRuntimeHeap") == 0) {
        cfg.Forbid_Dynamic_Growth = sf_cli_parse_bool(val);
    } else if (_stricmp(key, "Max_Bounded_Iterations") == 0 || _stricmp(key, "MaxLoopIterations") == 0 ||
               _stricmp(key, "Max_Iterations") == 0 || _stricmp(key, "MaxIterations") == 0) {
        cfg.max_loop_iterations = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
    } else if (_stricmp(key, "Allow_Operation_Dispatchings") == 0 || _stricmp(key, "AllowOperationDispatchings") == 0 ||
               _stricmp(key, "Allow_Dispatching") == 0 || _stricmp(key, "Operation_Dispatchings") == 0) {
        bool allow = sf_cli_parse_bool(val);
        cfg.strings.allow_operation_dispatchings = allow;
        SF_custominiVm_set_allow_operation_dispatchings(allow ? 1u : 0u);
    } else if (_stricmp(key, "Reduce_Memory_Resident_Propagation") == 0 ||
               _stricmp(key, "ReduceMemoryResidentPropagation") == 0 || _stricmp(key, "RMRP") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.reduce_memory_resident_propagation = en;
        g_sf_rmrp_enabled.store(en, std::memory_order_relaxed);
    } else if (_stricmp(key, "RMRP_Method") == 0 || _stricmp(key, "RMRPMethod") == 0) {
        cfg.strings.rmrp_method_explicit = true;
        std::strncpy(cfg.strings.rmrp_method_raw, val, sizeof(cfg.strings.rmrp_method_raw) - 1);
        if (_stricmp(val, "Sequential") == 0 || _stricmp(val, "Installment") == 0 || _stricmp(val, "Stepwise") == 0) {
            cfg.strings.rmrp_method = sf_rmrp_method_e::SEQUENTIAL;
        } else {
            cfg.strings.rmrp_method = sf_rmrp_method_e::PLUMP;
        }
        g_sf_rmrp_method.store(static_cast<uint8_t>(cfg.strings.rmrp_method), std::memory_order_relaxed);
    } else if (_stricmp(key, "Sequential") == 0 || _stricmp(key, "Installment") == 0) {
        cfg.strings.rmrp_method_explicit = true;
        cfg.strings.rmrp_method          = sf_rmrp_method_e::SEQUENTIAL;
        std::strncpy(cfg.strings.rmrp_method_raw, "Sequential", sizeof(cfg.strings.rmrp_method_raw) - 1);
        g_sf_rmrp_method.store(static_cast<uint8_t>(sf_rmrp_method_e::SEQUENTIAL), std::memory_order_relaxed);
    } else if (_stricmp(key, "Prune") == 0 || _stricmp(key, "Plump") == 0) {
        cfg.strings.rmrp_method_explicit = true;
        cfg.strings.rmrp_method          = sf_rmrp_method_e::PLUMP;
        std::strncpy(cfg.strings.rmrp_method_raw, "Plump", sizeof(cfg.strings.rmrp_method_raw) - 1);
        g_sf_rmrp_method.store(static_cast<uint8_t>(sf_rmrp_method_e::PLUMP), std::memory_order_relaxed);
    } else if (_stricmp(key, "Global_Volatility_Method") == 0 || _stricmp(key, "GlobalVolatilityMethod") == 0 ||
               _stricmp(key, "Volatility_Method") == 0) {
        std::strncpy(cfg.strings.global_volatility_raw, val, sizeof(cfg.strings.global_volatility_raw) - 1);
        if (_stricmp(val, "SRAM") == 0 || _stricmp(val, "Cache") == 0 || _stricmp(val, "L1L2") == 0) {
            cfg.strings.global_volatility = sf_global_volatility_e::SRAM;
        } else {
            cfg.strings.global_volatility = sf_global_volatility_e::DRAM;
        }
        g_sf_global_volatility.store(static_cast<uint8_t>(cfg.strings.global_volatility), std::memory_order_relaxed);
    } else if (_stricmp(key, "puzzler_Morph_Method") == 0 || _stricmp(key, "puzzlerMorphMethod") == 0 ||
               _stricmp(key, "Morph_Method") == 0) {
        std::strncpy(cfg.strings.puzzler_morph_raw, val, sizeof(cfg.strings.puzzler_morph_raw) - 1);
        if (_stricmp(val, "Excision") == 0 || _stricmp(val, "Exclusive") == 0 || _stricmp(val, "Busy") == 0) {
            cfg.strings.puzzler_morph = sf_puzzler_morph_e::EXCISION_EXCLUSIVE;
        } else {
            cfg.strings.puzzler_morph = sf_puzzler_morph_e::SHADOW_RCU;
        }
        g_sf_puzzler_morph_method.store(static_cast<uint8_t>(cfg.strings.puzzler_morph), std::memory_order_relaxed);
    } else if (_stricmp(key, "Experimental") == 0 || _stricmp(key, "Experimental_Features") == 0 ||
               _stricmp(key, "UnlockExperimentalVMOptions") == 0) {
        cfg.strings.experimental_options  = sf_cli_parse_bool(val);
        cfg.UnlockExperimentalVMOptions   = cfg.strings.experimental_options;
    } else if (_stricmp(key, "UseMLTCSolver") == 0 || _stricmp(key, "Use_MLTC_Solver") == 0 ||
               _stricmp(key, "MLTCSolver") == 0 || _stricmp(key, "MLTC_Solver") == 0) {
        cfg.UseMLTCSolver = sf_cli_parse_bool(val);
    } else if (_stricmp(key, "Batch_Register_and_Entangle_Execution") == 0 ||
               _stricmp(key, "Batch_Register_And_Entangle_Execution") == 0 ||
               _stricmp(key, "Batch_Register_Entangle") == 0 ||
               _stricmp(key, "Entangle_Execution") == 0 ||
               _stricmp(key, "Entangled_Execution") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.entangle_execution = en;
        g_sf_entangle_execution_flag.store(en, std::memory_order_relaxed);
    } else if (_stricmp(key, "Execution_Focus") == 0 || _stricmp(key, "ExecutionFocus") == 0 ||
               _stricmp(key, "Focus") == 0) {
        if (_stricmp(val, "SCP") == 0 || _stricmp(val, "SingleCore") == 0 || _stricmp(val, "Single_Core") == 0) {
            cfg.execution_focus = SF_FOCUS_SCP;
        } else if (_stricmp(val, "MCP") == 0 || _stricmp(val, "MultiCore") == 0 || _stricmp(val, "Multi_Core") == 0) {
            cfg.execution_focus = SF_FOCUS_MCP;
        } else if (_stricmp(val, "HybridDual") == 0 || _stricmp(val, "Hybrid_Dual") == 0 ||
                   _stricmp(val, "Dual") == 0 || _stricmp(val, "DualPerformance") == 0) {
            cfg.execution_focus = SF_FOCUS_HYBRID_DUAL;
        } else if (_stricmp(val, "SCP_Homo") == 0 || _stricmp(val, "SCPHomo") == 0 || _stricmp(val, "SCP_Homogeneous") == 0) {
            cfg.execution_focus = SF_FOCUS_SCP_HOMO;
        } else if (_stricmp(val, "SCP_Hybrid") == 0 || _stricmp(val, "SCPHybrid") == 0 || _stricmp(val, "SCP_Asymmetric") == 0) {
            cfg.execution_focus = SF_FOCUS_SCP_HYBRID;
        } else if (_stricmp(val, "MCP_Homo") == 0 || _stricmp(val, "MCPHomo") == 0 || _stricmp(val, "MCP_Homogeneous") == 0) {
            cfg.execution_focus = SF_FOCUS_MCP_HOMO;
        } else if (_stricmp(val, "MCP_Hybrid") == 0 || _stricmp(val, "MCPHybrid") == 0 || _stricmp(val, "MCP_Asymmetric") == 0) {
            cfg.execution_focus = SF_FOCUS_MCP_HYBRID;
        } else if (_stricmp(val, "Dual_Homo") == 0 || _stricmp(val, "DualHomo") == 0 || _stricmp(val, "Dual_Homogeneous") == 0) {
            cfg.execution_focus = SF_FOCUS_DUAL_HOMO;
        } else if (_stricmp(val, "Dual_Hybrid") == 0 || _stricmp(val, "DualHybrid") == 0 || _stricmp(val, "Dual_Asymmetric") == 0) {
            cfg.execution_focus = SF_FOCUS_DUAL_HYBRID;
        } else {
            cfg.execution_focus = SF_FOCUS_AUTO;
        }
        SF_custominiVm_set_execution_focus(cfg.execution_focus);
    } else if (_stricmp(key, "Strict_VFS_Branch_Validation") == 0 || _stricmp(key, "StrictVFSBranchValidation") == 0 ||
               _stricmp(key, "Strict_VFS") == 0 || _stricmp(key, "StrictVFS") == 0 ||
               _stricmp(key, "VFS_Strict") == 0 || _stricmp(key, "VFSStrict") == 0 ||
               _stricmp(key, "Strict_VFS_Validation") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.strict_vfs_branch_validation = en;
        g_sf_strict_vfs_validation.store(en, std::memory_order_relaxed);
    } else if (_stricmp(key, "VFS_Panic_Dialog") == 0 || _stricmp(key, "VFSPanicDialog") == 0 ||
               _stricmp(key, "Panic_Dialog") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.vfs_panic_dialog = en;
        g_sf_vfs_panic_dialog_enabled.store(en, std::memory_order_relaxed);
    } else if (_stricmp(key, "InlineASM_Threshold") == 0 || _stricmp(key, "IASM_Threshold") == 0 ||
               _stricmp(key, "InlineAsmThreshold") == 0 || _stricmp(key, "IASMThreshold") == 0) {
        uint32_t val_num = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
        cfg.integers.inline_asm_threshold          = val_num;
        cfg.integers.inline_asm_threshold_explicit = true;
        SF_custominiVm_set_iasm_threshold(val_num);
    } else if (_stricmp(key, "Strict_Ring3_Only") == 0 || _stricmp(key, "StrictRing3Only") == 0 ||
               _stricmp(key, "Strict_Ring3") == 0 || _stricmp(key, "StrictRing3") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.strict_ring3_only = en;
        SF_custominiVm_set_strict_ring3(en ? 1 : 0);
    } else if (_stricmp(key, "Strict_Flags") == 0 || _stricmp(key, "StrictFlags") == 0 ||
               _stricmp(key, "Strict_Flag_Validation") == 0 || _stricmp(key, "StrictFlagValidation") == 0) {
        bool en = sf_cli_parse_bool(val);
        cfg.strings.strict_flags = en;
        SF_custominiVm_set_strict_flags(en ? 1 : 0);
    } else {
        return false;
    }
    return true;
}
} // namespace

void parse_single_sf_flag(sf_vm_config_t& cfg, const char* flag_str) {
    if (!flag_str || !flag_str[0]) return;

    char key[64] = { 0 };
    char val[64] = { 0 };
    sf_cli_strip_prefix_and_split(flag_str, nullptr, key, sizeof(key), val, sizeof(val));
    if (key[0] == '\0') return;

    if (!parse_single_sf_flag_internal(cfg, key, val)) {
        sf_cli_resolution_result_t res = sf_cli_resolve_argument(key, val);
        if (res.matched) {
            if (res.was_semantic_intent) {
                char msg_buf[256];
                std::snprintf(msg_buf, sizeof(msg_buf),
                    "[SFVM CLI Semantic Engine] Recognized intent '%s' -> applied canonical flag '+#SF|%s:%s' (confidence: %.2f)",
                    key, res.canonical_flag, res.canonical_value, res.confidence);
                sf_unified_log_emit(SF_Ebin0_SAFEPOINT, SF_Ebin1_INFO, 0, "cli_semantic_intent", msg_buf);
                parse_single_sf_flag_internal(cfg, res.canonical_flag, res.canonical_value);
            } else if (res.was_fuzzy_typo) {
                char msg_buf[256];
                std::snprintf(msg_buf, sizeof(msg_buf),
                    "[SFVM CLI Helper] Unknown argument '+#SF|%s' --> did you mean %s? (DID YOU MEAN: '+#SF|%s'?)",
                    key, res.canonical_flag, res.canonical_flag);
                sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_WARN, 0, "cli_fuzzy_typo", msg_buf);
                parse_single_sf_flag_internal(cfg, res.canonical_flag, res.canonical_value);
            } else {
                parse_single_sf_flag_internal(cfg, res.canonical_flag, res.canonical_value);
            }
        } else {
            const char* best_match = nullptr;
            int min_dist = 99;
            for (const char* known : SF_KNOWN_FLAGS) {
                int dist = sf_damerau_levenshtein_distance(key, known);
                if (dist < min_dist && dist <= 3) {
                    min_dist   = dist;
                    best_match = known;
                }
            }
            char msg_buf[256];
            if (best_match) {
                std::snprintf(msg_buf, sizeof(msg_buf),
                    "[SFVM CLI Helper] Unknown argument '+#SF|%s' --> did you mean %s? (DID YOU MEAN: '+#SF|%s'?)",
                    key, best_match, best_match);
            } else {
                std::snprintf(msg_buf, sizeof(msg_buf), "[SFVM CLI Helper] Unrecognized flag '+#SF|%s'", key);
            }
            sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_WARN, 0, "cli_unknown_flag", msg_buf);
            if (SF_custominiVm_is_strict_flags()) {
                cfg.has_flag_conflict = true;
                std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                    "[SFVM STRICT FLAG PANIC] Unrecognized CLI Flag: '+#SF|%s' (Strict_Flags:true)",
                    key);
                sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_FATAL, 0, "strict_flag_panic", cfg.conflict_msg);
                if (!g_sf_strict_flag_suppress_abort.load(std::memory_order_relaxed)) {
                    (void)SF_custominiVm_emergency_os_sweep();
                    std::abort();
                }
            }
        }
    }
}

bool validate_and_reconcile_sf_flags(sf_vm_config_t& cfg) {
    if (cfg.strings.gc_operation_facility == sf_gc_operation_facility_e::SIMD) {
        if (cfg.strings.critical_simd_explicit) {
            cfg.has_flag_conflict = true;
            if (cfg.strings.critical_simd_mode == sf_critical_simd_e::DENY) {
                std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                    "FATAL FLAG CONFLICT: +#SF|GC_Operation_Facility:SIMD is incompatible with +#SF|Use_Critical_SIMDly_Operations:Deny! "
                    "SIMD GC requires SIMD vector units. Initialization rejected.");
                sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "flag_conflict_simd", cfg.conflict_msg);
                return false;
            } else {
                std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                    "INCOMPATIBILITY WARNING: 'Use_Critical_SIMDly_Operations' is superseded because +#SF|GC_Operation_Facility:SIMD is already fully SIMD-native.");
                sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_WARN, 0, "flag_warn_simd", cfg.conflict_msg);
            }
        }
    }

    if (cfg.strings.rmrp_method_explicit && !cfg.strings.reduce_memory_resident_propagation) {
        cfg.has_flag_conflict = true;
        std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
            "FATAL FLAG CONFLICT: 'RMRP_Method' cannot be used without '+#SF|Reduce_Memory_Resident_Propagation:true'! Initialization rejected.");
        sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "flag_conflict_rmrp", cfg.conflict_msg);
        return false;
    }

    if (cfg.strings.no_sfgc_overwrite_mode) {
        if (cfg.strings.always_focus_compactizing || cfg.strings.gc_operation_facility == sf_gc_operation_facility_e::SIMD) {
            cfg.has_flag_conflict = true;
            std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                "FATAL FLAG CONFLICT: No_sfGC / PrimitiveRW disables sfGC collector! Incompatible with Always_Focus_Compactizing or GC_Mode:SIMD.");
            sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "flag_conflict_nogc", cfg.conflict_msg);
            return false;
        }
    }

    if (cfg.strings.allow_operation_dispatchings) {
        std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
            "SFVM Operational Notice: +#SF|Allow_Operation_Dispatchings:true is restricted for High Priority (8-10) operations to protect execution locality.");
        sf_unified_log_emit(SF_Ebin0_ALU_DISPATCH_DIRECT, SF_Ebin1_INFO, 0, "alu_dispatch_notice", cfg.conflict_msg);
    }

    if (cfg.strings.entangle_execution && !cfg.strings.experimental_options) {
        cfg.has_flag_conflict = true;
        std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
            "SFVM Flag Conflict Error: 'Entangle_Execution' requires '+#SF|Experimental:true' to be enabled!");
        sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "flag_conflict_entgl", cfg.conflict_msg);
        return false;
    }

    if (cfg.UseMLTCSolver && !cfg.UnlockExperimentalVMOptions) {
        cfg.has_flag_conflict = true;
        std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
            "SFVM Flag Conflict Error: 'UseMLTCSolver' requires '+#SF|Experimental:true' to be enabled!");
        sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "flag_conflict_mltc", cfg.conflict_msg);
        return false;
    }

    if (cfg.is_safety_critical || cfg.profile == MSIMD_PROFILE_SAFETY_CRITICAL) {
        cfg.profile                  = MSIMD_PROFILE_SAFETY_CRITICAL;
        cfg.is_safety_critical       = true;
        cfg.Forbid_Dynamic_Growth    = true;
        cfg.disable_jit_deopt        = true;
        cfg.strings.Advance_Payment  = true;
        cfg.strings.gc_mode          = sf_gc_mode_e::STATIC_MODE;
        std::strncpy(cfg.strings.gc_mode_raw, "Static", sizeof(cfg.strings.gc_mode_raw) - 1);
        if (!cfg.strings.no_sfgc_overwrite_mode && cfg.strings.scgc_type != sf_scgc_type_e::PRIMITIVE_RW) {
            cfg.strings.gc_heuristic = sf_gc_heuristic_e::BUREAUCRATIC;
            std::strncpy(cfg.strings.gc_heuristic_raw, "Bureaucratic", sizeof(cfg.strings.gc_heuristic_raw) - 1);
        }
        if (cfg.strings.allow_operation_dispatchings) {
            cfg.strings.allow_operation_dispatchings = false;
            SF_custominiVm_set_allow_operation_dispatchings(0u);
            std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                "[SFVM Safety-Critical Profile] Disabling Allow_Operation_Dispatchings to guarantee strict hard real-time determinism.");
            sf_unified_log_emit(SF_Ebin0_SAFEPOINT, SF_Ebin1_INFO, 0, "sc_reconcile_dispatch", cfg.conflict_msg);
        } else {
            SF_custominiVm_set_allow_operation_dispatchings(0u);
        }
    }

    if (cfg.integers.wakes_gc_interval_explicit) {
        bool is_static_gc = (cfg.strings.gc_mode == sf_gc_mode_e::STATIC_MODE) ||
                            (cfg.strings.gc_operation_facility == sf_gc_operation_facility_e::STATIC_FIXED) ||
                            cfg.is_safety_critical || (cfg.profile == MSIMD_PROFILE_SAFETY_CRITICAL);
        if (!is_static_gc) {
            cfg.has_flag_conflict = true;
            std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                "FATAL FLAG CONFLICT: 'Wakes_GC_Cycling_Interval' is exclusively valid in Static GC mode (+#SF|GC_Mode:Static or Profile:SC)!");
            sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "flag_conflict_wakes_interval", cfg.conflict_msg);
            return false;
        }
    }

    if (cfg.strings.strict_ring3_only) {
        if (cfg.integers.inline_asm_threshold_explicit && cfg.integers.inline_asm_threshold < 10) {
            cfg.has_flag_conflict = true;
            std::snprintf(cfg.conflict_msg, sizeof(cfg.conflict_msg),
                "[SFVM Security Warning] Strict_Ring3_Only:true active while InlineASM_Threshold (%u < 10) requested! Clamping threshold to 10.",
                cfg.integers.inline_asm_threshold);
            sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_WARN, 0, "flag_conflict_strict_ring3", cfg.conflict_msg);
        }
        cfg.integers.inline_asm_threshold = 10;
        SF_custominiVm_set_iasm_threshold(10);
    }

    return true;
}

sf_vm_config_t parse_all_sf_flags(const char* custom_args, bool read_env) {
    sf_vm_config_t cfg;
    SF_custominiVm_set_strict_flags(0);

    if (read_env) {
        const char* env_sc = std::getenv("MSIMD_PROFILE");
        if (!env_sc) env_sc = std::getenv("SFVM_SC");
        if (env_sc && (_stricmp(env_sc, "SC") == 0 || _stricmp(env_sc, "SAFETY_CRITICAL") == 0 || _stricmp(env_sc, "1") == 0)) {
            cfg.profile               = MSIMD_PROFILE_SAFETY_CRITICAL;
            cfg.is_safety_critical    = true;
            cfg.Forbid_Dynamic_Growth = true;
            cfg.disable_jit_deopt     = true;
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
                parse_single_sf_flag(cfg, token);
            }
        }
    };

    if (read_env) {
        const char* env_args = std::getenv("MATHVANTAGE_OPTS");
        if (!env_args) env_args = std::getenv("SF_VM_ARGS");
        if (env_args) {
            parse_flags_loop(env_args);
        }
    }

    if (custom_args) {
        parse_flags_loop(custom_args);
    }

    (void)validate_and_reconcile_sf_flags(cfg);
    return cfg;
}

/* ========================================================================= */
/* 5. C ABI EXPORTS                                                          */
/* ========================================================================= */

extern "C" {

MSIMD_API int SF_custominiVm_cli_resolve_intent(const char* input_token,
                                                char* out_canonical_flag, size_t flag_buf_sz,
                                                char* out_canonical_value, size_t val_buf_sz) {
    if (!input_token || !out_canonical_flag || flag_buf_sz == 0) return 0;
    sf_cli_resolution_result_t res = sf_cli_resolve_argument(input_token, nullptr);
    if (!res.matched) return 0;

    std::strncpy(out_canonical_flag, res.canonical_flag, flag_buf_sz - 1);
    out_canonical_flag[flag_buf_sz - 1] = '\0';
    if (out_canonical_value && val_buf_sz > 0) {
        std::strncpy(out_canonical_value, res.canonical_value, val_buf_sz - 1);
        out_canonical_value[val_buf_sz - 1] = '\0';
    }
    return 1;
}

MSIMD_API void SF_custominiVm_set_iasm_threshold(uint32_t threshold) {
    g_sf_iasm_threshold.store(threshold > 10 ? 10 : threshold, std::memory_order_release);
}

MSIMD_API uint32_t SF_custominiVm_get_iasm_threshold(void) {
    return g_sf_iasm_threshold.load(std::memory_order_acquire);
}

MSIMD_API int SF_custominiVm_is_iasm_active(uint8_t op_priority) {
    if (g_sf_strict_ring3.load(std::memory_order_acquire)) return 0;
    uint32_t thresh = g_sf_iasm_threshold.load(std::memory_order_acquire);
    if (thresh >= 10) return 0;
    uint8_t eff_prio = (op_priority == 0) ? 5 : op_priority;
    return (eff_prio >= thresh) ? 1 : 0;
}

MSIMD_API void SF_custominiVm_set_strict_ring3(int enable) {
    g_sf_strict_ring3.store(enable != 0, std::memory_order_release);
    if (enable != 0) {
        g_sf_iasm_threshold.store(10, std::memory_order_release);
    }
}

MSIMD_API int SF_custominiVm_is_strict_ring3(void) {
    return g_sf_strict_ring3.load(std::memory_order_acquire) ? 1 : 0;
}

MSIMD_API void SF_custominiVm_set_strict_flags(int enable) {
    g_sf_strict_flags.store(enable != 0, std::memory_order_release);
}

MSIMD_API int SF_custominiVm_is_strict_flags(void) {
    return g_sf_strict_flags.load(std::memory_order_acquire) ? 1 : 0;
}

MSIMD_API void SF_custominiVm_set_strict_flag_suppress_abort(int suppress) {
    g_sf_strict_flag_suppress_abort.store(suppress != 0, std::memory_order_release);
}

} // extern "C"
