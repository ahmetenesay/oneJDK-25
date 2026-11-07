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

#include "gc/redjet/puzzler/redjetSelfAdvanceHooks.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#define log_warning(...) if (false) printf
#define log_error(...) if (false) printf
#endif

RedjetTSCHookCounter       g_redjet_tsc_hook_counter;
RedjetTLABFallineAnalyzer  g_redjet_falline_analyzer;
RedjetCachePressureTracker g_redjet_cache_tracker;
RedjetSelfAdvanceTelemetry g_redjet_self_advance_telemetry;

/* ========================================================================= */
/* 1. RedjetSelfAdvanceTelemetry                                             */
/* ========================================================================= */

RedjetSelfAdvanceTelemetry::RedjetSelfAdvanceTelemetry() {
    reset();
}

void RedjetSelfAdvanceTelemetry::record_event(uint32_t type, const char* tag, const char* msg,
                                              double before, double after) {
    std::lock_guard<std::mutex> guard(_lock);
    uint64_t idx = _write_idx.fetch_add(1, std::memory_order_relaxed);
    size_t slot = static_cast<size_t>(idx % RING_CAPACITY);

    auto& entry = _ring[slot];
    entry.timestamp_tsc = sacculus_rdtsc();
    entry.event_type = type;
    entry.value_before = before;
    entry.value_after = after;

    if (tag) {
        strncpy(entry.tag, tag, sizeof(entry.tag) - 1);
        entry.tag[sizeof(entry.tag) - 1] = '\0';
    } else {
        entry.tag[0] = '\0';
    }

    if (msg) {
        strncpy(entry.message, msg, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';
    } else {
        entry.message[0] = '\0';
    }
}

void RedjetSelfAdvanceTelemetry::print_summary(FILE* out) const {
    if (!out) out = stdout;
    std::lock_guard<std::mutex> guard(_lock);
    uint64_t total = _write_idx.load(std::memory_order_relaxed);
    size_t count = std::min(static_cast<size_t>(total), RING_CAPACITY);

    fprintf(out, "=== RedjetGC Self-Advancement Telemetry Log (Total Events: %llu) ===\n",
            static_cast<unsigned long long>(total));
    for (size_t i = 0; i < count; ++i) {
        const auto& e = _ring[i];
        if (e.timestamp_tsc == 0) continue;
        fprintf(out, "  [Event 0x%02X] [%s] %s (before=%.2f, after=%.2f)\n",
                e.event_type, e.tag, e.message, e.value_before, e.value_after);
    }
}

size_t RedjetSelfAdvanceTelemetry::count() const {
    uint64_t total = _write_idx.load(std::memory_order_relaxed);
    return std::min(static_cast<size_t>(total), RING_CAPACITY);
}

void RedjetSelfAdvanceTelemetry::reset() {
    std::lock_guard<std::mutex> guard(_lock);
    _write_idx.store(0, std::memory_order_relaxed);
    std::memset(_ring, 0, sizeof(_ring));
}

/* ========================================================================= */
/* 2. RedjetTSCHookCounter                                                   */
/* ========================================================================= */

RedjetTSCHookCounter::RedjetTSCHookCounter() {
    reset();
    const auto* topo = HS_hotspot_hardware_topology();
    if (topo && topo->cpu_cur_freq_mhz > 0) {
        _cpu_freq_mhz = topo->cpu_cur_freq_mhz;
    }
}

void RedjetTSCHookCounter::record(uint32_t hook_id, uint64_t cycles) noexcept {
    if (hook_id >= MAX_HOOK_POINTS) return;
    auto& m = _metrics[hook_id];
    m.invocations.fetch_add(1, std::memory_order_relaxed);
    m.total_cycles.fetch_add(cycles, std::memory_order_relaxed);

    uint64_t cur_min = m.min_cycles.load(std::memory_order_relaxed);
    while (cycles < cur_min && !m.min_cycles.compare_exchange_weak(cur_min, cycles, std::memory_order_relaxed)) {}

    uint64_t cur_max = m.max_cycles.load(std::memory_order_relaxed);
    while (cycles > cur_max && !m.max_cycles.compare_exchange_weak(cur_max, cycles, std::memory_order_relaxed)) {}

    uint64_t ns = cycles_to_ns(cycles);
    m.last_duration_ns.store(ns, std::memory_order_relaxed);
}

uint64_t RedjetTSCHookCounter::cycles_to_ns(uint64_t cycles) const noexcept {
    if (_cpu_freq_mhz == 0) return cycles;
    return (cycles * 1000ULL) / static_cast<uint64_t>(_cpu_freq_mhz);
}

uint64_t RedjetTSCHookCounter::get_average_cycles(uint32_t hook_id) const noexcept {
    if (hook_id >= MAX_HOOK_POINTS) return 0;
    const auto& m = _metrics[hook_id];
    uint64_t cnt = m.invocations.load(std::memory_order_relaxed);
    if (cnt == 0) return 0;
    return m.total_cycles.load(std::memory_order_relaxed) / cnt;
}

uint64_t RedjetTSCHookCounter::get_total_invocations(uint32_t hook_id) const noexcept {
    if (hook_id >= MAX_HOOK_POINTS) return 0;
    return _metrics[hook_id].invocations.load(std::memory_order_relaxed);
}

uint64_t RedjetTSCHookCounter::get_min_cycles(uint32_t hook_id) const noexcept {
    if (hook_id >= MAX_HOOK_POINTS) return 0;
    uint64_t val = _metrics[hook_id].min_cycles.load(std::memory_order_relaxed);
    return (val == UINT64_MAX) ? 0 : val;
}

uint64_t RedjetTSCHookCounter::get_max_cycles(uint32_t hook_id) const noexcept {
    if (hook_id >= MAX_HOOK_POINTS) return 0;
    return _metrics[hook_id].max_cycles.load(std::memory_order_relaxed);
}

void RedjetTSCHookCounter::reset() noexcept {
    for (size_t i = 0; i < MAX_HOOK_POINTS; ++i) {
        _metrics[i].invocations.store(0, std::memory_order_relaxed);
        _metrics[i].total_cycles.store(0, std::memory_order_relaxed);
        _metrics[i].min_cycles.store(UINT64_MAX, std::memory_order_relaxed);
        _metrics[i].max_cycles.store(0, std::memory_order_relaxed);
        _metrics[i].last_duration_ns.store(0, std::memory_order_relaxed);
    }
}

/* ========================================================================= */
/* 3. RedjetTLABFallineAnalyzer                                              */
/* ========================================================================= */

RedjetTLABFallineAnalyzer::RedjetTLABFallineAnalyzer() {
    reset();
}

RedjetTLABFallineAnalyzer::ThreadProfileSlot* RedjetTLABFallineAnalyzer::find_or_assign_slot(uint64_t tid) {
    if (tid == 0) return nullptr;

    // Fast path: find existing active slot for this thread
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        if (_slots[i].active.load(std::memory_order_acquire) &&
            _slots[i].thread_id.load(std::memory_order_relaxed) == tid) {
            return &_slots[i];
        }
    }

    // Slow path: claim an empty inactive slot
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        bool expected = false;
        if (_slots[i].active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            _slots[i].thread_id.store(tid, std::memory_order_release);
            _slots[i].last_sample_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
            _slots[i].leak_alerted.store(false, std::memory_order_relaxed);
            _active_thread_count.fetch_add(1, std::memory_order_relaxed);
            return &_slots[i];
        }
    }

    // Slot table capacity reached (64 active threads)
    return nullptr;
}

