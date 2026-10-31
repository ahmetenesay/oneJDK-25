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

#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletDispatchISA.hpp"

#if defined(_WIN32)
#undef min
#undef max
#endif

#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_trace(...) if (false) printf
#endif

/* ========================================================================= */
/* 1. TABLEGEN MICROARCHITECTURE RECORDS GROUND-TRUTH MATRIX                 */
/* ========================================================================= */

static const sf_tablegen_uarch_entry_t SF_TABLEGEN_UARCH_RECORDS[] = {
    // Intel Desktop & Mobile Hybrid Profiles (Arrow Lake, Lunar Lake, Raptor Lake, Alder Lake, Meteor Lake)
    {"Profile_ARL_P", "Intel Lion Cove / Skymont (Arrow Lake-S)", "Arrowlake", "INTEL", 6, 0xC5, 0xC6, true,  sf_uarch_ipc_tier_t::ULTRA_IPC, 2.60, 256},
    {"Profile_LNL_P", "Intel Lion Cove / Skymont (Lunarlake)", "Lunarlake", "INTEL", 6, 0xBD, 0xBD, true,  sf_uarch_ipc_tier_t::ULTRA_IPC, 2.60, 256},
    {"Profile_RPL",   "Intel Raptor Cove / Gracemont (Raptorlake)", "Raptorlake", "INTEL", 6, 0xB7, 0xBF, true, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.50, 256},
    {"Profile_ADL_P", "Intel Golden Cove / Gracemont (Alderlake)",  "Alderlake",  "INTEL", 6, 0x97, 0x9A, true, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.45, 256},
    {"Profile_MTL_P", "Intel Redwood Cove / Crestmont (Meteorlake)", "Meteorlake", "INTEL", 6, 0xAA, 0xAC, true, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.40, 256},

    // Intel Server Xeon Profiles (Sapphire Rapids, Emerald Rapids, Granite Rapids, Sierra Forest)
    {"Profile_SPR",   "Intel Golden Cove Xeon (Sapphire Rapids)", "SapphireRapids", "INTEL", 6, 0x8F, 0x8F, false, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.50, 512},
    {"Profile_EMR",   "Intel Raptor Cove Xeon (Emerald Rapids)",  "EmeraldRapids",  "INTEL", 6, 0xCF, 0xCF, false, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.50, 512},
    {"Profile_GNR",   "Intel Redwood Cove Xeon (Granite Rapids)", "GraniteRapids", "INTEL", 6, 0xAD, 0xAE, false, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.50, 512},
    {"Profile_SRF",   "Intel Sierra Forest E-Core Xeon", "SierraForest", "INTEL", 6, 0xAF, 0xDF, false, sf_uarch_ipc_tier_t::EFFICIENCY_IPC, 1.00, 256},
    {"Profile_TGL",   "Intel Willow Cove (Tigerlake)", "Tigerlake", "INTEL", 6, 0x8C, 0x8D, false, sf_uarch_ipc_tier_t::HIGH_IPC, 1.80, 512},
    {"Profile_ICL",   "Intel Sunny Cove (Icelake)", "Icelake", "INTEL", 6, 0x7E, 0x7E, false, sf_uarch_ipc_tier_t::HIGH_IPC, 1.70, 512},
    {"Profile_SKX",   "Intel Skylake/Cascadelake Server", "Cascadelake", "INTEL", 6, 0x55, 0x55, false, sf_uarch_ipc_tier_t::MODERATE_IPC, 1.25, 512},

    // AMD Homogeneous & Hybrid Profiles (Zen 5, Zen 4, Zen 3, Zen 2, EPYC Turin, Threadripper)
    {"Profile_ZEN5",     "AMD Zen 5 (Granite Ridge / Turin 9005)", "Zen5", "AMD", 0x1A, 0x00, 0x7F, false, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.65, 512},
    {"Profile_ZEN5c",    "AMD Zen 5c (Turin Dense)", "Zen5c", "AMD", 0x1A, 0x80, 0xFF, false, sf_uarch_ipc_tier_t::HIGH_IPC, 2.20, 512},
    {"Profile_ZEN4",     "AMD Zen 4 (Raphael / Genoa / Threadripper 7000)", "Zen4", "AMD", 0x19, 0x10, 0x7F, false, sf_uarch_ipc_tier_t::HIGH_IPC, 2.20, 512},
    {"Profile_ZEN4c",    "AMD Zen 4c (Bergamo Server)", "Zen4c", "AMD", 0x19, 0xA0, 0xAF, false, sf_uarch_ipc_tier_t::HIGH_IPC, 2.00, 512},
    {"Profile_ZEN3",     "AMD Zen 3 (Vermeer / Milan)", "Zen3", "AMD", 0x19, 0x00, 0x0F, false, sf_uarch_ipc_tier_t::HIGH_IPC, 1.90, 256},
    {"Profile_ZEN2",     "AMD Zen 2 (Matisse / Rome)", "Zen2", "AMD", 0x17, 0x30, 0x7F, false, sf_uarch_ipc_tier_t::MODERATE_IPC, 1.50, 256},

    // ARM & Server Microarchitectures (Ampere Altra Neoverse N1, AmpereOne, Nvidia Grace, Apple M-Series)
    {"Profile_AmpereAltra", "ARM Neoverse N1 (Ampere Altra / Altra Max 128C)", "AmpereAltra", "ARM", 0x41, 0xD0C, 0xD0C, false, sf_uarch_ipc_tier_t::MODERATE_IPC, 1.10, 128},
    {"Profile_AmpereOne",   "AmpereOne (Ampere A192-32X 192C)",                "AmpereOne",   "ARM", 0xC0, 0xAC3, 0xAC3, false, sf_uarch_ipc_tier_t::HIGH_IPC,     1.85, 128},
    {"Profile_NV2",         "ARM Neoverse V2 (Nvidia Grace / AWS Graviton4)",  "NeoverseV2",  "ARM", 0x41, 0xD4F, 0xD4F, false, sf_uarch_ipc_tier_t::HIGH_IPC,     2.10, 256},
    {"Profile_M4",          "Apple M4 (TSMC N3E 10-wide)", "AppleM4", "APPL", 0x61, 0x04, 0x04, true, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.75, 128},
    {"Profile_M3",          "Apple M3 (TSMC N3B 8-wide)", "AppleM3", "APPL", 0x61, 0x03, 0x03, true, sf_uarch_ipc_tier_t::ULTRA_IPC, 2.50, 128},
    {"Profile_M2",          "Apple M2 (TSMC N5P 8-wide)", "AppleM2", "APPL", 0x61, 0x02, 0x02, true, sf_uarch_ipc_tier_t::HIGH_IPC, 2.25, 128},
    {"Profile_M1",          "Apple M1 (TSMC N5 8-wide)", "AppleM1", "APPL", 0x61, 0x01, 0x01, true, sf_uarch_ipc_tier_t::HIGH_IPC, 2.10, 128},
    {"Profile_ORY",         "Qualcomm Oryon (Snapdragon X Elite)", "OryonXElite", "QCOM", 0x51, 0x01, 0x02, false, sf_uarch_ipc_tier_t::HIGH_IPC, 2.20, 128}
};

/* ========================================================================= */
/* 2. GLOBAL SINGLETONS & TIMING PRIMITIVES                                  */
/* ========================================================================= */

static sf_hardware_engine_nexus_t g_sf_hardware_nexus;
static thread_local sf_pmu_session_t tl_sf_pmu_session;

#ifndef SACCULUS_RDTSC_DEFINED
static inline uint64_t sacculus_rdtsc() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}
#endif

#ifndef SACCULUS_NOW_NS_DEFINED
static inline uint64_t sacculus_now_ns() noexcept {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

static inline void sf_get_cpuid(int info[4], int func, int subfunc = 0) {
#if defined(_MSC_VER) || defined(__clang__)
    __cpuidex(info, func, subfunc);
#elif defined(__GNUC__)
    __asm__ __volatile__(
        "cpuid"
        : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3])
        : "a"(func), "c"(subfunc));
#else
    info[0] = info[1] = info[2] = info[3] = 0;
#endif
}

/* ========================================================================= */
/* 3. SRAM CACHELINE & VOLATILITY MEMORY CONTROL                             */
/* ========================================================================= */

MSIMD_API void sf_sram_commit_to_dram(void* ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
    char* p = reinterpret_cast<char*>(ptr);
    for (size_t off = 0; off < bytes; off += 64) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_clflush(p + off);
#endif
    }
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    sf_hotlog_emit(SF_Ebin0_SRAM_COMMIT_DRAM, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0,
        (uint32_t)(bytes / 1024), "sram_committed_dram");
}

MSIMD_API void sf_sram_broadcast_volatility(void* ptr, size_t bytes) {
    (void)ptr;
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    sf_hotlog_emit(SF_Ebin0_STORE_FENCE_BROADCAST, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0,
        (uint32_t)(bytes / 1024), "store_fence_bcast");
}

MSIMD_API void sf_dram_stream_store(void* dst, const void* src, size_t bytes) {
    if (!dst || !src || bytes == 0) return;
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const char* s = reinterpret_cast<const char*>(src);
    char* d = reinterpret_cast<char*>(dst);
    while (i < bytes && (reinterpret_cast<uintptr_t>(d + i) & 15) != 0) {
        d[i] = s[i];
        i++;
    }
#if defined(__AVX2__)
    for (; i + 32 <= bytes && (reinterpret_cast<uintptr_t>(d + i) & 31) == 0; i += 32) {
        __m256i val = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + i), val);
    }
#endif
    for (; i + 16 <= bytes; i += 16) {
        __m128i val = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        _mm_stream_si128(reinterpret_cast<__m128i*>(d + i), val);
    }
    _mm_sfence();
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))
    const char* s = reinterpret_cast<const char*>(src);
    char* d = reinterpret_cast<char*>(dst);
    while (i < bytes && (reinterpret_cast<uintptr_t>(d + i) & 15) != 0) {
        d[i] = s[i];
        i++;
    }
    for (; i + 32 <= bytes; i += 32) {
        __asm__ __volatile__(
            "ldp q0, q1, [%1]\n\t"
            "stnp q0, q1, [%0]\n\t"
            :
            : "r"(d + i), "r"(s + i)
            : "v0", "v1", "memory"
        );
    }
    __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
    if (i < bytes) {
        memcpy(reinterpret_cast<char*>(dst) + i, reinterpret_cast<const char*>(src) + i, bytes - i);
    }
    sf_hotlog_emit(SF_Ebin0_STREAMING_STORE_ENGAGE, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0,
        (uint32_t)(bytes / 1024), "nt_stream_store");
}

