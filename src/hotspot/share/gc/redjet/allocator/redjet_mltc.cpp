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

#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/allocator/redjet_rmrp.hpp"
#include "gc/redjet/allocator/redjetPageDefrag.hpp"
#include "gc/redjet/allocator/redjetStreamThrottle.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_warning(...) if (false) printf
#endif

hs_mltc_engine_t g_hs_mltc_solver;

/* ========================================================================= */
/* 1. REDJET MLTC PACER IMPLEMENTATION                                       */
/* ========================================================================= */

RedjetMLTCPacer::RedjetMLTCPacer() {
    _target_bandwidth_mbps.store(800.0f, std::memory_order_relaxed);
    _pacing_credit_pool.store(DEFAULT_BURST_ALLOWANCE_BYTES, std::memory_order_relaxed);
    _total_paced_allocations.store(0, std::memory_order_relaxed);
    _total_stall_delay_ns.store(0, std::memory_order_relaxed);
    _active_throttle_rate_percent.store(0, std::memory_order_relaxed);
    _last_replenish_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
}

bool RedjetMLTCPacer::should_pace_allocation(size_t requested_bytes, float current_velocity_mbps) noexcept {
    if (current_velocity_mbps > VELOCITY_PANIC_THRESHOLD_MBPS) {
        return true;
    }

    if (current_velocity_mbps > VELOCITY_THROTTLE_THRESHOLD_MBPS) {
        uint64_t credits = _pacing_credit_pool.load(std::memory_order_relaxed);
        if (credits < requested_bytes) {
            return true;
        }
        _pacing_credit_pool.fetch_sub(requested_bytes, std::memory_order_relaxed);
    }
    return false;
}

uint32_t RedjetMLTCPacer::compute_pace_delay_us(size_t requested_bytes, RedjetBusCongestionLevel level) noexcept {
    uint32_t base_us = 0;
    switch (level) {
        case RedjetBusCongestionLevel::CONGESTION_FREE:
            return 0;
        case RedjetBusCongestionLevel::MILD_CONTENTION:
            base_us = 2;
            break;
        case RedjetBusCongestionLevel::HIGH_CONTENTION:
            base_us = 8;
            break;
        case RedjetBusCongestionLevel::BUS_SATURATION:
            base_us = 25;
            break;
        case RedjetBusCongestionLevel::EMERGENCY_THROTTLE:
            base_us = 100;
            break;
    }

    // Scale delay proportionally to allocation size (per 64KB chunk)
    uint32_t scale = static_cast<uint32_t>(requested_bytes / (64 * 1024));
    if (scale > 10) scale = 10;
    return base_us * (1 + scale);
}

void RedjetMLTCPacer::apply_bus_pacing(size_t requested_bytes, uint32_t thread_id) {
    RedjetBusCongestionLevel level = g_hs_mltc_solver.current_congestion_level();
    if (level == RedjetBusCongestionLevel::CONGESTION_FREE) return;

    uint32_t delay_us = compute_pace_delay_us(requested_bytes, level);
    if (delay_us == 0) return;

    uint64_t start_tsc = sacculus_rdtsc();
    uint64_t target_cycles = static_cast<uint64_t>(delay_us) * 3000; // ~3 GHz CPU

    // Fine-grained hardware spinning
    while ((sacculus_rdtsc() - start_tsc) < target_cycles) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }

    uint64_t elapsed_cycles = sacculus_rdtsc() - start_tsc;
    uint64_t elapsed_ns = (elapsed_cycles * 10) / 30; // Approx ns

    _total_paced_allocations.fetch_add(1, std::memory_order_relaxed);
    _total_stall_delay_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