void RedjetTLABFallineAnalyzer::record_allocation(uint64_t tid, size_t bytes, bool from_falline) {
    auto* slot = find_or_assign_slot(tid);
    if (!slot) {
        _overflow_alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);
        return;
    }

    slot->total_allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    slot->total_allocations.fetch_add(1, std::memory_order_relaxed);
    slot->sample_allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);

    if (from_falline) {
        slot->falline_snatches.fetch_add(1, std::memory_order_relaxed);
    } else {
        slot->falline_misses.fetch_add(1, std::memory_order_relaxed);
    }

    // Update size distribution histogram
    if (bytes < 64) {
        slot->size_bin_under_64.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 256) {
        slot->size_bin_64_256.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 1024) {
        slot->size_bin_256_1k.fetch_add(1, std::memory_order_relaxed);
    } else if (bytes <= 4096) {
        slot->size_bin_1k_4k.fetch_add(1, std::memory_order_relaxed);
    } else {
        slot->size_bin_over_4k.fetch_add(1, std::memory_order_relaxed);
    }
}

void RedjetTLABFallineAnalyzer::record_deallocation(uint64_t tid, size_t bytes) {
    auto* slot = find_or_assign_slot(tid);
    if (!slot) return;
    slot->total_freed_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RedjetTLABFallineAnalyzer::unregister_thread(uint64_t tid) {
    if (tid == 0) return;
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        if (_slots[i].active.load(std::memory_order_acquire) &&
            _slots[i].thread_id.load(std::memory_order_relaxed) == tid) {
            _slots[i].active.store(false, std::memory_order_release);
            _slots[i].thread_id.store(0, std::memory_order_relaxed);
            uint32_t c = _active_thread_count.load(std::memory_order_relaxed);
            if (c > 0) _active_thread_count.fetch_sub(1, std::memory_order_relaxed);
            break;
        }
    }
}