/* ========================================================================= */
/* 4. HARDWARE TOPOLOGY DETECTION IMPLEMENTATION                             */
/* ========================================================================= */

MSIMD_API sf_hardware_topology_t sf_detect_hardware_topology(const char* isa_desc) {
    (void)isa_desc;
    sf_hardware_topology_t topo{};
    topo.primary_cacheline_size = 64;
    topo.page_size = 4096;
    topo.huge_page_size = 2 * 1024 * 1024;
    topo.numa_nodes = 1;

#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::vector<uint8_t> buffer(len);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
        if (GetLogicalProcessorInformationEx(RelationAll, p, &len)) {
            DWORD offset = 0;
            uint32_t eff_class_0_count = 0;
            uint32_t eff_class_high_count = 0;
            uint32_t numa_count = 0;

            while (offset < len) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
                if (item->Relationship == RelationCache) {
                    uint8_t lvl = item->Cache.Level;
                    uint32_t sz_kb = (uint32_t)(item->Cache.CacheSize / 1024);
                    uint16_t line_sz = (uint16_t)item->Cache.LineSize;
                    uint8_t assoc = (uint8_t)item->Cache.Associativity;

                    if (line_sz > 0) topo.primary_cacheline_size = line_sz;

                    if (lvl == 1) {
                        if (item->Cache.Type == CacheInstruction) {
                            topo.l1i.size_kb = sz_kb;
                            topo.l1i.line_size = line_sz;
                            topo.l1i.associativity = assoc;
                            topo.l1i.type = 2;
                        } else {
                            topo.l1d.size_kb = sz_kb;
                            topo.l1d.line_size = line_sz;
                            topo.l1d.associativity = assoc;
                            topo.l1d.type = 1;
                        }
                    } else if (lvl == 2) {
                        topo.l2.size_kb = sz_kb;
                        topo.l2.line_size = line_sz;
                        topo.l2.associativity = assoc;
                        topo.l2.type = (item->Cache.Type == CacheData) ? 1 : 3;
                    } else if (lvl == 3) {
                        topo.l3.size_kb = sz_kb;
                        topo.l3.line_size = line_sz;
                        topo.l3.associativity = assoc;
                        topo.l3.type = 3;
                    }
                } else if (item->Relationship == RelationNumaNode) {
                    numa_count++;
                } else if (item->Relationship == RelationProcessorCore) {
                    topo.physical_cores++;
                    if (item->Processor.EfficiencyClass == 0) {
                        eff_class_0_count++;
                    } else {
                        eff_class_high_count++;
                    }
                }
                offset += item->Size;
            }

            if (numa_count > 0) topo.numa_nodes = (uint8_t)numa_count;

            if (eff_class_0_count > 0 && eff_class_high_count > 0) {
                topo.is_heterogeneous = 1;
                topo.p_cores = eff_class_high_count;
                topo.e_cores = eff_class_0_count;
            } else {
                topo.p_cores = topo.physical_cores;
                topo.e_cores = 0;
            }
        }
    }

#if defined(_M_X64) || defined(__x86_64__)
    if (topo.l1d.size_kb == 0 || topo.l2.size_kb == 0) {
        int cinfo[4] = { 0 };
        for (int sub = 0; sub < 8; ++sub) {
            sf_get_cpuid(cinfo, 4, sub);
            int ctype = cinfo[0] & 0x1F;
            if (ctype == 0) break;
            int clvl = (cinfo[0] >> 5) & 0x7;
            int line_sz = (cinfo[1] & 0xFFF) + 1;
            int partitions = ((cinfo[1] >> 12) & 0x3FF) + 1;
            int ways = ((cinfo[1] >> 22) & 0x3FF) + 1;
            int sets = cinfo[2] + 1;
            uint32_t sz_kb = (uint32_t)(((uint64_t)ways * partitions * line_sz * sets) / 1024);

            if (line_sz > 0) topo.primary_cacheline_size = (uint16_t)line_sz;
            if (clvl == 1) {
                if (ctype == 2) { topo.l1i.size_kb = sz_kb; topo.l1i.line_size = (uint16_t)line_sz; topo.l1i.associativity = (uint8_t)ways; topo.l1i.type = 2; }
                else { topo.l1d.size_kb = sz_kb; topo.l1d.line_size = (uint16_t)line_sz; topo.l1d.associativity = (uint8_t)ways; topo.l1d.type = 1; }
            } else if (clvl == 2) {
                topo.l2.size_kb = sz_kb; topo.l2.line_size = (uint16_t)line_sz; topo.l2.associativity = (uint8_t)ways; topo.l2.type = 3;
            } else if (clvl == 3) {
                topo.l3.size_kb = sz_kb; topo.l3.line_size = (uint16_t)line_sz; topo.l3.associativity = (uint8_t)ways; topo.l3.type = 3;
            }
        }
    }
#endif

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        topo.total_phys_ram_bytes = ms.ullTotalPhys;
        topo.avail_phys_ram_bytes = ms.ullAvailPhys;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    topo.page_size = si.dwPageSize;
    topo.logical_cores = si.dwNumberOfProcessors;
    if (topo.physical_cores == 0) topo.physical_cores = topo.logical_cores;
    if (topo.p_cores == 0) topo.p_cores = topo.physical_cores;

    SIZE_T lp_min = GetLargePageMinimum();
    if (lp_min > 0) {
        topo.huge_page_size = (uint32_t)lp_min;
        topo.huge_pages_supported = 1;
    }

#elif defined(__linux__) || defined(__ANDROID__)
    long l1d_sz = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    long l1i_sz = sysconf(_SC_LEVEL1_ICACHE_SIZE);
    long l2_sz = sysconf(_SC_LEVEL2_CACHE_SIZE);
    long l3_sz = sysconf(_SC_LEVEL3_CACHE_SIZE);
    long cl_sz = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

    if (l1d_sz > 0) { topo.l1d.size_kb = (uint32_t)(l1d_sz / 1024); topo.l1d.type = 1; }
    if (l1i_sz > 0) { topo.l1i.size_kb = (uint32_t)(l1i_sz / 1024); topo.l1i.type = 2; }
    if (l2_sz > 0) { topo.l2.size_kb = (uint32_t)(l2_sz / 1024); topo.l2.type = 3; }
    if (l3_sz > 0) { topo.l3.size_kb = (uint32_t)(l3_sz / 1024); topo.l3.type = 3; }
    if (cl_sz > 0)  topo.primary_cacheline_size = (uint16_t)cl_sz;

    FILE* fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                uint64_t kb = 0;
                if (sscanf(line + 9, "%llu", &kb) == 1) topo.total_phys_ram_bytes = kb * 1024;
            } else if (strncmp(line, "MemAvailable:", 13) == 0) {
                uint64_t kb = 0;
                if (sscanf(line + 13, "%llu", &kb) == 1) topo.avail_phys_ram_bytes = kb * 1024;
            } else if (strncmp(line, "Hugepagesize:", 13) == 0) {
                uint64_t kb = 0;
                if (sscanf(line + 13, "%llu", &kb) == 1) {
                    topo.huge_page_size = (uint32_t)(kb * 1024);
                    topo.huge_pages_supported = 1;
                }
            }
        }
        fclose(fp);
    }
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) topo.page_size = (uint32_t)ps;
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) {
        topo.logical_cores = (uint32_t)nprocs;
        topo.physical_cores = topo.logical_cores;
        topo.p_cores = topo.physical_cores;
    }
#endif

    if (topo.l1d.size_kb == 0) { topo.l1d.size_kb = 32; topo.l1d.line_size = 64; topo.l1d.associativity = 8; topo.l1d.type = 1; }
    if (topo.l1i.size_kb == 0) { topo.l1i.size_kb = 32; topo.l1i.line_size = 64; topo.l1i.associativity = 8; topo.l1i.type = 2; }
    if (topo.l2.size_kb == 0) { topo.l2.size_kb = 512; topo.l2.line_size = 64; topo.l2.associativity = 8; topo.l2.type = 3; }
    if (topo.primary_cacheline_size == 0) topo.primary_cacheline_size = 64;
    if (topo.page_size == 0) topo.page_size = 4096;
    if (topo.physical_cores == 0) topo.physical_cores = 1;
    if (topo.logical_cores == 0) topo.logical_cores = topo.physical_cores;
    if (topo.p_cores == 0) topo.p_cores = topo.physical_cores;

    snprintf(topo.summary, sizeof(topo.summary),
        "Redlet Hardware Topology: Physical Cores=%u, Logical Cores=%u, P-Cores=%u, E-Cores=%u, L1d=%u KB, L2=%u KB, L3=%u KB, NUMA Nodes=%u",
        topo.physical_cores, topo.logical_cores, topo.p_cores, topo.e_cores,
        topo.l1d.size_kb, topo.l2.size_kb, topo.l3.size_kb, (unsigned int)topo.numa_nodes);

    return topo;
}

/* ========================================================================= */
/* 5. HYBRID VS HOMOGENEOUS CPU EXECUTION ENGINE                             */
/* ========================================================================= */

MSIMD_API void sf_hybrid_homo_engine_t::init(const sf_hardware_topology_t* topo) {
    if (!topo) return;
    is_hybrid = (topo->is_heterogeneous != 0 && topo->p_cores > 0 && topo->e_cores > 0);
    p_core_count = is_hybrid ? topo->p_cores : topo->physical_cores;
    e_core_count = is_hybrid ? topo->e_cores : 0;
    lp_e_core_count = 0;

    uint32_t total_logical = topo->logical_cores > 0 ? topo->logical_cores : 1;
    if (total_logical > 64) total_logical = 64;
    all_core_mask = (total_logical == 64) ? ~0ULL : ((1ULL << total_logical) - 1);
    p_core_mask = 0;
    e_core_mask = 0;
    lp_e_core_mask = 0;

#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::vector<uint8_t> buffer(len);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) {
            DWORD offset = 0;
            while (offset < len) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
                if (item->Relationship == RelationProcessorCore) {
                    uint64_t mask = (uint64_t)item->Processor.GroupMask[0].Mask;
                    if (is_hybrid) {
                        if (item->Processor.EfficiencyClass == 0) {
                            e_core_mask |= mask;
                        } else {
                            p_core_mask |= mask;
                        }
                    }
                }
                offset += item->Size;
            }
        }
    }
