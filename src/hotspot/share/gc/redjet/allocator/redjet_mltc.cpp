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
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <thread>
#include <algorithm>
#include <cmath>

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
    uint32_t delay_us = compute_pace_delay_us(requested_bytes, level);

    if (delay_us > 0) {
        _total_paced_allocations.fetch_add(1, std::memory_order_relaxed);
        _total_stall_delay_ns.fetch_add(static_cast<uint64_t>(delay_us) * 1000ULL, std::memory_order_relaxed);

        const uint64_t start_tsc = sacculus_rdtsc();
        const uint64_t spin_target_cycles = static_cast<uint64_t>(delay_us) * 2500ULL; // Approximate 2.5 GHz cycles
        
        if (delay_us < 10) {
            while ((sacculus_rdtsc() - start_tsc) < spin_target_cycles) {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
                _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
                __yield();
#else
                __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
            }
        } else {
            std::this_thread::yield();
        }

        hs_hotlog_emit(HS_Ebin0_ALLOC_PACE_STALL, HS_Ebin1_TRACE, thread_id,
                       static_cast<uint16_t>(level), 0, 0, 0, 0,
                       delay_us, "mltc_pacer_delay");
    }
}

void RedjetMLTCPacer::replenish_credits(uint64_t elapsed_us, float target_mbps) {
    if (elapsed_us == 0) return;
    float bytes_per_us = (target_mbps * 1024.0f * 1024.0f) / 1000000.0f;
    uint64_t add_credits = static_cast<uint64_t>(bytes_per_us * static_cast<float>(elapsed_us));

    uint64_t cur = _pacing_credit_pool.load(std::memory_order_relaxed);
    uint64_t max_credits = DEFAULT_BURST_ALLOWANCE_BYTES;
    if (cur + add_credits > max_credits) {
        _pacing_credit_pool.store(max_credits, std::memory_order_relaxed);
    } else {
        _pacing_credit_pool.fetch_add(add_credits, std::memory_order_relaxed);
    }
}

/* ========================================================================= */
/* 2. REDJET MLTC LEAK DETECTOR IMPLEMENTATION                               */
/* ========================================================================= */

RedjetMLTCLeakDetector::RedjetMLTCLeakDetector() {
    _consecutive_stagnant_cycles.store(0, std::memory_order_relaxed);
    _memory_growth_slope.store(0.0f, std::memory_order_relaxed);
    _current_leak_tier.store(static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_CLEAN), std::memory_order_relaxed);
    _total_leak_alerts.store(0, std::memory_order_relaxed);

    for (size_t i = 0; i < HISTORY_CAPACITY; ++i) {
        _history[i] = HeapSample{};
    }
}

RedjetLeakRiskTier RedjetMLTCLeakDetector::record_sweep_observation(
    uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes) {

    hs_ticket_guard_t guard(_history_lock);

    size_t idx = (_history_head + _history_count) % HISTORY_CAPACITY;
    if (_history_count == HISTORY_CAPACITY) {
        _history_head = (_history_head + 1) % HISTORY_CAPACITY;
    } else {
        _history_count++;
    }

    _history[idx].timestamp_ns = sacculus_now_ns();
    _history[idx].used_bytes = live_bytes;
    _history[idx].retained_bytes = (capacity_bytes > live_bytes) ? (capacity_bytes - live_bytes) : 0;
    _history[idx].sweep_cycle = sweep_cycle;

    // Check if reclaimed bytes is abnormally low compared to total live bytes (stagnancy)
    if (live_bytes > 0 && reclaimed_bytes < (live_bytes / 100)) {
        _consecutive_stagnant_cycles.fetch_add(1, std::memory_order_relaxed);
    } else {
        _consecutive_stagnant_cycles.store(0, std::memory_order_relaxed);
    }

    float slope = compute_growth_slope();
    _memory_growth_slope.store(slope, std::memory_order_relaxed);

    RedjetLeakRiskTier tier = RedjetLeakRiskTier::LEAK_CLEAN;
    size_t stagnants = _consecutive_stagnant_cycles.load(std::memory_order_relaxed);

    if (stagnants >= STAGNANT_CYCLE_THRESHOLD * 2 || (capacity_bytes > 0 && (live_bytes * 100 / capacity_bytes) > 90)) {
        tier = RedjetLeakRiskTier::LEAK_CRITICAL;
        _total_leak_alerts.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, static_cast<uint32_t>(sweep_cycle),
                            "mltc_leak_crit", "[MLTC-LEAK] Critical stagnant memory retention detected!");
    } else if (stagnants >= STAGNANT_CYCLE_THRESHOLD || slope > 0.15f) {
        tier = RedjetLeakRiskTier::LEAK_SUSPECT;
        hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, static_cast<uint32_t>(sweep_cycle),
                            "mltc_leak_suspect", "[MLTC-LEAK] Monotonic stagnant memory growth suspected.");
    } else if (stagnants >= 2 || slope > 0.05f) {
        tier = RedjetLeakRiskTier::LEAK_WATCH;
    }

    _current_leak_tier.store(static_cast<uint8_t>(tier), std::memory_order_release);
    return tier;
}