void RedjetTLABFallineAnalyzer::evaluate_leak_heuristics() {
    uint64_t now_tsc = sacculus_rdtsc();
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        if (!_slots[i].active.load(std::memory_order_relaxed)) continue;

        uint64_t allocated = _slots[i].total_allocated_bytes.load(std::memory_order_relaxed);
        uint64_t freed = _slots[i].total_freed_bytes.load(std::memory_order_relaxed);

        // Leak indicator: thread allocated > 128 MB and freed < 5% of it, with high allocation count
        if (allocated > (128ULL * 1024 * 1024) && (freed * 20 < allocated)) {
            uint64_t count = _slots[i].total_allocations.load(std::memory_order_relaxed);
            if (count > 50000) {
                bool already = _slots[i].leak_alerted.exchange(true, std::memory_order_relaxed);
                if (!already) {
                    _global_leak_alerts.fetch_add(1, std::memory_order_relaxed);
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Possible memory leak on JavaThread 0x%llX (alloc=%llu MB, freed=%llu MB)",
                             static_cast<unsigned long long>(_slots[i].thread_id.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(allocated >> 20),
                             static_cast<unsigned long long>(freed >> 20));
                    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, 0, "falline_leak_alert", msg);
                    g_redjet_self_advance_telemetry.record_event(
                        HS_EVENT_LEAK_ALERT, "leak_alert", msg, static_cast<double>(allocated), static_cast<double>(freed));
                }
            }
        }
        _slots[i].last_sample_tsc.store(now_tsc, std::memory_order_relaxed);
    }
}