#endif

    if (!is_hybrid || p_core_mask == 0) {
        p_core_mask = all_core_mask;
        e_core_mask = 0;
        topo_class = SF_CPU_TOPOLOGY_HOMOGENEOUS;
        r_ipc = 1.0;
        freq_ratio = 1.0;
        throughput_ratio = 1.0;
    } else {
        topo_class = SF_CPU_TOPOLOGY_HYBRID_INTEL;
        r_ipc = 2.60;
        freq_ratio = 1.40;
        throughput_ratio = r_ipc * freq_ratio;
    }
}

MSIMD_API bool sf_hybrid_homo_engine_t::bind_current_thread(SF_custominiVm_core_tier_t tier) {
    uint64_t target_mask = all_core_mask;
    if (tier == SF_CORE_TIER_PERFORMANCE && p_core_mask != 0) {
        target_mask = p_core_mask;
    } else if (tier == SF_CORE_TIER_EFFICIENCY && e_core_mask != 0) {
        target_mask = e_core_mask;
    }

#if defined(_WIN32)
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)target_mask);
    return prev != 0;
#elif defined(__linux__) || defined(__ANDROID__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 64; ++i) {
        if (target_mask & (1ULL << i)) CPU_SET(i, &cpuset);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)target_mask;
    return true;
#endif
}

MSIMD_API bool sf_hybrid_homo_engine_t::set_thread_qos(SF_custominiVm_core_tier_t tier) {
#if defined(_WIN32)
    typedef enum _THREAD_INFO_CLASS_LOCAL {
        LocalThreadPowerThrottling = 1
    } THREAD_INFO_CLASS_LOCAL;

    typedef struct _THREAD_POWER_THROTTLING_LOCAL {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    } THREAD_POWER_THROTTLING_LOCAL;

    THREAD_POWER_THROTTLING_LOCAL throttling{};
    throttling.Version = 1;

    if (tier == SF_CORE_TIER_EFFICIENCY) {
        throttling.ControlMask = 0x1;
        throttling.StateMask = 0x1;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    } else if (tier == SF_CORE_TIER_PERFORMANCE) {
        throttling.ControlMask = 0x1;
        throttling.StateMask = 0x0;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    typedef BOOL(WINAPI* SetThreadInformationFn)(HANDLE, int, PVOID, DWORD);
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        SetThreadInformationFn pfn = (SetThreadInformationFn)GetProcAddress(hKernel32, "SetThreadInformation");
        if (pfn) {
            return pfn(GetCurrentThread(), LocalThreadPowerThrottling, &throttling, sizeof(throttling)) != FALSE;
        }
    }
    return false;
#else
    (void)tier;
    return true;
#endif
}

MSIMD_API void sf_hybrid_homo_engine_t::partition_asymmetric_work(size_t total_elements,
                                                                 size_t num_p, size_t num_e,
                                                                 std::vector<std::pair<size_t, size_t>>& out_p_ranges,
                                                                 std::vector<std::pair<size_t, size_t>>& out_e_ranges,
                                                                 size_t align_elements) const {
    out_p_ranges.clear();
    out_e_ranges.clear();
    if (total_elements == 0) return;

    if (!is_hybrid || num_e == 0 || num_p == 0) {
        size_t total_workers = (num_p + num_e > 0) ? (num_p + num_e) : 1;
        size_t base_chunk = total_elements / total_workers;
        size_t remainder = total_elements % total_workers;
        size_t curr = 0;
        for (size_t i = 0; i < total_workers; ++i) {
            size_t sz = base_chunk + (i < remainder ? 1 : 0);
            if (num_p > 0 && i < num_p) {
                out_p_ranges.emplace_back(curr, curr + sz);
            } else {
                out_e_ranges.emplace_back(curr, curr + sz);
            }
            curr += sz;
        }
        return;
    }

    double weight_p = throughput_ratio;
    double weight_e = 1.0;
    double total_capacity = (double)num_p * weight_p + (double)num_e * weight_e;
    double elem_per_unit = (double)total_elements / total_capacity;

    size_t current_idx = 0;
    for (size_t i = 0; i < num_p; ++i) {
        size_t raw_sz = (size_t)(elem_per_unit * weight_p + 0.5);
        if (align_elements > 1 && i + 1 < num_p + num_e) {
            raw_sz = ((raw_sz + align_elements - 1) / align_elements) * align_elements;
        }
        size_t end_idx = (std::min)(current_idx + raw_sz, total_elements);
        out_p_ranges.emplace_back(current_idx, end_idx);
        current_idx = end_idx;
        if (current_idx >= total_elements) break;
    }

    for (size_t i = 0; i < num_e; ++i) {
        if (current_idx >= total_elements) {
            out_e_ranges.emplace_back(total_elements, total_elements);
            continue;
        }
        size_t remaining_e = num_e - i;
        size_t raw_sz = (total_elements - current_idx) / remaining_e;
        if (align_elements > 1 && i + 1 < num_e) {
            raw_sz = ((raw_sz + align_elements - 1) / align_elements) * align_elements;
        }
        size_t end_idx = (std::min)(current_idx + raw_sz, total_elements);
        if (i + 1 == num_e) end_idx = total_elements;
        out_e_ranges.emplace_back(current_idx, end_idx);
        current_idx = end_idx;
    }
}

MSIMD_API uint32_t sf_hybrid_homo_engine_t::get_optimal_worker_count(bool is_heavy_compute) const {
    if (is_heavy_compute && is_hybrid && p_core_count > 0) {
        return p_core_count;
    }
    return p_core_count + e_core_count;
}

MSIMD_API bool sf_hybrid_homo_engine_t::should_steer_to_pcore(bool is_heavy_simd, size_t vector_bytes) const {
    if (!is_hybrid) return false;
    return is_heavy_simd || (vector_bytes >= 32);
}

MSIMD_API SF_custominiVm_core_tier_t sf_hybrid_homo_engine_t::classify_core(uint32_t core_id) const {
    if (!is_hybrid) return SF_CORE_TIER_ANY;
    if (core_id < 64) {
        uint64_t bit = (1ULL << core_id);
        if (p_core_mask & bit) return SF_CORE_TIER_PERFORMANCE;
        if (e_core_mask & bit) return SF_CORE_TIER_EFFICIENCY;
    }
    return SF_CORE_TIER_ANY;
}

/* ========================================================================= */
/* 6. NUMA VS UMA MEMORY ARCHITECTURE ENGINE                                 */
/* ========================================================================= */

MSIMD_API void sf_numa_uma_engine_t::init(const sf_hardware_topology_t* topo) {
    if (!topo) return;
    node_count = topo->numa_nodes > 0 ? topo->numa_nodes : 1;
    if (node_count > 8) node_count = 8;
    interconnect_bandwidth_gbps = topo->estimated_bandwidth_gbps;
    is_uma = (topo->is_uma != 0 || node_count <= 1);
    mem_arch = is_uma ? SF_MEM_ARCH_UMA : SF_MEM_ARCH_NUMA_SYMMETRIC;

    for (uint8_t i = 0; i < 8; ++i) {
        nodes[i].node_id = i;
        nodes[i].allocated_bytes.store(0, std::memory_order_relaxed);
        nodes[i].access_count.store(0, std::memory_order_relaxed);
        nodes[i].local_reads.store(0, std::memory_order_relaxed);
        nodes[i].remote_reads.store(0, std::memory_order_relaxed);
        nodes[i].processor_mask = 0;

        for (uint8_t j = 0; j < 8; ++j) {
            access_matrix[i][j].store(0, std::memory_order_relaxed);
            if (i == j) {
                slit_distance[i][j] = 10;
            } else if ((i / 2) == (j / 2)) {
                slit_distance[i][j] = 20;
            } else {
                slit_distance[i][j] = 30;
            }
        }
    }

#if defined(_WIN32)
    if (!is_uma) {
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationNumaNode, nullptr, &len);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
            std::vector<uint8_t> buffer(len);
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
            if (GetLogicalProcessorInformationEx(RelationNumaNode, p, &len)) {
                DWORD offset = 0;
                while (offset < len) {
                    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
                    if (item->Relationship == RelationNumaNode) {
                        ULONG nid = item->NumaNode.NodeNumber;
                        if (nid < 8) {
                            nodes[nid].processor_mask = (uint64_t)item->NumaNode.GroupMask.Mask;
                        }
                    }
                    offset += item->Size;
                }
            }
        }
    }
#endif
}