float RedjetMLTCLeakDetector::compute_growth_slope() noexcept {
    if (_history_count < 3) return 0.0f;

    // Simple linear regression over recorded samples: y = live_bytes, x = index
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_xy = 0.0f;
    float sum_xx = 0.0f;
    const float n = static_cast<float>(_history_count);

    for (size_t i = 0; i < _history_count; ++i) {
        size_t idx = (_history_head + i) % HISTORY_CAPACITY;
        float x = static_cast<float>(i);
        float y = static_cast<float>(_history[idx].used_bytes / 1024); // KB

        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    float denom = (n * sum_xx - sum_x * sum_x);
    if (std::abs(denom) < 1e-5f) return 0.0f;

    float slope = (n * sum_xy - sum_x * sum_y) / denom;
    // Normalize relative to initial sample
    size_t first_idx = _history_head;
    float base_kb = static_cast<float>(_history[first_idx].used_bytes / 1024);
    if (base_kb > 0.0f) {
        return slope / base_kb;
    }
    return slope;
}

bool RedjetMLTCLeakDetector::requires_emergency_mitigation() const noexcept {
    return _current_leak_tier.load(std::memory_order_relaxed) == static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_CRITICAL);
}

/* ========================================================================= */
/* 3. REDJET MLTC ARBITRATOR IMPLEMENTATION                                  */
/* ========================================================================= */

RedjetMLTCArbitrator::RedjetMLTCArbitrator() {
    _core_count = 0;
    _p_core_priority_boost.store(120, std::memory_order_relaxed);
    _e_core_traffic_cap.store(80, std::memory_order_relaxed);
}

void RedjetMLTCArbitrator::init(size_t logical_cores) {
    hs_ticket_guard_t guard(_arb_lock);
    _core_count = (logical_cores > MAX_TRACKED_CORES) ? MAX_TRACKED_CORES : logical_cores;
    for (size_t i = 0; i < _core_count; ++i) {
        _cores[i].core_id = static_cast<uint32_t>(i);
        _cores[i].is_efficiency_core = false;
        _cores[i].numa_node = 0;
        _cores[i].allocated_bytes_epoch.store(0, std::memory_order_relaxed);
        _cores[i].throttle_weight.store(100, std::memory_order_relaxed);
        _cores[i].stall_events.store(0, std::memory_order_relaxed);
    }
}

void RedjetMLTCArbitrator::register_core(uint32_t core_id, bool is_e_core, uint32_t numa_node) {
    if (core_id >= MAX_TRACKED_CORES) return;
    hs_ticket_guard_t guard(_arb_lock);
    if (core_id >= _core_count) {
        _core_count = core_id + 1;
    }
    _cores[core_id].core_id = core_id;
    _cores[core_id].is_efficiency_core = is_e_core;
    _cores[core_id].numa_node = numa_node;
    _cores[core_id].throttle_weight.store(is_e_core ? _e_core_traffic_cap.load() : _p_core_priority_boost.load(),
                                          std::memory_order_relaxed);
}

bool RedjetMLTCArbitrator::arbitrate_allocation(uint32_t core_id, size_t bytes, uint32_t* out_delay_us) {
    if (core_id >= _core_count) {
        if (out_delay_us) *out_delay_us = 0;
        return true;
    }

    CoreArbitrationState& c = _cores[core_id];
    c.allocated_bytes_epoch.fetch_add(bytes, std::memory_order_relaxed);

    uint32_t weight = c.throttle_weight.load(std::memory_order_relaxed);
    if (weight < 50) {
        // Severe core throttling in effect
        c.stall_events.fetch_add(1, std::memory_order_relaxed);
        if (out_delay_us) {
            *out_delay_us = (50 - weight) * 2;
        }
        return false;
    }

    if (out_delay_us) *out_delay_us = 0;
    return true;
}

void RedjetMLTCArbitrator::rebalance_bandwidth_weights(RedjetBusCongestionLevel bus_level) {
    for (size_t i = 0; i < _core_count; ++i) {
        CoreArbitrationState& c = _cores[i];
        if (bus_level >= RedjetBusCongestionLevel::BUS_SATURATION) {
            if (c.is_efficiency_core) {
                // Heavily throttle background/E-cores to give runway to latency-critical mutators
                c.throttle_weight.store(30, std::memory_order_relaxed);
            } else {
                c.throttle_weight.store(80, std::memory_order_relaxed);
            }
        } else if (bus_level == RedjetBusCongestionLevel::HIGH_CONTENTION) {
            if (c.is_efficiency_core) {
                c.throttle_weight.store(60, std::memory_order_relaxed);
            } else {
                c.throttle_weight.store(100, std::memory_order_relaxed);
            }
        } else {
            // Normal operating weights
            c.throttle_weight.store(c.is_efficiency_core ? _e_core_traffic_cap.load() : _p_core_priority_boost.load(),
                                    std::memory_order_relaxed);
        }
    }
}