bool RedjetTLABFallineAnalyzer::query_thread_stats(uint64_t tid, RedjetThreadAllocStats* out_stats) {
    if (!out_stats || tid == 0) return false;
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        if (_slots[i].active.load(std::memory_order_relaxed) &&
            _slots[i].thread_id.load(std::memory_order_relaxed) == tid) {
            out_stats->thread_id = tid;
            out_stats->total_allocated_bytes = _slots[i].total_allocated_bytes.load(std::memory_order_relaxed);
            out_stats->total_allocations = _slots[i].total_allocations.load(std::memory_order_relaxed);
            out_stats->total_freed_bytes = _slots[i].total_freed_bytes.load(std::memory_order_relaxed);
            out_stats->falline_snatches = _slots[i].falline_snatches.load(std::memory_order_relaxed);
            out_stats->falline_misses = _slots[i].falline_misses.load(std::memory_order_relaxed);
            out_stats->size_bin_under_64 = _slots[i].size_bin_under_64.load(std::memory_order_relaxed);
            out_stats->size_bin_64_256 = _slots[i].size_bin_64_256.load(std::memory_order_relaxed);
            out_stats->size_bin_256_1k = _slots[i].size_bin_256_1k.load(std::memory_order_relaxed);
            out_stats->size_bin_1k_4k = _slots[i].size_bin_1k_4k.load(std::memory_order_relaxed);
            out_stats->size_bin_over_4k = _slots[i].size_bin_over_4k.load(std::memory_order_relaxed);
            out_stats->possible_leak = (out_stats->total_allocated_bytes > 64ULL * 1024 * 1024) &&
                                       (out_stats->total_freed_bytes * 10 < out_stats->total_allocated_bytes);
            return true;
        }
    }
    return false;
}

uint32_t RedjetTLABFallineAnalyzer::query_all_active(RedjetThreadAllocStats* out_array, uint32_t max_count) {
    if (!out_array || max_count == 0) return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < MAX_TRACKED_THREADS && count < max_count; ++i) {
        if (_slots[i].active.load(std::memory_order_relaxed)) {
            query_thread_stats(_slots[i].thread_id.load(std::memory_order_relaxed), &out_array[count]);
            count++;
        }
    }
    return count;
}

void RedjetTLABFallineAnalyzer::reset() {
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        _slots[i].thread_id.store(0, std::memory_order_relaxed);
        _slots[i].total_allocated_bytes.store(0, std::memory_order_relaxed);
        _slots[i].total_allocations.store(0, std::memory_order_relaxed);
        _slots[i].total_freed_bytes.store(0, std::memory_order_relaxed);
        _slots[i].falline_snatches.store(0, std::memory_order_relaxed);
        _slots[i].falline_misses.store(0, std::memory_order_relaxed);
        _slots[i].size_bin_under_64.store(0, std::memory_order_relaxed);
        _slots[i].size_bin_64_256.store(0, std::memory_order_relaxed);
        _slots[i].size_bin_256_1k.store(0, std::memory_order_relaxed);
        _slots[i].size_bin_1k_4k.store(0, std::memory_order_relaxed);
        _slots[i].size_bin_over_4k.store(0, std::memory_order_relaxed);
        _slots[i].last_sample_tsc.store(0, std::memory_order_relaxed);
        _slots[i].sample_allocated_bytes.store(0, std::memory_order_relaxed);
        _slots[i].leak_alerted.store(false, std::memory_order_relaxed);
        _slots[i].active.store(false, std::memory_order_relaxed);
    }
    _active_thread_count.store(0, std::memory_order_relaxed);
    _global_leak_alerts.store(0, std::memory_order_relaxed);
    _overflow_alloc_bytes.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 4. RedjetCachePressureTracker                                             */
/* ========================================================================= */

RedjetCachePressureTracker::RedjetCachePressureTracker() {
    reset();
}

void RedjetCachePressureTracker::record_pmu_sample(const HS_hotspot_pmu_stat_t& stat) {
    record_manual_sample(stat.cycles, stat.instructions, stat.l1d_cache_misses, stat.llc_cache_misses);
}

void RedjetCachePressureTracker::record_manual_sample(uint64_t cycles, uint64_t instrs, uint64_t l1d_miss, uint64_t llc_miss) {
    _sample_count.fetch_add(1, std::memory_order_relaxed);
    _accum_cycles.fetch_add(cycles, std::memory_order_relaxed);
    _accum_instructions.fetch_add(instrs, std::memory_order_relaxed);
    _accum_l1d_misses.fetch_add(l1d_miss, std::memory_order_relaxed);
    _accum_llc_misses.fetch_add(llc_miss, std::memory_order_relaxed);

    if (cycles > 0) {
        double l1d_ratio = static_cast<double>(l1d_miss) / static_cast<double>(cycles);
        double llc_ratio = static_cast<double>(llc_miss) / static_cast<double>(cycles);

        bool high_p = (l1d_ratio > _l1d_pressure_threshold) || (llc_ratio > _llc_pressure_threshold);
        _high_pressure_flag.store(high_p, std::memory_order_relaxed);

        bool bus_sat = (llc_ratio > 0.10);
        _bus_saturated_flag.store(bus_sat, std::memory_order_relaxed);
    }
}