MSIMD_API void* sf_numa_uma_engine_t::alloc_node(size_t bytes, uint8_t preferred_node) {
    if (bytes == 0) return nullptr;
    uint8_t n = (preferred_node < node_count) ? preferred_node : 0;
    void* ptr = nullptr;

#if defined(_WIN32)
    if (!is_uma && node_count > 1) {
        ptr = VirtualAllocExNuma(GetCurrentProcess(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, (DWORD)n);
    }
    if (!ptr) {
        ptr = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
#else
    ptr = sf_os_reserve_memory(bytes);
    if (ptr) sf_os_commit_memory(ptr, bytes);
#endif
    if (ptr) {
        nodes[n].allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    return ptr;
}

MSIMD_API void sf_numa_uma_engine_t::free_node(void* ptr, size_t bytes, uint8_t node) {
    if (!ptr) return;
    uint8_t n = (node < 8) ? node : 0;
    nodes[n].allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    sf_os_release_memory(ptr, bytes);
#endif
}

MSIMD_API uint8_t sf_numa_uma_engine_t::get_current_thread_node() {
    if (is_uma || node_count <= 1) return 0;
#if defined(_WIN32)
    PROCESSOR_NUMBER proc_num;
    GetCurrentProcessorNumberEx(&proc_num);
    USHORT node_num = 0;
    if (GetNumaProcessorNodeEx(&proc_num, &node_num)) {
        return (uint8_t)node_num;
    }
    return 0;
#else
    return 0;
#endif
}

MSIMD_API void sf_numa_uma_engine_t::record_access(uint8_t accessing_node, uint8_t target_node, size_t bytes) {
    (void)bytes;
    if (accessing_node >= 8 || target_node >= 8) return;
    access_matrix[accessing_node][target_node].fetch_add(1, std::memory_order_relaxed);
    nodes[target_node].access_count.fetch_add(1, std::memory_order_relaxed);
    if (accessing_node == target_node) {
        nodes[target_node].local_reads.fetch_add(1, std::memory_order_relaxed);
    } else {
        nodes[target_node].remote_reads.fetch_add(1, std::memory_order_relaxed);
    }
}

MSIMD_API float sf_numa_uma_engine_t::get_remote_heat_ratio(uint8_t node) const {
    if (node >= 8) return 0.0f;
    uint64_t loc = nodes[node].local_reads.load(std::memory_order_relaxed);
    uint64_t rem = nodes[node].remote_reads.load(std::memory_order_relaxed);
    uint64_t tot = loc + rem;
    return (tot > 0) ? (float)rem / (float)tot : 0.0f;
}

MSIMD_API bool sf_numa_uma_engine_t::should_migrate(uint8_t host_node, uint8_t& out_dominant_node, float threshold) const {
    if (is_uma || node_count <= 1 || host_node >= 8) return false;
    uint64_t max_remote = 0;
    uint8_t  best_node = host_node;
    uint64_t total_remote = 0;

    for (uint8_t src = 0; src < node_count; ++src) {
        if (src == host_node) continue;
        uint64_t acc = access_matrix[src][host_node].load(std::memory_order_relaxed);
        total_remote += acc;
        if (acc > max_remote) {
            max_remote = acc;
            best_node = src;
        }
    }

    uint64_t local_acc = access_matrix[host_node][host_node].load(std::memory_order_relaxed);
    uint64_t total = local_acc + total_remote;
    if (total < 1000) return false;

    float rem_ratio = (float)max_remote / (float)total;
    if (rem_ratio >= threshold && best_node != host_node) {
        out_dominant_node = best_node;
        return true;
    }
    return false;
}

MSIMD_API void sf_numa_uma_engine_t::distribute_numa_work(size_t total_elements,
                                                         std::vector<std::pair<size_t, size_t>>& out_node_slices) const {
    out_node_slices.clear();
    if (total_elements == 0) return;
    uint8_t n_nodes = (is_uma || node_count <= 1) ? 1 : node_count;
    size_t base = total_elements / n_nodes;
    size_t rem = total_elements % n_nodes;
    size_t cur = 0;
    for (uint8_t n = 0; n < n_nodes; ++n) {
        size_t sz = base + (n < rem ? 1 : 0);
        out_node_slices.emplace_back(cur, cur + sz);
        cur += sz;
    }
}

/* ========================================================================= */
/* 7. SMT (HYPER-THREADING) VS NON-HT CPU EXECUTION ENGINE                   */
/* ========================================================================= */

MSIMD_API void sf_smt_ht_engine_t::init(const sf_hardware_topology_t* topo) {
    if (!topo) return;
    physical_cores = topo->physical_cores > 0 ? topo->physical_cores : 1;
    logical_cores = topo->logical_cores > 0 ? topo->logical_cores : physical_cores;
    is_ht_active = (logical_cores > physical_cores);
    threads_per_core = is_ht_active ? (logical_cores / physical_cores) : 1;
    if (threads_per_core >= 4) {
        smt_mode = SF_SMT_MODE_4WAY_PLUS;
    } else if (is_ht_active) {
        smt_mode = SF_SMT_MODE_2WAY;
    } else {
        smt_mode = SF_SMT_MODE_DISABLED_OR_ABSENT;
    }

    primary_threads_mask = 0;
    sibling_threads_mask = 0;
    for (int i = 0; i < 64; ++i) sibling_map[i] = (uint8_t)i;

#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::vector<uint8_t> buffer(len);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) {
            DWORD offset = 0;
            while (offset < len) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
                if (item->Relationship == RelationProcessorCore) {
                    uint64_t mask = (uint64_t)item->Processor.GroupMask[0].Mask;
                    if (mask != 0) {
                        uint64_t primary_bit = (mask & (~mask + 1));
                        primary_threads_mask |= primary_bit;
                        sibling_threads_mask |= (mask & ~primary_bit);

                        std::vector<int> core_ids;
                        for (int b = 0; b < 64; ++b) {
                            if (mask & (1ULL << b)) core_ids.push_back(b);
                        }
                        if (core_ids.size() >= 2) {
                            sibling_map[core_ids[0]] = (uint8_t)core_ids[1];
                            sibling_map[core_ids[1]] = (uint8_t)core_ids[0];
                        }
                    }
                }
                offset += item->Size;
            }
        }
    }
#endif
    if (primary_threads_mask == 0) {
        if (is_ht_active) {
            for (uint32_t i = 0; i < logical_cores && i < 64; ++i) {
                if (i % 2 == 0) {
                    primary_threads_mask |= (1ULL << i);
                    if (i + 1 < 64) {
                        sibling_map[i] = (uint8_t)(i + 1);
                        sibling_map[i + 1] = (uint8_t)i;
                    }
                } else {
                    sibling_threads_mask |= (1ULL << i);
                }
            }
        } else {
            uint32_t max_bits = logical_cores < 64 ? logical_cores : 64;
            primary_threads_mask = (max_bits == 64) ? ~0ULL : ((1ULL << max_bits) - 1);
        }
    }
}

MSIMD_API uint32_t sf_smt_ht_engine_t::get_optimal_concurrency(SF_custominiVm_workload_affinity_policy_t policy) const {
    if (policy == SF_AFFINITY_PHYSICAL_CORES_ONLY || !is_ht_active) {
        return physical_cores;
    }
    return logical_cores;
}

MSIMD_API SF_custominiVm_hardware_tier_t sf_smt_ht_engine_t::detect_hardware_tier() const {
    uint32_t l = logical_cores > 0 ? logical_cores : (physical_cores > 0 ? physical_cores : 4);
    uint32_t p = physical_cores > 0 ? physical_cores : l;
    if (l > 32 || p > 32) return SF_HW_TIER_UP;
    if (l >= 15) return SF_HW_TIER_MID_UP;
    if (l >= 9) return SF_HW_TIER_MID_MID;
    if (l >= 5) return SF_HW_TIER_MID_DOWN;
    return SF_HW_TIER_DOWN;
}

MSIMD_API const char* sf_smt_ht_engine_t::get_hardware_tier_name() const {
    switch (detect_hardware_tier()) {
    case SF_HW_TIER_DOWN:      return "Down (Entry / Embedded / SBC)";
    case SF_HW_TIER_MID_DOWN: return "Mid-Down (Lower-Midrange / Laptop)";
    case SF_HW_TIER_MID_MID:  return "Mid-Mid (Midrange Desktop)";
    case SF_HW_TIER_MID_UP:   return "Mid-Up (High-End Desktop)";
    case SF_HW_TIER_UP:       return "Up / Extreme (HPC / Workstation / Server)";
    default:                  return "Standard";
    }
}

MSIMD_API sf_smt_ht_engine_t::sf_thread_budget_t sf_smt_ht_engine_t::calculate_thread_budget() const {
    sf_thread_budget_t b{};
    SF_custominiVm_hardware_tier_t tier = detect_hardware_tier();
    uint32_t l = logical_cores > 0 ? logical_cores : 4;

    switch (tier) {
    case SF_HW_TIER_UP:
        b.gc_threads = std::min(16u, std::max(4u, l / 8));
        b.puzzle_threads = std::min(8u, std::max(2u, l / 16));
        b.vm_threads = std::min(104u, (l > (b.gc_threads + b.puzzle_threads)) ? (l - b.gc_threads - b.puzzle_threads) : 4u);
        b.total_threads = b.gc_threads + b.puzzle_threads + b.vm_threads;
        if (b.total_threads > 128) {
            b.total_threads = 128;
            b.vm_threads = 128 - b.gc_threads - b.puzzle_threads;
        }
        break;
    case SF_HW_TIER_MID_UP:
        b.gc_threads = (physical_cores >= 16) ? 6 : 4;
        b.puzzle_threads = 3;
        b.vm_threads = 15;
        b.total_threads = b.gc_threads + b.puzzle_threads + b.vm_threads;
        break;
    case SF_HW_TIER_MID_MID:
        b.gc_threads = (l >= 14) ? 4 : 3;
        b.puzzle_threads = 2;
        b.vm_threads = (l >= 14) ? 8 : 7;
        b.total_threads = b.gc_threads + b.puzzle_threads + b.vm_threads;
        break;
    case SF_HW_TIER_MID_DOWN:
        b.gc_threads = 2;
        b.puzzle_threads = 2;
        b.vm_threads = (l >= 8) ? 4 : 3;
        b.total_threads = b.gc_threads + b.puzzle_threads + b.vm_threads;
        break;
    case SF_HW_TIER_DOWN:
    default:
        b.gc_threads = 1;
        b.puzzle_threads = 1;
        b.vm_threads = (l >= 4) ? 2 : 1;
        b.total_threads = b.gc_threads + b.puzzle_threads + b.vm_threads;
        break;
    }
    return b;
}

MSIMD_API bool sf_smt_ht_engine_t::apply_affinity_policy(SF_custominiVm_workload_affinity_policy_t policy) {
    uint64_t target_mask = 0;
    if (policy == SF_AFFINITY_PHYSICAL_CORES_ONLY) {
        target_mask = primary_threads_mask;
    } else if (policy == SF_AFFINITY_ALL_LOGICAL_THREADS) {
        target_mask = primary_threads_mask | sibling_threads_mask;
    }
    if (target_mask == 0) return true;

#if defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)target_mask) != 0;
#elif defined(__linux__) || defined(__ANDROID__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 64; ++i) {
        if (target_mask & (1ULL << i)) CPU_SET(i, &cpuset);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    return true;
#endif
}

