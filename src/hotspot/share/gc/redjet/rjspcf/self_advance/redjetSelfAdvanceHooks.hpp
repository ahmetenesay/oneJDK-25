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

#ifndef SHARE_GC_REDJET_RJSPCF_SELF_ADVANCE_REDJETSELFADVANCEHOOKS_HPP
#define SHARE_GC_REDJET_RJSPCF_SELF_ADVANCE_REDJETSELFADVANCEHOOKS_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/redjetHardware.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. SELF-ADVANCEMENT TELEMETRY EVENT RING BUFFER                           */
/* ========================================================================= */

constexpr uint32_t SF_EVENT_GC_HOTSWAP           = 0x01;
constexpr uint32_t SF_EVENT_RMRP_TRANSITION      = 0x02;
constexpr uint32_t SF_EVENT_AFFINITY_MIGRATION   = 0x03;
constexpr uint32_t SF_EVENT_LEAK_ALERT           = 0x04;
constexpr uint32_t SF_EVENT_BITMAP_EVOLVED       = 0x05;
constexpr uint32_t SF_EVENT_ROLLBACK             = 0x06;

struct sf_self_advance_event_entry_t {
    uint64_t timestamp_tsc{ 0 };
    uint32_t event_type{ 0 };
    char     tag[32]{ 0 };
    char     message[96]{ 0 };
    double   value_before{ 0.0 };
    double   value_after{ 0.0 };
};

class RedjetSelfAdvanceTelemetry {
private:
    static constexpr size_t RING_CAPACITY = 1024;
    sf_self_advance_event_entry_t _ring[RING_CAPACITY];
    std::atomic<uint64_t>         _write_idx{ 0 };
    mutable std::mutex            _lock;

public:
    RedjetSelfAdvanceTelemetry();
    ~RedjetSelfAdvanceTelemetry() = default;

    void record_event(uint32_t type, const char* tag, const char* msg,
                      double before = 0.0, double after = 0.0);
    void print_summary(FILE* out = stdout) const;
    size_t count() const;
    void reset();
};

/* ========================================================================= */
/* 2. HOOKPOINT TSC CYCLE COUNTER & PROFILER                                 */
/* ========================================================================= */

struct RedjetTSCHookRecord {
    uint32_t hook_id{ 0 };
    uint64_t enter_tsc{ 0 };
    uint64_t exit_tsc{ 0 };
    uint64_t duration_cycles{ 0 };
    uint64_t duration_ns{ 0 };
};

class RedjetTSCHookCounter {
public:
    static constexpr size_t MAX_HOOK_POINTS = 16;

    struct HookMetrics {
        std::atomic<uint64_t> invocations{ 0 };
        std::atomic<uint64_t> total_cycles{ 0 };
        std::atomic<uint64_t> min_cycles{ UINT64_MAX };
        std::atomic<uint64_t> max_cycles{ 0 };
        std::atomic<uint64_t> last_duration_ns{ 0 };
    };

private:
    HookMetrics _metrics[MAX_HOOK_POINTS];
    uint32_t    _cpu_freq_mhz{ 3000 };

public:
    RedjetTSCHookCounter();

    void set_cpu_frequency_mhz(uint32_t mhz) noexcept {
        if (mhz > 0) _cpu_freq_mhz = mhz;
    }

    uint32_t cpu_frequency_mhz() const noexcept {
        return _cpu_freq_mhz;
    }

    inline uint64_t enter() const noexcept {
        return sacculus_rdtsc();
    }

    inline uint64_t exit(uint32_t hook_id, uint64_t enter_tsc) noexcept {
        uint64_t exit_tsc = sacculus_rdtsc();
        uint64_t cycles = (exit_tsc >= enter_tsc) ? (exit_tsc - enter_tsc) : 0;
        record(hook_id, cycles);
        return cycles;
    }

    void record(uint32_t hook_id, uint64_t cycles) noexcept;
    uint64_t cycles_to_ns(uint64_t cycles) const noexcept;
    uint64_t get_average_cycles(uint32_t hook_id) const noexcept;
    uint64_t get_total_invocations(uint32_t hook_id) const noexcept;
    uint64_t get_min_cycles(uint32_t hook_id) const noexcept;
    uint64_t get_max_cycles(uint32_t hook_id) const noexcept;
    void reset() noexcept;
};

/* ========================================================================= */
/* 3. TLAB & FALLINE LEAK AND ALLOCATION ANALYZER                            */
/* ========================================================================= */

struct RedjetThreadAllocStats {
    uint64_t thread_id{ 0 };
    uint64_t total_allocated_bytes{ 0 };
    uint64_t total_allocations{ 0 };
    uint64_t total_freed_bytes{ 0 };
    uint64_t falline_snatches{ 0 };
    uint64_t falline_misses{ 0 };
    uint64_t size_bin_under_64{ 0 };
    uint64_t size_bin_64_256{ 0 };
    uint64_t size_bin_256_1k{ 0 };
    uint64_t size_bin_1k_4k{ 0 };
    uint64_t size_bin_over_4k{ 0 };
    double   allocation_rate_mbps{ 0.0 };
    bool     possible_leak{ false };
};

class RedjetTLABFallineAnalyzer {
public:
    static constexpr size_t MAX_TRACKED_THREADS = 64;