RedjetCachePressureMetrics RedjetCachePressureTracker::query_metrics() const {
    RedjetCachePressureMetrics m{};
    uint64_t cycles = _accum_cycles.load(std::memory_order_relaxed);
    uint64_t instrs = _accum_instructions.load(std::memory_order_relaxed);
    uint64_t l1d = _accum_l1d_misses.load(std::memory_order_relaxed);
    uint64_t llc = _accum_llc_misses.load(std::memory_order_relaxed);

    m.total_cycles = cycles;
    m.total_instructions = instrs;
    m.l1d_misses = l1d;
    m.llc_misses = llc;

    if (cycles > 0) {
        m.instructions_per_cycle = static_cast<double>(instrs) / static_cast<double>(cycles);
        m.l1d_miss_rate = static_cast<double>(l1d) / static_cast<double>(cycles);
        m.l3_miss_rate = static_cast<double>(llc) / static_cast<double>(cycles);
        m.l2_miss_rate = (m.l1d_miss_rate + m.l3_miss_rate) * 0.5;
    }

    m.high_cache_pressure = _high_pressure_flag.load(std::memory_order_relaxed);
    m.bus_bandwidth_saturated = _bus_saturated_flag.load(std::memory_order_relaxed);
    return m;
}

void RedjetCachePressureTracker::reset() {
    _sample_count.store(0, std::memory_order_relaxed);
    _accum_cycles.store(0, std::memory_order_relaxed);
    _accum_instructions.store(0, std::memory_order_relaxed);
    _accum_l1d_misses.store(0, std::memory_order_relaxed);
    _accum_llc_misses.store(0, std::memory_order_relaxed);
    _high_pressure_flag.store(false, std::memory_order_relaxed);
    _bus_saturated_flag.store(false, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 5. C EXPORT APIS                                                          */
/* ========================================================================= */

RJ_API uint64_t HS_self_advance_hook_enter(uint32_t hook_id) {
    (void)hook_id;
    return g_redjet_tsc_hook_counter.enter();
}

RJ_API uint64_t HS_self_advance_hook_exit(uint32_t hook_id, uint64_t enter_tsc) {
    return g_redjet_tsc_hook_counter.exit(hook_id, enter_tsc);
}

RJ_API void HS_self_advance_record_alloc(uint64_t tid, size_t bytes, int from_falline) {
    g_redjet_falline_analyzer.record_allocation(tid, bytes, from_falline != 0);
}

RJ_API void HS_self_advance_record_free(uint64_t tid, size_t bytes) {
    g_redjet_falline_analyzer.record_deallocation(tid, bytes);
}

RJ_API void HS_self_advance_unregister_thread(uint64_t tid) {
    g_redjet_falline_analyzer.unregister_thread(tid);
}

RJ_API int HS_self_advance_query_cache_pressure(RedjetCachePressureMetrics* out_metrics) {
    if (!out_metrics) return 0;
    *out_metrics = g_redjet_cache_tracker.query_metrics();
    return 1;
}

RJ_API uint64_t HS_self_advance_get_leak_alerts(void) {
    return g_redjet_falline_analyzer.get_leak_alert_count();
}

RJ_API void HS_self_advance_record_event(uint32_t type, const char* tag, const char* msg,
                                            double before, double after) {
    g_redjet_self_advance_telemetry.record_event(type, tag, msg, before, after);
}

RJ_API size_t HS_self_advance_telemetry_count(void) {
    return g_redjet_self_advance_telemetry.count();
}