MSIMD_API bool sf_smt_ht_engine_t::is_primary_core(uint32_t core_id) const {
    if (core_id >= 64) return false;
    return (primary_threads_mask & (1ULL << core_id)) != 0;
}

MSIMD_API uint8_t sf_smt_ht_engine_t::get_sibling_core(uint8_t core_id) const {
    return (core_id < 64) ? sibling_map[core_id] : core_id;
}

MSIMD_API double sf_smt_ht_engine_t::estimate_port_contention_penalty(bool is_fma_vector) const {
    if (!is_ht_active) return 1.0;
    return is_fma_vector ? 1.65 : 1.15;
}

/* ========================================================================= */
/* 8. DIVERSE SRAM & CACHE ARCHITECTURES ENGINE                              */
/* ========================================================================= */

MSIMD_API void sf_sram_cache_engine_t::init(const sf_hardware_topology_t* topo) {
    if (!topo) return;
    l1d_kb = topo->l1d.size_kb > 0 ? topo->l1d.size_kb : 32;
    l1i_kb = topo->l1i.size_kb > 0 ? topo->l1i.size_kb : 32;
    l2_kb = topo->l2.size_kb > 0 ? topo->l2.size_kb : 512;
    l3_kb = topo->l3.size_kb > 0 ? topo->l3.size_kb : 16384;
    cacheline_bytes = topo->primary_cacheline_size > 0 ? topo->primary_cacheline_size : 64;

    if (l3_kb >= 130 * 1024) {
        has_intel_bllc = true;
        has_3d_vcache = false;
        sram_regime = SF_SRAM_REGIME_3D_VCACHE;
    } else if (l3_kb >= 48 * 1024) {
        has_3d_vcache = true;
        has_intel_bllc = false;
        sram_regime = SF_SRAM_REGIME_3D_VCACHE;
    } else if (cacheline_bytes >= 128) {
        sram_regime = SF_SRAM_REGIME_WIDE_CACHELINE_128B;
    } else {
        sram_regime = SF_SRAM_REGIME_STANDARD_HIERARCHICAL;
    }

    if (!scratchpad_base) {
#if defined(_WIN32)
        scratchpad_base = (uint8_t*)VirtualAlloc(nullptr, SCRATCHPAD_CAPACITY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        scratchpad_base = (uint8_t*)sf_os_reserve_memory(SCRATCHPAD_CAPACITY);
        if (scratchpad_base) sf_os_commit_memory(scratchpad_base, SCRATCHPAD_CAPACITY);
#endif
        if (scratchpad_base) {
            has_scratchpad_sram = true;
        }
    }
    scratchpad_offset.store(0, std::memory_order_relaxed);
}

MSIMD_API void sf_sram_cache_engine_t::cleanup() {
    if (scratchpad_base) {
#if defined(_WIN32)
        VirtualFree(scratchpad_base, 0, MEM_RELEASE);
#else
        sf_os_release_memory(scratchpad_base, SCRATCHPAD_CAPACITY);
#endif
        scratchpad_base = nullptr;
        has_scratchpad_sram = false;
    }
}

MSIMD_API void* sf_sram_cache_engine_t::scratchpad_alloc(size_t bytes, size_t alignment) {
    if (!scratchpad_base || bytes == 0) return nullptr;
    if (alignment == 0) alignment = 64;
    size_t cur = scratchpad_offset.load(std::memory_order_relaxed);
    while (true) {
        size_t aligned = (cur + alignment - 1) & ~(alignment - 1);
        size_t next = aligned + bytes;
        if (next > SCRATCHPAD_CAPACITY) return nullptr;
        if (scratchpad_offset.compare_exchange_weak(cur, next, std::memory_order_acquire, std::memory_order_relaxed)) {
            return scratchpad_base + aligned;
        }
    }
}

MSIMD_API void sf_sram_cache_engine_t::scratchpad_reset() {
    scratchpad_offset.store(0, std::memory_order_release);
}

MSIMD_API size_t sf_sram_cache_engine_t::pad_to_cacheline(size_t size) const {
    size_t cl = cacheline_bytes > 0 ? cacheline_bytes : 64;
    return (size + cl - 1) & ~(cl - 1);
}

MSIMD_API void* sf_sram_cache_engine_t::align_to_cacheline(void* ptr) const {
    size_t cl = cacheline_bytes > 0 ? cacheline_bytes : 64;
    uintptr_t addr = (uintptr_t)ptr;
    return (void*)((addr + cl - 1) & ~(cl - 1));
}

MSIMD_API bool sf_sram_cache_engine_t::should_stream_store(size_t tensor_bytes) const {
    size_t sram_threshold;
    if (has_intel_bllc) {
        sram_threshold = 72ULL * 1024 * 1024;
    } else if (has_3d_vcache) {
        sram_threshold = 48ULL * 1024 * 1024;
    } else {
        sram_threshold = (l3_kb > 0 ? (l3_kb * 1024ULL) / 2 : (l2_kb * 1024ULL));
    }
    return tensor_bytes > sram_threshold;
}

MSIMD_API void sf_sram_cache_engine_t::flush_wc_buffers() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(__clang__) || defined(__GNUC__)
    __asm__ __volatile__("dmb ishst" ::: "memory");
#endif
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
}

MSIMD_API void sf_sram_cache_engine_t::compute_blis_tiles(size_t elem_size,
                                                         size_t total_M, size_t total_N, size_t total_K,
                                                         int& M_C, int& N_C, int& K_C,
                                                         int& M_R, int& N_R) const {
    if (elem_size == 0) elem_size = 4;
    M_R = 8;
    N_R = 6;

    size_t l1d_bytes = (l1d_kb > 0 ? l1d_kb : 32) * 1024ULL;
    size_t l1d_target = l1d_bytes / 2;
    int kc_calc = (int)(l1d_target / (M_R * elem_size));
    if (kc_calc < 64)  kc_calc = 64;
    if (kc_calc > 512) kc_calc = 512;
    K_C = (int)(std::min)((size_t)kc_calc, total_K > 0 ? total_K : (size_t)kc_calc);

    size_t l2_bytes = (l2_kb > 0 ? l2_kb : 512) * 1024ULL;
    size_t l2_target = (l2_bytes * 6) / 10;
    int mc_calc = (int)(l2_target / (K_C * elem_size));
    if (mc_calc < 128)  mc_calc = 128;
    if (mc_calc > 2048) mc_calc = 2048;
    mc_calc = (mc_calc / M_R) * M_R;
    if (mc_calc == 0) mc_calc = M_R;
    M_C = (int)(std::min)((size_t)mc_calc, total_M > 0 ? total_M : (size_t)mc_calc);

    if (has_intel_bllc) {
        N_C = 16384;
    } else if (has_3d_vcache) {
        N_C = 8192;
    } else {
        size_t l3_bytes = (l3_kb > 0 ? l3_kb : 16384) * 1024ULL;
        size_t l3_target = l3_bytes / 3;
        int nc_calc = (int)(l3_target / (K_C * elem_size));
        if (nc_calc < 256)  nc_calc = 256;
        if (nc_calc > 4096) nc_calc = 4096;
        N_C = nc_calc;
    }
    N_C = (N_C / N_R) * N_R;
    if (N_C == 0) N_C = N_R;
    if (total_N > 0 && (size_t)N_C > total_N) N_C = (int)total_N;
}

MSIMD_API void sf_sram_cache_engine_t::get_optimal_tile_sizes(size_t elem_size, int& tile_M, int& tile_N, int& tile_K) const {
    int mr = 8, nr = 6;
    compute_blis_tiles(elem_size, 0, 0, 0, tile_M, tile_N, tile_K, mr, nr);
}

MSIMD_API size_t sf_sram_cache_engine_t::get_cache_color_stride(uint32_t ways) const {
    if (ways == 0) ways = 8;
    return (l1d_kb * 1024ULL) / ways;
}

/* ========================================================================= */
/* 9. UPCLOCKER ENGINE                                                       */
/* ========================================================================= */

MSIMD_API void sf_upclocker_t::pin_max_frequency_warmup(uint32_t duration_us) {
#if defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64)
    uint64_t t0 = sacculus_rdtsc();
    uint64_t target_cycles = (uint64_t)duration_us * 3500ULL;
    volatile uint64_t acc = 0x5A5A5A5AULL;
    while ((sacculus_rdtsc() - t0) < target_cycles) {
        acc = (acc * 6364136223846793005ULL) + 1442695040888963407ULL;
        acc = acc ^ (acc >> 17);
    }
    (void)acc;
#else
    (void)duration_us;
#endif
}

MSIMD_API void sf_upclocker_t::enable(bool boost) {
    is_boosted.store(boost, std::memory_order_relaxed);
    if (boost) {
#if defined(_WIN32)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        pin_max_frequency_warmup(100);
#endif
    } else {
#if defined(_WIN32)
        SetThreadExecutionState(ES_CONTINUOUS);
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#endif
    }
}

/* ========================================================================= */
/* 10. EXECUTION FOCUS ENGINE                                                */
/* ========================================================================= */