void RedjetMLTCPacer::replenish_credits(uint64_t elapsed_us, float target_mbps) {
    if (elapsed_us == 0) return;

    // Bytes per microsecond = MB/s
    uint64_t added_credits = static_cast<uint64_t>(target_mbps * static_cast<float>(elapsed_us));
    uint64_t cur = _pacing_credit_pool.load(std::memory_order_relaxed);
    uint64_t max_credits = DEFAULT_BURST_ALLOWANCE_BYTES;

    uint64_t next = cur + added_credits;
    if (next > max_credits) next = max_credits;

    _pacing_credit_pool.store(next, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. STAGNANT CYCLE LEAK DETECTOR IMPLEMENTATION                            */
/* ========================================================================= */

RedjetMLTCLeakDetector::RedjetMLTCLeakDetector()
    : _history_count(0), _history_head(0), _kalman_filter(0.0f, 0.02f, 0.15f) {
    _history_lock.next_ticket.store(0, std::memory_order_relaxed);
    _history_lock.now_serving.store(0, std::memory_order_relaxed);
    _consecutive_stagnant_cycles.store(0, std::memory_order_relaxed);
    _memory_growth_slope.store(0.0f, std::memory_order_relaxed);
    _autocorrelation_lag1.store(0.0f, std::memory_order_relaxed);
    _current_leak_tier.store(static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_CLEAN), std::memory_order_relaxed);
    _total_leak_alerts.store(0, std::memory_order_relaxed);
}

RedjetLeakRiskTier RedjetMLTCLeakDetector::record_sweep_observation(
    uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes) {
    hs_ticket_guard_t guard(_history_lock);

    float raw_used_mb = static_cast<float>(live_bytes) / (1024.0f * 1024.0f);
    float filtered_mb = _kalman_filter.update(raw_used_mb);

    size_t idx = _history_head;
    _history[idx].timestamp_ns = sacculus_rdtsc();
    _history[idx].used_bytes = live_bytes;
    _history[idx].retained_bytes = (capacity_bytes > live_bytes) ? (capacity_bytes - live_bytes) : 0;
    _history[idx].sweep_cycle = sweep_cycle;
    _history[idx].filtered_used_mb = filtered_mb;

    _history_head = (_history_head + 1) % HISTORY_CAPACITY;
    if (_history_count < HISTORY_CAPACITY) {
        _history_count++;
    }

    float slope = compute_growth_slope();
    float autocorr = compute_autocorrelation(1);

    _memory_growth_slope.store(slope, std::memory_order_release);
    _autocorrelation_lag1.store(autocorr, std::memory_order_release);

    RedjetLeakRiskTier tier = RedjetLeakRiskTier::LEAK_CLEAN;

    // Mathematical leak assessment:
    // Sustained positive slope + strong lag-1 autocorrelation (indicating structural retention)
    if (slope > 5.0f && autocorr > 0.85f) {
        size_t stag = _consecutive_stagnant_cycles.fetch_add(1, std::memory_order_relaxed) + 1;
        if (stag >= 10) {
            tier = RedjetLeakRiskTier::LEAK_CRITICAL;
        } else if (stag >= 5) {
            tier = RedjetLeakRiskTier::LEAK_SUSPECT;
        } else {
            tier = RedjetLeakRiskTier::LEAK_WATCH;
        }
    } else if (slope > 2.0f && autocorr > 0.70f) {
        _consecutive_stagnant_cycles.fetch_add(1, std::memory_order_relaxed);
        tier = RedjetLeakRiskTier::LEAK_WATCH;
    } else {
        _consecutive_stagnant_cycles.store(0, std::memory_order_relaxed);
        tier = RedjetLeakRiskTier::LEAK_CLEAN;
    }

    _current_leak_tier.store(static_cast<uint8_t>(tier), std::memory_order_release);
    if (tier != RedjetLeakRiskTier::LEAK_CLEAN) {
        _total_leak_alerts.fetch_add(1, std::memory_order_relaxed);
    }

    return tier;
}

float RedjetMLTCLeakDetector::compute_growth_slope() noexcept {
    if (_history_count < 4) return 0.0f;

    // Linear regression: y = mx + b
    // x = sample index (0 .. N-1), y = filtered_used_mb
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_xy = 0.0f;
    float sum_x2 = 0.0f;

    size_t n = _history_count;
    float nf = static_cast<float>(n);

    for (size_t i = 0; i < n; ++i) {
        size_t sample_idx = (_history_head + HISTORY_CAPACITY - n + i) % HISTORY_CAPACITY;
        float x = static_cast<float>(i);
        float y = _history[sample_idx].filtered_used_mb;

        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    float denom = (nf * sum_x2) - (sum_x * sum_x);
    if (std::abs(denom) < 0.0001f) return 0.0f;

    float slope = ((nf * sum_xy) - (sum_x * sum_y)) / denom;
    return slope;
}

float RedjetMLTCLeakDetector::compute_autocorrelation(size_t lag) noexcept {
    if (_history_count <= lag + 2) return 0.0f;

    size_t n = _history_count;
    float mean = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        size_t idx = (_history_head + HISTORY_CAPACITY - n + i) % HISTORY_CAPACITY;
        mean += _history[idx].filtered_used_mb;
    }
    mean /= static_cast<float>(n);

    float num = 0.0f;
    float den = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        size_t idx = (_history_head + HISTORY_CAPACITY - n + i) % HISTORY_CAPACITY;
        float diff = _history[idx].filtered_used_mb - mean;
        den += diff * diff;

        if (i + lag < n) {
            size_t next_idx = (_history_head + HISTORY_CAPACITY - n + i + lag) % HISTORY_CAPACITY;
            float next_diff = _history[next_idx].filtered_used_mb - mean;
            num += diff * next_diff;
        }
    }

    if (den < 0.0001f) return 0.0f;
    return num / den;
}