    struct alignas(64) ThreadProfileSlot {
        std::atomic<uint64_t> thread_id{ 0 };
        std::atomic<uint64_t> total_allocated_bytes{ 0 };
        std::atomic<uint64_t> total_allocations{ 0 };
        std::atomic<uint64_t> total_freed_bytes{ 0 };
        std::atomic<uint64_t> falline_snatches{ 0 };
        std::atomic<uint64_t> falline_misses{ 0 };
        std::atomic<uint64_t> size_bin_under_64{ 0 };
        std::atomic<uint64_t> size_bin_64_256{ 0 };
        std::atomic<uint64_t> size_bin_256_1k{ 0 };
        std::atomic<uint64_t> size_bin_1k_4k{ 0 };
        std::atomic<uint64_t> size_bin_over_4k{ 0 };
        std::atomic<uint64_t> last_sample_tsc{ 0 };
        std::atomic<uint64_t> sample_allocated_bytes{ 0 };
        std::atomic<bool>     leak_alerted{ false };
        std::atomic<bool>     active{ false };
    };

private:
    ThreadProfileSlot _slots[MAX_TRACKED_THREADS];
    std::atomic<uint32_t> _active_thread_count{ 0 };
    std::atomic<uint64_t> _global_leak_alerts{ 0 };
    std::atomic<uint64_t> _overflow_alloc_bytes{ 0 };

    ThreadProfileSlot* find_or_assign_slot(uint64_t tid);

public:
    RedjetTLABFallineAnalyzer();

    void record_allocation(uint64_t tid, size_t bytes, bool from_falline);
    void record_deallocation(uint64_t tid, size_t bytes);
    void unregister_thread(uint64_t tid);
    void evaluate_leak_heuristics();
    bool query_thread_stats(uint64_t tid, RedjetThreadAllocStats* out_stats);
    uint32_t query_all_active(RedjetThreadAllocStats* out_array, uint32_t max_count);
    uint64_t get_leak_alert_count() const noexcept {
        return _global_leak_alerts.load(std::memory_order_relaxed);
    }
    uint32_t active_thread_count() const noexcept {
        return _active_thread_count.load(std::memory_order_relaxed);
    }
    void reset();
};

/* ========================================================================= */
/* 4. SRAM & CACHE PRESSURE TRACKER                                          */
/* ========================================================================= */

struct RedjetCachePressureMetrics {
    double   l1d_miss_rate{ 0.0 };
    double   l2_miss_rate{ 0.0 };
    double   l3_miss_rate{ 0.0 };
    double   instructions_per_cycle{ 0.0 };
    uint64_t total_cycles{ 0 };
    uint64_t total_instructions{ 0 };
    uint64_t l1d_misses{ 0 };
    uint64_t llc_misses{ 0 };
    bool     high_cache_pressure{ false };
    bool     bus_bandwidth_saturated{ false };
};

class RedjetCachePressureTracker {
private:
    std::atomic<uint64_t> _sample_count{ 0 };
    std::atomic<uint64_t> _accum_cycles{ 0 };
    std::atomic<uint64_t> _accum_instructions{ 0 };
    std::atomic<uint64_t> _accum_l1d_misses{ 0 };
    std::atomic<uint64_t> _accum_llc_misses{ 0 };
    std::atomic<bool>     _high_pressure_flag{ false };
    std::atomic<bool>     _bus_saturated_flag{ false };

    double _l1d_pressure_threshold{ 0.15 };  // >15% miss rate = high L1D pressure
    double _llc_pressure_threshold{ 0.05 };  // >5% miss rate = high LLC pressure

public:
    RedjetCachePressureTracker();

    void record_pmu_sample(const SF_custominiVm_pmu_stat_t& stat);
    void record_manual_sample(uint64_t cycles, uint64_t instrs, uint64_t l1d_miss, uint64_t llc_miss);
    RedjetCachePressureMetrics query_metrics() const;
    bool is_high_cache_pressure() const noexcept {
        return _high_pressure_flag.load(std::memory_order_relaxed);
    }
    bool is_bus_saturated() const noexcept {
        return _bus_saturated_flag.load(std::memory_order_relaxed);
    }
    void reset();
};

/* ========================================================================= */
/* 5. GLOBAL ACCESSORS & C EXPORTS                                           */
/* ========================================================================= */

extern RedjetTSCHookCounter       g_redjet_tsc_hook_counter;
extern RedjetTLABFallineAnalyzer  g_redjet_falline_analyzer;
extern RedjetCachePressureTracker g_redjet_cache_tracker;
extern RedjetSelfAdvanceTelemetry g_redjet_self_advance_telemetry;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API uint64_t SF_self_advance_hook_enter(uint32_t hook_id);
MSIMD_API uint64_t SF_self_advance_hook_exit(uint32_t hook_id, uint64_t enter_tsc);
MSIMD_API void     SF_self_advance_record_alloc(uint64_t tid, size_t bytes, int from_falline);
MSIMD_API void     SF_self_advance_record_free(uint64_t tid, size_t bytes);
MSIMD_API void     SF_self_advance_unregister_thread(uint64_t tid);
MSIMD_API int      SF_self_advance_query_cache_pressure(RedjetCachePressureMetrics* out_metrics);
MSIMD_API uint64_t SF_self_advance_get_leak_alerts(void);
MSIMD_API void     SF_self_advance_record_event(uint32_t type, const char* tag, const char* msg,
                                                double before, double after);
MSIMD_API size_t   SF_self_advance_telemetry_count(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_RJSPCF_SELF_ADVANCE_REDJETSELFADVANCEHOOKS_HPP