MSIMD_API void sf_execution_focus_engine_t::init(const sf_hardware_topology_t* topo,
                                                sf_hybrid_homo_engine_t* hybrid_eng,
                                                const sf_sram_cache_engine_t* sram_eng) {
    (void)sram_eng;
    hybrid_eng_ref = hybrid_eng;
    total_cores = topo ? topo->physical_cores : (uint32_t)std::thread::hardware_concurrency();
    if (total_cores == 0) total_cores = 1;

    hw_is_hybrid = (hybrid_eng && hybrid_eng->is_hybrid) || (topo && topo->is_heterogeneous != 0);
    is_hybrid = hw_is_hybrid;
    is_homogeneous = !hw_is_hybrid;
    p_cores = (hybrid_eng && hybrid_eng->p_core_count > 0) ? hybrid_eng->p_core_count : (topo ? topo->p_cores : total_cores);
    e_cores = (hybrid_eng && hw_is_hybrid) ? hybrid_eng->e_core_count : (topo ? topo->e_cores : 0);

    char vendor[16] = { 0 };
    uint32_t family = 0, model = 0;
#if defined(_M_X64) || defined(__x86_64__)
    int cinfo[4] = { 0 };
    sf_get_cpuid(cinfo, 0, 0);
    memcpy(vendor + 0, &cinfo[1], 4);
    memcpy(vendor + 4, &cinfo[3], 4);
    memcpy(vendor + 8, &cinfo[2], 4);
    vendor[12] = '\0';

    sf_get_cpuid(cinfo, 1, 0);
    uint32_t raw_f = (cinfo[0] >> 8) & 0xF;
    uint32_t raw_m = (cinfo[0] >> 4) & 0xF;
    uint32_t ext_f = (cinfo[0] >> 20) & 0xFF;
    uint32_t ext_m = (cinfo[0] >> 16) & 0xF;
    family = (raw_f == 15) ? (raw_f + ext_f) : raw_f;
    model = ((raw_f == 6 || raw_f == 15) ? (ext_m << 4) : 0) | raw_m;
#endif

    matched_uarch = nullptr;
    for (const auto& rec : SF_TABLEGEN_UARCH_RECORDS) {
        bool vmatch = false;
        if (strcmp(rec.vendor_code, "INTEL") == 0 && (strstr(vendor, "Intel") || strstr(vendor, "INTEL") || vendor[0] == '\0')) vmatch = true;
        else if (strcmp(rec.vendor_code, "AMD") == 0 && (strstr(vendor, "AMD") || strstr(vendor, "AuthenticAMD"))) vmatch = true;

        if (vmatch && rec.family == family && model >= rec.model_min && model <= rec.model_max) {
            matched_uarch = &rec;
            break;
        }
    }

    if (!matched_uarch) {
        matched_uarch = &SF_TABLEGEN_UARCH_RECORDS[0]; // Arrow Lake-S default
    }

    snprintf(uarch_summary, sizeof(uarch_summary), "%s [%s | %s]",
        matched_uarch->uarch_name, matched_uarch->profile_tag,
        hw_is_hybrid ? "Hybrid P+E" : "Homogeneous");

    SF_custominiVm_execution_focus_t detected = detect_focus(topo, hybrid_eng);
    SF_custominiVm_execution_focus_t target = configured_focus.load(std::memory_order_relaxed);
    if (target == SF_FOCUS_AUTO) {
        apply_state(detected);
    } else {
        set_focus(target);
    }
}

MSIMD_API SF_custominiVm_execution_focus_t sf_execution_focus_engine_t::detect_focus(
    const sf_hardware_topology_t* topo,
    const sf_hybrid_homo_engine_t* hybrid_eng) const {
    bool hyb = (hybrid_eng && hybrid_eng->is_hybrid) || (topo && topo->is_heterogeneous != 0) || hw_is_hybrid;
    uint32_t c_phys = (topo && topo->physical_cores > 0) ? topo->physical_cores : total_cores;
    if (c_phys == 0) c_phys = 1;

    if (c_phys <= 8) {
        return hyb ? SF_FOCUS_SCP_HYBRID : SF_FOCUS_SCP_HOMO;
    }
    if (c_phys >= 64) {
        return hyb ? SF_FOCUS_DUAL_HYBRID : SF_FOCUS_DUAL_HOMO;
    }
    if (hyb) {
        return SF_FOCUS_DUAL_HYBRID;
    }
    if (c_phys >= 12) {
        return SF_FOCUS_DUAL_HOMO;
    }
    return SF_FOCUS_MCP_HOMO;
}

MSIMD_API void sf_execution_focus_engine_t::set_focus(SF_custominiVm_execution_focus_t focus) {
    configured_focus.store(focus, std::memory_order_relaxed);
    SF_custominiVm_execution_focus_t resolved = focus;
    if (focus == SF_FOCUS_AUTO) {
        resolved = detect_focus(nullptr, hybrid_eng_ref);
    } else if (focus == SF_FOCUS_SCP) {
        resolved = hw_is_hybrid ? SF_FOCUS_SCP_HYBRID : SF_FOCUS_SCP_HOMO;
    } else if (focus == SF_FOCUS_MCP) {
        resolved = hw_is_hybrid ? SF_FOCUS_MCP_HYBRID : SF_FOCUS_MCP_HOMO;
    } else if (focus == SF_FOCUS_HYBRID_DUAL || focus == SF_FOCUS_DUAL) {
        resolved = hw_is_hybrid ? SF_FOCUS_DUAL_HYBRID : SF_FOCUS_DUAL_HOMO;
    }
    apply_state(resolved);
}

MSIMD_API void sf_execution_focus_engine_t::apply_state(SF_custominiVm_execution_focus_t resolved) {
    active_focus.store(resolved, std::memory_order_release);
    switch (resolved) {
    case SF_FOCUS_SCP_HOMO:
        is_scp = true; is_mcp = false; is_dual = false;
        is_hybrid = false; is_homogeneous = true;
        p_weight = 1.0; e_weight = 1.0;
        if (hybrid_eng_ref) hybrid_eng_ref->bind_current_thread(SF_CORE_TIER_ANY);
        break;
    case SF_FOCUS_SCP_HYBRID:
        is_scp = true; is_mcp = false; is_dual = false;
        is_hybrid = true; is_homogeneous = false;
        p_weight = 1.0; e_weight = 0.0;
        if (hybrid_eng_ref) hybrid_eng_ref->bind_current_thread(SF_CORE_TIER_PERFORMANCE);
        break;
    case SF_FOCUS_MCP_HOMO:
        is_scp = false; is_mcp = true; is_dual = false;
        is_hybrid = false; is_homogeneous = true;
        p_weight = 1.0; e_weight = 1.0;
        if (hybrid_eng_ref) hybrid_eng_ref->bind_current_thread(SF_CORE_TIER_ANY);
        break;
    case SF_FOCUS_MCP_HYBRID:
        is_scp = false; is_mcp = true; is_dual = false;
        is_hybrid = true; is_homogeneous = false;
        p_weight = 2.5; e_weight = 1.0;
        break;
    case SF_FOCUS_DUAL_HOMO:
        is_scp = false; is_mcp = false; is_dual = true;
        is_hybrid = false; is_homogeneous = true;
        p_weight = 1.0; e_weight = 1.0;
        if (hybrid_eng_ref) hybrid_eng_ref->bind_current_thread(SF_CORE_TIER_ANY);
        break;
    case SF_FOCUS_DUAL_HYBRID:
        is_scp = false; is_mcp = false; is_dual = true;
        is_hybrid = true; is_homogeneous = false;
        p_weight = 2.5; e_weight = 1.0;
        break;
    default:
        break;
    }
}

/* ========================================================================= */
/* 11. HARDWARE PERFORMANCE COUNTER (PMU) TELEMETRY ENGINE                   */
/* ========================================================================= */

MSIMD_API void sf_pmu_session_t::begin() noexcept {
    start_ns = sacculus_now_ns();
    start_tsc = sacculus_rdtsc();
}

MSIMD_API void sf_pmu_session_t::end(SF_custominiVm_pmu_stat_t* out, size_t bytes_processed) noexcept {
    uint64_t delta_tsc = sacculus_rdtsc() - start_tsc;
    uint64_t delta_ns = sacculus_now_ns() - start_ns;
    if (delta_tsc == 0) delta_tsc = 1;
    if (delta_ns == 0)  delta_ns = 1;

    SF_custominiVm_pmu_stat_t st{};
    st.cycles = delta_tsc;
    st.hw_pmu_active = 0;

#if defined(_WIN32)
    ULONG64 thread_cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &thread_cycles) && thread_cycles > 0) {
        st.hw_pmu_active = 1;
    }
#endif
    if (st.instructions == 0) {
        st.instructions = (bytes_processed > 0) ? (uint64_t)(bytes_processed / 16) + (st.cycles * 2) : (st.cycles * 2);
    }
    st.ipc = (double)st.instructions / (double)(st.cycles > 0 ? st.cycles : 1);
    double eff_ghz = (double)st.cycles / (double)delta_ns;
    double nom_ghz = 3.5;
    st.freq_downclock_ratio = (eff_ghz > 0.1) ? std::min(1.25, std::max(0.50, eff_ghz / nom_ghz)) : 1.0;
    if (out) *out = st;
}

/* ========================================================================= */
/* 12. UNIFIED HARDWARE ENGINE NEXUS IMPLEMENTATION                          */
/* ========================================================================= */

MSIMD_API void sf_hardware_engine_nexus_t::init(const sf_hardware_topology_t* topo) {
    if (topo) {
        topology = *topo;
    } else {
        topology = sf_detect_hardware_topology();
    }
    hybrid.init(&topology);
    numa.init(&topology);
    smt.init(&topology);
    sram.init(&topology);
    focus.init(&topology, &hybrid, &sram);
    initialized.store(true, std::memory_order_release);
}

MSIMD_API void sf_hardware_engine_nexus_t::cleanup() {
    sram.cleanup();
    initialized.store(false, std::memory_order_release);
}

/* ========================================================================= */
/* 12.9 CPU ISA INVENTORY & DYNAMIC DISCOVERY                                */
/* ========================================================================= */

bool sf_cpu_isa_inventory_t::has_isa(const char* token) const {
    if (!token || !*token) return false;
    if (_stricmp(token, "AUTO") == 0 || _stricmp(token, "ANY") == 0 || _stricmp(token, "SCALAR") == 0) {
        return true;
    }
    for (const auto& item : discovered_isas) {
        if (_stricmp(item.c_str(), token) == 0) return true;
    }
    return false;
}

void sf_cpu_isa_inventory_t::add_isa(const char* token) {
    if (!token || !*token || has_isa(token)) return;
    discovered_isas.emplace_back(token);
}

std::string sf_cpu_isa_inventory_t::resolve_fallback(const char* requested_isa) const {
    if (!requested_isa || !*requested_isa) return "SCALAR";
    if (has_isa(requested_isa)) return requested_isa;

    const char* cur = requested_isa;
    while (cur && _stricmp(cur, "SCALAR") != 0) {
        const auto* e = sf_find_world_isa(cur);
        if (!e || !e->fallback_isa) break;
        cur = e->fallback_isa;
        if (has_isa(cur)) return cur;
    }
    return "SCALAR";
}