bool RedjetMLTCLeakDetector::requires_emergency_mitigation() const noexcept {
    uint8_t t = _current_leak_tier.load(std::memory_order_relaxed);
    return (t == static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_CRITICAL) ||
            t == static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_SUSPECT));
}

/* ========================================================================= */
/* 3. CORE MEMORY ARBITRATOR IMPLEMENTATION                                  */
/* ========================================================================= */

RedjetMLTCArbitrator::RedjetMLTCArbitrator() : _core_count(0) {
    _arb_lock.next_ticket.store(0, std::memory_order_relaxed);
    _arb_lock.now_serving.store(0, std::memory_order_relaxed);

    for (size_t i = 0; i < MAX_TRACKED_CORES; ++i) {
        _cores[i].core_id = static_cast<uint32_t>(i);
        _cores[i].is_efficiency_core = false;
        _cores[i].numa_node = 0;
        _cores[i].allocated_bytes_epoch.store(0, std::memory_order_relaxed);
        _cores[i].throttle_weight.store(100, std::memory_order_relaxed);
        _cores[i].stall_events.store(0, std::memory_order_relaxed);
    }

    // Default NUMA latency matrix (normalized: 1.0 local, 1.8 remote)
    for (size_t s = 0; s < MAX_NUMA_NODES; ++s) {
        for (size_t d = 0; d < MAX_NUMA_NODES; ++d) {
            _numa_latency_matrix[s][d] = (s == d) ? 1.0f : 1.8f;
        }
    }
}

void RedjetMLTCArbitrator::init(size_t logical_cores) {
    _core_count = std::min<size_t>(logical_cores, MAX_TRACKED_CORES);
}