void RedjetMLTCArbitrator::reset_epoch_counters() {
    for (size_t i = 0; i < _core_count; ++i) {
        _cores[i].allocated_bytes_epoch.store(0, std::memory_order_relaxed);
    }
}

/* ========================================================================= */
/* 4. REDJET MLTC NEXUS IMPLEMENTATION                                       */
/* ========================================================================= */

bool hs_mltc_engine_t::check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps) {
    velocity_mbps = alloc_rate_mbps;
    bool solved = false;

    // 1. Continuous upward trend / potential leak detection
    if (capacity_bytes > 0 && (used_bytes * 100 / capacity_bytes) > 85) {
        continuous_upward_trend.store(true, std::memory_order_relaxed);
        emergency_compactions.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_WARN, 0, "mltc_leak",
                            "[MLTC-LEAK] Continuous upward heap trend detected: invoking emergency defragmentation");
        solved = true;
    }

    // 2. High allocation velocity / traffic congestion detection
    if (alloc_rate_mbps > 500.0f) {
        throttled_allocations.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_ALLOC_PACE_STALL, HS_Ebin1_WARN, 0, "mltc_congest",
                            "[MLTC-CONGEST] High allocation velocity detected: engaging traffic throttling and page rebate");
        solved = true;
    }

    // 3. SRAM/DRAM Bus hazard misalignment detection
    if (total_inspected > 0 && (misaligned_count * 100 / total_inspected) > 20) {
        realignments_forced.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_GC_ALIGN, HS_Ebin1_WARN, 0, "mltc_align",
                            "[MLTC-ALIGN] Memory bus hazard detected: forcing 256-byte cache/DRAM realignment");
        solved = true;
    }

    // 4. Update arbitrator weights
    RedjetBusCongestionLevel level = current_congestion_level();
    arbitrator.rebalance_bandwidth_weights(level);

    return solved;
}

void hs_mltc_engine_t::reset() {
    continuous_upward_trend.store(false, std::memory_order_relaxed);
    emergency_compactions.store(0, std::memory_order_relaxed);
    throttled_allocations.store(0, std::memory_order_relaxed);
    realignments_forced.store(0, std::memory_order_relaxed);
    arbitrator.reset_epoch_counters();
}

void hs_mltc_engine_t::record_l3_dram_telemetry(
    uint64_t l3_misses, uint64_t dram_cycles, uint64_t bus_stalls, float alloc_mbps) {

    uint64_t now = sacculus_now_ns();
    last_telemetry_sample_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);

    RedjetMLTCTelemetrySnapshot snap;
    snap.timestamp_ns = now;
    snap.l3_miss_count = l3_misses;
    snap.dram_access_cycles = dram_cycles;
    snap.bus_stall_cycles = bus_stalls;
    snap.bandwidth_mbps = alloc_mbps;

    // Calculate congestion score: normalized based on L3 miss rate and bus stall cycles
    float l3_factor = (l3_misses > 100000) ? 1.0f : static_cast<float>(l3_misses) / 100000.0f;
    float stall_factor = (bus_stalls > 500000) ? 1.0f : static_cast<float>(bus_stalls) / 500000.0f;
    float velocity_factor = (alloc_mbps > 1000.0f) ? 1.0f : (alloc_mbps / 1000.0f);

    snap.congestion_score = (0.4f * l3_factor) + (0.4f * stall_factor) + (0.2f * velocity_factor);

    if (snap.congestion_score > 0.85f) {
        snap.congestion_level = RedjetBusCongestionLevel::EMERGENCY_THROTTLE;
    } else if (snap.congestion_score > 0.70f) {
        snap.congestion_level = RedjetBusCongestionLevel::BUS_SATURATION;
    } else if (snap.congestion_score > 0.50f) {
        snap.congestion_level = RedjetBusCongestionLevel::HIGH_CONTENTION;
    } else if (snap.congestion_score > 0.25f) {
        snap.congestion_level = RedjetBusCongestionLevel::MILD_CONTENTION;
    } else {
        snap.congestion_level = RedjetBusCongestionLevel::CONGESTION_FREE;
    }

    snap.active_mutator_threads = 0;
    snap.throttled_thread_count = static_cast<uint32_t>(throttled_allocations.load(std::memory_order_relaxed));

    latest_snapshot = snap;
    arbitrator.rebalance_bandwidth_weights(snap.congestion_level);
}

void hs_mltc_engine_t::pace_mutator_allocation(size_t bytes, uint32_t thread_id) {
    if (pacer.should_pace_allocation(bytes, velocity_mbps)) {
        pacer.apply_bus_pacing(bytes, thread_id);
    }
}

RedjetLeakRiskTier hs_mltc_engine_t::evaluate_leak_risk(
    uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes) {
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