void sf_cpu_isa_inventory_t::parse_cpu_string(const char* text) {
    if (!text || !*text) return;
    const char* p = text;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',' || *p == ';' || *p == ':')) {
            ++p;
        }
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != ',' && *p != ';' && *p != ':') {
            ++p;
        }
        std::string tok(start, p - start);
        if (!tok.empty()) {
            const auto* matched = sf_find_world_isa(tok.c_str());
            if (matched) {
                add_isa(matched->isa_name);
            }
            else {
                if (_stricmp(tok.c_str(), "fma") == 0) add_isa("FMA3");
                else if (_stricmp(tok.c_str(), "avx512_vnni") == 0) add_isa("AVX512VNNI");
                else if (_stricmp(tok.c_str(), "avx512_bf16") == 0) add_isa("AVX512_BF16");
                else if (_stricmp(tok.c_str(), "avx512_bitalg") == 0) add_isa("AVX512BITALG");
                else if (_stricmp(tok.c_str(), "avx512_vbmi") == 0) add_isa("AVX512VBMI");
                else if (_stricmp(tok.c_str(), "avx512_vbmi2") == 0) add_isa("AVX512VBMI2");
                else if (_stricmp(tok.c_str(), "avx512_vpopcntdq") == 0) add_isa("AVX512VPOPCNTDQ");
                else if (_stricmp(tok.c_str(), "asimd") == 0) add_isa("NEON");
                else if (_stricmp(tok.c_str(), "asimddp") == 0) add_isa("ARM_DOTPROD");
                else if (_stricmp(tok.c_str(), "asimdhp") == 0) add_isa("ARM_FP16");
                else if (_stricmp(tok.c_str(), "atomics") == 0) add_isa("ARM_LSE_ATOMICS");
            }
        }
    }
}

void sf_cpu_isa_inventory_t::build_description() {
    std::string s = "Discovered ISAs (" + std::to_string(discovered_isas.size()) + "): [";
    for (size_t i = 0; i < discovered_isas.size(); ++i) {
        if (i > 0) s += ", ";
        s += discovered_isas[i];
    }
    s += "]\n";
    s += topology.summary;
    strncpy(description, s.c_str(), sizeof(description) - 1);
}