void RedjetMLTCArbitrator::register_core(uint32_t core_id, bool is_e_core, uint32_t numa_node) {
    if (core_id >= MAX_TRACKED_CORES) return;
    hs_ticket_guard_t guard(_arb_lock);

    _cores[core_id].core_id = core_id;
    _cores[core_id].is_efficiency_core = is_e_core;
    _cores[core_id].numa_node = (numa_node < MAX_NUMA_NODES) ? numa_node : 0;
    _cores[core_id].throttle_weight.store(is_e_core ? _e_core_traffic_cap.load(std::memory_order_relaxed)
                                                    : _p_core_priority_boost.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
}

bool RedjetMLTCArbitrator::arbitrate_allocation(uint32_t core_id, size_t bytes, uint32_t* out_delay_us) {
    if (core_id >= MAX_TRACKED_CORES) return true;

    uint32_t weight = _cores[core_id].throttle_weight.load(std::memory_order_relaxed);
    _cores[core_id].allocated_bytes_epoch.fetch_add(bytes, std::memory_order_relaxed);

    if (weight < 50) {
        _cores[core_id].stall_events.fetch_add(1, std::memory_order_relaxed);
        if (out_delay_us != nullptr) {
            *out_delay_us = (50 - weight) * 2;
        }
        return false; // Throttled
    }

    if (out_delay_us != nullptr) {
        *out_delay_us = 0;
    }
    return true;
}

void RedjetMLTCArbitrator::rebalance_bandwidth_weights(RedjetBusCongestionLevel bus_level) {
    for (size_t i = 0; i < _core_count; ++i) {
        bool is_e = _cores[i].is_efficiency_core;
        switch (bus_level) {
            case RedjetBusCongestionLevel::CONGESTION_FREE:
                _cores[i].throttle_weight.store(is_e ? 80 : 120, std::memory_order_relaxed);
                break;
            case RedjetBusCongestionLevel::MILD_CONTENTION:
                _cores[i].throttle_weight.store(is_e ? 60 : 110, std::memory_order_relaxed);
                break;
            case RedjetBusCongestionLevel::HIGH_CONTENTION:
                _cores[i].throttle_weight.store(is_e ? 40 : 90, std::memory_order_relaxed);
                break;
            case RedjetBusCongestionLevel::BUS_SATURATION:
                _cores[i].throttle_weight.store(is_e ? 20 : 70, std::memory_order_relaxed);
                break;
            case RedjetBusCongestionLevel::EMERGENCY_THROTTLE:
                _cores[i].throttle_weight.store(is_e ? 10 : 40, std::memory_order_relaxed);
                break;
        }
    }
}

void RedjetMLTCArbitrator::reset_epoch_counters() {
    for (size_t i = 0; i < _core_count; ++i) {
        _cores[i].allocated_bytes_epoch.store(0, std::memory_order_relaxed);
    }
}

/* ========================================================================= */
/* 4. ALIGNMENT HAZARD AUTO-REPAIR ENGINE IMPLEMENTATION                     */
/* ========================================================================= */

RedjetAlignmentHazardEngine::RedjetAlignmentHazardEngine() {
    _alignment_checks.store(0, std::memory_order_relaxed);
    _hazards_detected.store(0, std::memory_order_relaxed);
    _realignments_performed.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 5. MLTC SOLVER NEXUS IMPLEMENTATION                                       */
/* ========================================================================= */

bool hs_mltc_engine_t::check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps) {
    velocity_mbps = alloc_rate_mbps;

    // Check leak risk
    RedjetLeakRiskTier risk = evaluate_leak_risk(0, used_bytes, capacity_bytes, 0);
    if (risk == RedjetLeakRiskTier::LEAK_CRITICAL || risk == RedjetLeakRiskTier::LEAK_SUSPECT) {
        continuous_upward_trend.store(true, std::memory_order_release);
        emergency_compactions.fetch_add(1, std::memory_order_relaxed);

        // Dispatch autonomous VFS compaction solver
        check_autonomous_vfs_compaction_trigger(used_bytes, capacity_bytes, 64 * 1024 * 1024);
        return true;
    }

    continuous_upward_trend.store(false, std::memory_order_release);
    return false;
}

void hs_mltc_engine_t::reset() {
    velocity_mbps = 0.0f;
    misaligned_count = 0;
    total_inspected = 0;
    continuous_upward_trend.store(false, std::memory_order_relaxed);
    emergency_compactions.store(0, std::memory_order_relaxed);
    throttled_allocations.store(0, std::memory_order_relaxed);
    realignments_forced.store(0, std::memory_order_relaxed);
}

void hs_mltc_engine_t::record_l3_dram_telemetry(uint64_t l3_misses, uint64_t dram_cycles, uint64_t bus_stalls, float alloc_mbps) {
    uint64_t now_tsc = sacculus_rdtsc();
    last_telemetry_sample_tsc.store(now_tsc, std::memory_order_relaxed);

    latest_snapshot.timestamp_ns = now_tsc;
    latest_snapshot.l3_miss_count = l3_misses;
    latest_snapshot.dram_access_cycles = dram_cycles;
    latest_snapshot.bus_stall_cycles = bus_stalls;
    latest_snapshot.bandwidth_mbps = alloc_mbps;

    // Calculate normalized congestion score: 0.0 (idle) to 1.0 (saturated)
    float stall_score = (bus_stalls > 1000000) ? 1.0f : (static_cast<float>(bus_stalls) / 1000000.0f);
    float bw_score = (alloc_mbps > 1200.0f) ? 1.0f : (alloc_mbps / 1200.0f);
    float score = 0.6f * bw_score + 0.4f * stall_score;
    latest_snapshot.congestion_score = score;

    if (score < 0.25f) {
        latest_snapshot.congestion_level = RedjetBusCongestionLevel::CONGESTION_FREE;
    } else if (score < 0.50f) {
        latest_snapshot.congestion_level = RedjetBusCongestionLevel::MILD_CONTENTION;
    } else if (score < 0.75f) {
        latest_snapshot.congestion_level = RedjetBusCongestionLevel::HIGH_CONTENTION;
    } else if (score < 0.90f) {
        latest_snapshot.congestion_level = RedjetBusCongestionLevel::BUS_SATURATION;
    } else {
        latest_snapshot.congestion_level = RedjetBusCongestionLevel::EMERGENCY_THROTTLE;
    }

    arbitrator.rebalance_bandwidth_weights(latest_snapshot.congestion_level);
}

void hs_mltc_engine_t::pace_mutator_allocation(size_t bytes, uint32_t thread_id) {
    if (pacer.should_pace_allocation(bytes, velocity_mbps)) {
        throttled_allocations.fetch_add(1, std::memory_order_relaxed);
        pacer.apply_bus_pacing(bytes, thread_id);
    }
}

RedjetLeakRiskTier hs_mltc_engine_t::evaluate_leak_risk(uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes) {
    return leak_detector.record_sweep_observation(sweep_cycle, live_bytes, capacity_bytes, reclaimed_bytes);
}

bool hs_mltc_engine_t::arbitrate_core_bandwidth(uint32_t core_id, size_t requested_bytes, uint32_t* out_delay_us) {
    return arbitrator.arbitrate_allocation(core_id, requested_bytes, out_delay_us);
}

RedjetBusCongestionLevel hs_mltc_engine_t::current_congestion_level() const noexcept {
    return latest_snapshot.congestion_level;
}

RedjetMLTCTelemetrySnapshot hs_mltc_engine_t::get_latest_telemetry() const {
    return latest_snapshot;
}

float hs_mltc_engine_t::compute_vfs_fragmentation_score(size_t used_bytes, size_t capacity_bytes) const noexcept {
    if (capacity_bytes == 0) return 0.0f;
    float used_ratio = static_cast<float>(used_bytes) / static_cast<float>(capacity_bytes);
    // If utilized ratio is low (<30%) in large capacity, fragmentation or unused bloat is high
    if (used_ratio < 0.30f && capacity_bytes > 64 * 1024 * 1024) {
        return (0.30f - used_ratio) / 0.30f * 0.60f + 0.20f;
    }
    return (1.0f - used_ratio) * 0.25f;
}

bool hs_mltc_engine_t::is_bus_traffic_lull_window() const noexcept {
    return (latest_snapshot.congestion_level == RedjetBusCongestionLevel::CONGESTION_FREE ||
            latest_snapshot.congestion_level == RedjetBusCongestionLevel::MILD_CONTENTION);
}

bool hs_mltc_engine_t::check_autonomous_vfs_compaction_trigger(size_t used_bytes, size_t capacity_bytes, size_t target_reclaim_bytes) {
    float frag_score = compute_vfs_fragmentation_score(used_bytes, capacity_bytes);
    bool lull = is_bus_traffic_lull_window();

    RedjetVFSCompactionTriggerResult res = RedjetRMRP::evaluate_and_trigger_vfs_compaction(frag_score, lull, target_reclaim_bytes);
    if (res.triggered) {
        autonomous_vfs_compactions_triggered.fetch_add(1, std::memory_order_relaxed);
        compaction_lull_windows_utilized.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