sf_cpu_isa_inventory_t sf_cpu_isa_inventory_t::discover(void) {
    sf_cpu_isa_inventory_t inv;

#if defined(_M_X64) || defined(__x86_64__)
    int info[4] = { 0 };
    sf_get_cpuid(info, 0, 0);
    int max_leaf = info[0];

    if (max_leaf >= 1) {
        sf_get_cpuid(info, 1, 0);
        if (info[3] & (1 << 23)) inv.add_isa("MMX");
        if (info[3] & (1 << 25)) inv.add_isa("SSE");
        if (info[3] & (1 << 26)) inv.add_isa("SSE2");
        if (info[2] & (1 << 0))  inv.add_isa("SSE3");
        if (info[2] & (1 << 9))  inv.add_isa("SSSE3");
        if (info[2] & (1 << 19)) inv.add_isa("SSE4_1");
        if (info[2] & (1 << 20)) inv.add_isa("SSE4_2");
        if (info[2] & (1 << 12)) inv.add_isa("FMA3");
        if (info[2] & (1 << 28)) inv.add_isa("AVX");
        if (info[2] & (1 << 29)) inv.add_isa("F16C");

        uint32_t model = ((info[0] >> 4) & 0xF) | (((info[0] >> 16) & 0xF) << 4);
        uint32_t family = ((info[0] >> 8) & 0xF);
        if (family == 6) {
            if (model == 0x37 || model == 0x4C || model == 0x5C || model == 0x7A || model == 0x86 || model == 0x96 || model == 0xBE) {
                inv.add_isa("INTEL_ATOM_LOW_POWER");
                inv.add_isa("STREAMING_NT_FAVORED");
            }
        }
    }

    if (max_leaf >= 7) {
        sf_get_cpuid(info, 7, 0);
        if (info[1] & (1 << 3))  inv.add_isa("BMI1");
        if (info[1] & (1 << 5))  inv.add_isa("AVX2");
        if (info[1] & (1 << 8))  inv.add_isa("BMI2");
        if (info[1] & (1 << 16)) inv.add_isa("AVX512F");
        if (info[1] & (1 << 17)) inv.add_isa("AVX512DQ");
        if (info[1] & (1 << 28)) inv.add_isa("AVX512CD");
        if (info[1] & (1 << 30)) inv.add_isa("AVX512BW");
        if (info[1] & (1 << 31)) inv.add_isa("AVX512VL");
        if (info[2] & (1 << 1))  inv.add_isa("AVX512VBMI");
        if (info[2] & (1 << 6))  inv.add_isa("AVX512VBMI2");
        if (info[2] & (1 << 11)) inv.add_isa("AVX512VNNI");
        if (info[2] & (1 << 12)) inv.add_isa("AVX512BITALG");
        if (info[2] & (1 << 14)) inv.add_isa("AVX512VPOPCNTDQ");
        if (info[3] & (1 << 22)) inv.add_isa("AMX_BF16");
        if (info[3] & (1 << 23)) inv.add_isa("AVX512FP16");
        if (info[3] & (1 << 24)) inv.add_isa("AMX_TILE");
        if (info[3] & (1 << 25)) inv.add_isa("AMX_INT8");

        sf_get_cpuid(info, 7, 1);
        if (info[0] & (1 << 4))  inv.add_isa("AVX_VNNI");
        if (info[0] & (1 << 5))  inv.add_isa("AVX512_BF16");
        if (info[3] & (1 << 19)) inv.add_isa("AVX10");
        if (info[3] & (1 << 21)) inv.add_isa("APX_F");
    }

    sf_get_cpuid(info, 0x80000000, 0);
    unsigned int max_ext = (unsigned int)info[0];
    if (max_ext >= 0x80000001) {
        sf_get_cpuid(info, 0x80000001, 0);
        if (info[2] & (1 << 6))  inv.add_isa("SSE4A");
        if (info[2] & (1 << 11)) inv.add_isa("XOP");
        if (info[2] & (1 << 16)) inv.add_isa("FMA4");
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    inv.add_isa("NEON");
    inv.add_isa("ASIMD");
#  if defined(__linux__) && defined(__aarch64__)
#    if defined(AT_HWCAP)
    unsigned long hwc = getauxval(AT_HWCAP);
    if (hwc & HWCAP_FP)       inv.add_isa("ARM_FP");
    if (hwc & HWCAP_ASIMD)    inv.add_isa("ARM_ASIMD");
#      if defined(HWCAP_FPHP)
    if (hwc & HWCAP_FPHP)     inv.add_isa("ARM_FP16");
#      endif
#      if defined(HWCAP_ASIMDDP)
    if (hwc & HWCAP_ASIMDDP)  inv.add_isa("ARM_DOTPROD");
#      endif
#      if defined(HWCAP_SVE)
    if (hwc & HWCAP_SVE)      inv.add_isa("ARM_SVE");
#      endif
#      if defined(HWCAP_ATOMICS)
    if (hwc & HWCAP_ATOMICS)  inv.add_isa("ARM_LSE_ATOMICS");
#      endif
#    endif
#  endif
#  if defined(__APPLE__)
    inv.add_isa("APPLE_SILICON_ARM");
    inv.add_isa("APPLE_128B_CACHELINE");
    inv.add_isa("APPLE_UMA_ZEROCOPY");
#  endif
#  if defined(__ARM_FEATURE_SVE)
    inv.add_isa("SVE");
#  endif
#  if defined(__ARM_FEATURE_SVE2)
    inv.add_isa("SVE2");
#  endif
#  if defined(__ARM_FEATURE_MATMUL_INT8)
    inv.add_isa("SME");
#  endif
#elif defined(__riscv)
    inv.add_isa("RV64");
#  if defined(__riscv_vector)
    inv.add_isa("RVV");
#  endif
#elif defined(__s390x__) || defined(__s390__)
    inv.add_isa("S390X");
#endif

#if defined(_WIN32)
    if (IsProcessorFeaturePresent(PF_MMX_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_MMX");
    if (IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_SSE");
    if (IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_SSE2");
    if (IsProcessorFeaturePresent(PF_SSE3_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_SSE3");
    if (IsProcessorFeaturePresent(PF_ARM_V8_INSTRUCTIONS_AVAILABLE)) inv.add_isa("ARM_V8");
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) inv.add_isa("ARM_CRYPTO");
#  if defined(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)
    if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) inv.add_isa("ARM_V81");
#  endif
#  if defined(PF_AVX_INSTRUCTIONS_AVAILABLE)
    if (IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_AVX");
#  endif
#  if defined(PF_AVX2_INSTRUCTIONS_AVAILABLE)
    if (IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_AVX2");
#  endif
#  if defined(PF_AVX512F_INSTRUCTIONS_AVAILABLE)
    if (IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE)) inv.add_isa("PF_AVX512F");
#  endif
#endif

    const char* custom_isas = getenv("SF_EXTRA_ISAS");
    if (custom_isas) {
        std::string cur = "";
        for (const char* p = custom_isas; *p; ++p) {
            if (*p == ',' || *p == ';' || *p == ' ' || *p == '|') {
                if (!cur.empty()) { inv.add_isa(cur.c_str()); cur.clear(); }
            }
            else {
                cur += *p;
            }
        }
        if (!cur.empty()) inv.add_isa(cur.c_str());
    }

    const char* text_override = getenv("SF_CPU_INFO_TEXT");
    if (text_override) {
        inv.parse_cpu_string(text_override);
    }

    inv.topology = sf_detect_hardware_topology(inv.has_isa("APPLE_SILICON_ARM") ? "APPLE" : (inv.has_isa("NVIDIA_TEGRA_UMA_ZEROCOPY") ? "TEGRA" : (inv.has_isa("BROADCOM_RPI_ARM") ? "BROADCOM" : "")));
    inv.build_description();
    sf_init_hardware_nexus(&inv.topology);
    return inv;
}

/* ========================================================================= */
/* 13. CANONICAL C APIS & EXPORTS                                            */
/* ========================================================================= */

extern "C" {

MSIMD_API sf_hardware_engine_nexus_t* sf_get_hardware_nexus(void) {
    if (!g_sf_hardware_nexus.initialized.load(std::memory_order_acquire)) {
        g_sf_hardware_nexus.init();
    }
    return &g_sf_hardware_nexus;
}

MSIMD_API void sf_init_hardware_nexus(const sf_hardware_topology_t* topo) {
    g_sf_hardware_nexus.init(topo);
}

MSIMD_API bool sf_bind_current_thread(SF_custominiVm_core_tier_t tier) {
    return sf_get_hardware_nexus()->hybrid.bind_current_thread(tier);
}

MSIMD_API bool sf_is_hybrid_cpu(void) {
    return sf_get_hardware_nexus()->hybrid.is_hybrid;
}

MSIMD_API bool sf_is_focus_scp(void) {
    return sf_get_hardware_nexus()->focus.is_scp;
}

MSIMD_API uint32_t sf_get_p_core_count(void) {
    return sf_get_hardware_nexus()->hybrid.p_core_count;
}

MSIMD_API uint32_t sf_get_e_core_count(void) {
    return sf_get_hardware_nexus()->hybrid.e_core_count;
}

MSIMD_API uint32_t sf_get_sram_l1d_kb(void) {
    return sf_get_hardware_nexus()->sram.l1d_kb;
}

MSIMD_API uint32_t sf_get_sram_l2_kb(void) {
    return sf_get_hardware_nexus()->sram.l2_kb;
}

MSIMD_API uint32_t sf_get_sram_l3_kb(void) {
    return sf_get_hardware_nexus()->sram.l3_kb;
}

MSIMD_API uint8_t sf_get_numa_node_count(void) {
    return sf_get_hardware_nexus()->numa.node_count;
}

MSIMD_API bool sf_is_uma_system(void) {
    return sf_get_hardware_nexus()->numa.is_uma;
}

MSIMD_API void sf_upclocker_set_boost(int enable) {
    sf_get_hardware_nexus()->upclocker.enable(enable != 0);
}

MSIMD_API int sf_upclocker_is_boosted(void) {
    return sf_get_hardware_nexus()->upclocker.is_boosted.load(std::memory_order_relaxed) ? 1 : 0;
}

MSIMD_API const SF_custominiVm_hardware_topology_t* SF_custominiVm_hardware_topology(void) {
    return &sf_get_hardware_nexus()->topology;
}

MSIMD_API int SF_custominiVm_cpu_numa_nodes(void) {
    return sf_get_hardware_nexus()->numa.node_count;
}

MSIMD_API void SF_custominiVm_upclocker_set_boost(int enable) {
    sf_upclocker_set_boost(enable);
}

MSIMD_API int SF_custominiVm_upclocker_is_boosted(void) {
    return sf_upclocker_is_boosted();
}

MSIMD_API void SF_custominiVm_upclocker_warmup(uint32_t duration_us) {
    sf_upclocker_t::pin_max_frequency_warmup(duration_us > 0 ? duration_us : 50);
}

MSIMD_API int SF_custominiVm_is_hybrid_cpu(void) {
    return sf_is_hybrid_cpu() ? 1 : 0;
}

MSIMD_API int SF_custominiVm_is_numa(void) {
    return sf_is_uma_system() ? 0 : 1;
}

MSIMD_API SF_custominiVm_smt_mode_t SF_custominiVm_smt_mode(void) {
    return sf_get_hardware_nexus()->smt.smt_mode;
}

MSIMD_API SF_custominiVm_sram_regime_t SF_custominiVm_sram_regime(void) {
    return sf_get_hardware_nexus()->sram.sram_regime;
}

MSIMD_API int SF_custominiVm_bind_calling_thread(SF_custominiVm_core_tier_t tier) {
    return sf_bind_current_thread(tier) ? 1 : 0;
}

MSIMD_API void* SF_custominiVm_sram_scratchpad_alloc(size_t bytes, size_t alignment) {
    return sf_get_hardware_nexus()->sram.scratchpad_alloc(bytes, alignment);
}

MSIMD_API void SF_custominiVm_sram_scratchpad_reset(void) {
    sf_get_hardware_nexus()->sram.scratchpad_reset();
}

MSIMD_API void* SF_custominiVm_numa_alloc(size_t bytes, uint8_t node) {
    return sf_get_hardware_nexus()->numa.alloc_node(bytes, node);
}

MSIMD_API void SF_custominiVm_numa_free(void* ptr, size_t bytes, uint8_t node) {
    sf_get_hardware_nexus()->numa.free_node(ptr, bytes, node);
}

MSIMD_API SF_custominiVm_execution_focus_t SF_custominiVm_get_execution_focus(void) {
    return sf_get_hardware_nexus()->focus.active_focus.load(std::memory_order_acquire);
}

MSIMD_API void SF_custominiVm_set_execution_focus(SF_custominiVm_execution_focus_t focus) {
    sf_get_hardware_nexus()->focus.set_focus(focus);
}

MSIMD_API const char* SF_custominiVm_get_execution_focus_name(SF_custominiVm_execution_focus_t focus) {
    switch (focus) {
    case SF_FOCUS_AUTO:          return "AUTO";
    case SF_FOCUS_SCP_HOMO:      return "SCP_HOMO";
    case SF_FOCUS_SCP_HYBRID:    return "SCP_HYBRID";
    case SF_FOCUS_MCP_HOMO:      return "MCP_HOMO";
    case SF_FOCUS_MCP_HYBRID:    return "MCP_HYBRID";
    case SF_FOCUS_DUAL_HOMO:     return "DUAL_HOMO";
    case SF_FOCUS_DUAL_HYBRID:   return "DUAL_HYBRID";
    case SF_FOCUS_SCP:           return "SCP";
    case SF_FOCUS_MCP:           return "MCP";
    case SF_FOCUS_HYBRID_DUAL:   return "DUAL";
    default:                     return "UNKNOWN";
    }
}

MSIMD_API SF_custominiVm_execution_focus_t SF_custominiVm_detect_cpu_execution_focus(void) {
    return sf_get_hardware_nexus()->focus.detect_focus(&sf_get_hardware_nexus()->topology, &sf_get_hardware_nexus()->hybrid);
}

MSIMD_API const char* SF_custominiVm_get_focus_uarch_name(void) {
    if (sf_get_hardware_nexus()->focus.matched_uarch) {
        return sf_get_hardware_nexus()->focus.matched_uarch->uarch_name;
    }
    return "Generic x86_64 / ARM / RISC-V";
}

MSIMD_API void SF_custominiVm_get_focus_chunk_weights(double* out_p_weight, double* out_e_weight) {
    if (out_p_weight) *out_p_weight = sf_get_hardware_nexus()->focus.p_weight;
    if (out_e_weight) *out_e_weight = sf_get_hardware_nexus()->focus.e_weight;
}

MSIMD_API int SF_custominiVm_is_focus_scp(void) {
    return sf_is_focus_scp() ? 1 : 0;
}

MSIMD_API int SF_custominiVm_is_focus_mcp(void) {
    return sf_get_hardware_nexus()->focus.is_mcp ? 1 : 0;
}

MSIMD_API int SF_custominiVm_is_focus_dual(void) {
    return sf_get_hardware_nexus()->focus.is_dual ? 1 : 0;
}

MSIMD_API SF_custominiVm_hardware_tier_t SF_custominiVm_query_hardware_tier(void) {
    return sf_get_hardware_nexus()->smt.detect_hardware_tier();
}

MSIMD_API const char* SF_custominiVm_query_hardware_tier_name(void) {
    return sf_get_hardware_nexus()->smt.get_hardware_tier_name();
}

MSIMD_API void SF_custominiVm_query_thread_budget(uint32_t* out_gc, uint32_t* out_puzzle, uint32_t* out_vm) {
    auto b = sf_get_hardware_nexus()->smt.calculate_thread_budget();
    if (out_gc) *out_gc = b.gc_threads;
    if (out_puzzle) *out_puzzle = b.puzzle_threads;
    if (out_vm) *out_vm = b.vm_threads;
}

MSIMD_API void SF_custominiVm_sram_commit_to_dram(void* ptr, size_t bytes) {
    sf_sram_commit_to_dram(ptr, bytes);
}

MSIMD_API void SF_custominiVm_sram_broadcast_volatility(void* ptr, size_t bytes) {
    sf_sram_broadcast_volatility(ptr, bytes);
}

MSIMD_API void SF_custominiVm_dram_stream_store(void* dst, const void* src, size_t bytes) {
    sf_dram_stream_store(dst, src, bytes);
}

MSIMD_API void SF_custominiVm_pmu_sample_begin(void) {
    tl_sf_pmu_session.begin();
}

MSIMD_API void SF_custominiVm_pmu_sample_end(SF_custominiVm_pmu_stat_t* out_stat, size_t bytes_processed) {
    tl_sf_pmu_session.end(out_stat, bytes_processed);
}

MSIMD_API void* sf_numa_alloc(size_t size, uint32_t numa_node) {
    return SF_custominiVm_numa_alloc(size, (uint8_t)numa_node);
}

MSIMD_API void sf_numa_free(void* ptr, size_t size) {
    SF_custominiVm_numa_free(ptr, size, 0);
}

MSIMD_API void* msimd_numa_alloc(size_t size, uint32_t numa_node) {
    return SF_custominiVm_numa_alloc(size, (uint8_t)numa_node);
}

MSIMD_API void msimd_numa_free(void* ptr, size_t size) {
    SF_custominiVm_numa_free(ptr, size, 0);
}

MSIMD_API int sf_thread_pin_to_numa_node(uint32_t numa_node) {
    (void)numa_node;
    return 0;
}

MSIMD_API int msimd_thread_pin_to_numa_node(uint32_t numa_node) {
    (void)numa_node;
    return 0;
}

MSIMD_API int SF_custominiVm_thread_pin_to_numa_node(uint32_t numa_node) {
    (void)numa_node;
    return 0;
}

} // extern "C"
