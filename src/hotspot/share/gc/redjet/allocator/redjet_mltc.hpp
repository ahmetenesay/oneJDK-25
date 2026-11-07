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

#ifndef SHARE_GC_REDJET_REDJETMLTC_HPP
#define SHARE_GC_REDJET_REDJETMLTC_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <chrono>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifndef HS_RMRP_METHOD_E_DEFINED
#define HS_RMRP_METHOD_E_DEFINED
enum class hs_rmrp_method_e : uint8_t {
    PLUMP      = 0,
    SEQUENTIAL = 1
};
#endif

/* ========================================================================= */
/* 1. MLTC CONGESTION & LEAK TELEMETRY DATA STRUCTURES                       */
/* ========================================================================= */

enum class RedjetBusCongestionLevel : uint8_t {
    CONGESTION_FREE    = 0,
    MILD_CONTENTION    = 1,
    HIGH_CONTENTION    = 2,
    BUS_SATURATION     = 3,
    EMERGENCY_THROTTLE = 4
};

enum class RedjetLeakRiskTier : uint8_t {
    LEAK_CLEAN         = 0,
    LEAK_WATCH         = 1,
    LEAK_SUSPECT       = 2,
    LEAK_CRITICAL      = 3
};

struct alignas(64) RedjetMLTCTelemetrySnapshot {
    uint64_t timestamp_ns{ 0 };
    uint64_t l3_miss_count{ 0 };
    uint64_t dram_access_cycles{ 0 };
    uint64_t bus_stall_cycles{ 0 };
    float    bandwidth_mbps{ 0.0f };
    float    congestion_score{ 0.0f }; // 0.0 (idle) to 1.0 (fully saturated)
    RedjetBusCongestionLevel congestion_level{ RedjetBusCongestionLevel::CONGESTION_FREE };
    uint32_t active_mutator_threads{ 0 };
    uint32_t throttled_thread_count{ 0 };
};

/* ========================================================================= */
/* 2. BUS TRAFFIC PACING SUBSYSTEM                                           */
/* ========================================================================= */

class RedjetMLTCPacer {
public:
    static constexpr size_t DEFAULT_BURST_ALLOWANCE_BYTES = 4ULL * 1024 * 1024;
    static constexpr float  VELOCITY_THROTTLE_THRESHOLD_MBPS = 650.0f;
    static constexpr float  VELOCITY_PANIC_THRESHOLD_MBPS = 1200.0f;

private:
    std::atomic<float>    _target_bandwidth_mbps{ 800.0f };
    std::atomic<uint64_t> _pacing_credit_pool{ DEFAULT_BURST_ALLOWANCE_BYTES };
    std::atomic<uint64_t> _total_paced_allocations{ 0 };
    std::atomic<uint64_t> _total_stall_delay_ns{ 0 };
    std::atomic<uint32_t> _active_throttle_rate_percent{ 0 };

public:
    RedjetMLTCPacer();

    // Evaluates allocation request against available bus credits
    bool should_pace_allocation(size_t requested_bytes, float current_velocity_mbps) noexcept;

    // Computes microsecond stall delay based on congestion severity
    uint32_t compute_pace_delay_us(size_t requested_bytes, RedjetBusCongestionLevel level) noexcept;

    // Executes inline spinning or yield to pace traffic
    void apply_bus_pacing(size_t requested_bytes, uint32_t thread_id);

    // Replenishes credits periodically based on clock tick
    void replenish_credits(uint64_t elapsed_us, float target_mbps);

    // Getters
    uint64_t total_paced_allocations() const noexcept { return _total_paced_allocations.load(std::memory_order_relaxed); }
    uint64_t total_stall_delay_ns() const noexcept { return _total_stall_delay_ns.load(std::memory_order_relaxed); }
    uint32_t active_throttle_rate() const noexcept { return _active_throttle_rate_percent.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 3. STAGNANT CYCLE LEAK DETECTOR                                           */
/* ========================================================================= */

class RedjetMLTCLeakDetector {
public:
    static constexpr size_t HISTORY_CAPACITY = 32;
    static constexpr size_t STAGNANT_CYCLE_THRESHOLD = 5;

private:
    struct HeapSample {
        uint64_t timestamp_ns{ 0 };
        size_t   used_bytes{ 0 };
        size_t   retained_bytes{ 0 };
        uint64_t sweep_cycle{ 0 };
    };

    HeapSample            _history[HISTORY_CAPACITY];
    size_t                _history_count{ 0 };
    size_t                _history_head{ 0 };
    hs_ticket_lock_t      _history_lock;

    std::atomic<size_t>   _consecutive_stagnant_cycles{ 0 };
    std::atomic<float>    _memory_growth_slope{ 0.0f };
    std::atomic<uint8_t>  _current_leak_tier{ static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_CLEAN) };
    std::atomic<uint64_t> _total_leak_alerts{ 0 };

public:
    RedjetMLTCLeakDetector();

    // Inspects heap retention after sweep cycle
    RedjetLeakRiskTier record_sweep_observation(uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes);

    // Computes linear regression growth slope over history buffer
    float compute_growth_slope() noexcept;

    // Checks if emergency compaction should be dispatched
    bool requires_emergency_mitigation() const noexcept;

    RedjetLeakRiskTier current_risk_tier() const noexcept {
        return static_cast<RedjetLeakRiskTier>(_current_leak_tier.load(std::memory_order_relaxed));
    }
    size_t stagnant_cycles() const noexcept { return _consecutive_stagnant_cycles.load(std::memory_order_relaxed); }
    uint64_t total_leak_alerts() const noexcept { return _total_leak_alerts.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 4. CORE MEMORY ARBITRATOR (P-Core vs E-Core, NUMA Pacing)                 */
/* ========================================================================= */

class RedjetMLTCArbitrator {
public:
    static constexpr size_t MAX_TRACKED_CORES = 128;

    struct alignas(64) CoreArbitrationState {
        uint32_t core_id{ 0 };
        bool     is_efficiency_core{ false };
        uint32_t numa_node{ 0 };
        std::atomic<uint64_t> allocated_bytes_epoch{ 0 };
        std::atomic<uint32_t> throttle_weight{ 100 }; // 100 = 100% normal allowance
        std::atomic<uint32_t> stall_events{ 0 };
    };

private:
    CoreArbitrationState _cores[MAX_TRACKED_CORES];
    size_t               _core_count{ 0 };
    hs_ticket_lock_t     _arb_lock;

    std::atomic<uint32_t> _p_core_priority_boost{ 120 }; // 120% weight to P-cores
    std::atomic<uint32_t> _e_core_traffic_cap{ 80 };     // 80% cap on E-core bursts

public:
    RedjetMLTCArbitrator();

    void init(size_t logical_cores);

    // Registers thread core affinity and characteristics
    void register_core(uint32_t core_id, bool is_e_core, uint32_t numa_node);

    // Arbitrates quota requested by given core
    bool arbitrate_allocation(uint32_t core_id, size_t bytes, uint32_t* out_delay_us = nullptr);

    // Periodic rebalance of core weights based on bus congestion
    void rebalance_bandwidth_weights(RedjetBusCongestionLevel bus_level);

    void reset_epoch_counters();
};

/* ========================================================================= */
/* 5. MLTC (MEMORY LEAK & TRAFFIC CONGESTION) SOLVER NEXUS                   */
/* ========================================================================= */

struct hs_mltc_engine_t {
    float               velocity_mbps{ 0.0f };
    size_t              misaligned_count{ 0 };
    size_t              total_inspected{ 0 };
    std::atomic<bool>   continuous_upward_trend{ false };
    std::atomic<size_t> emergency_compactions{ 0 };
    std::atomic<size_t> throttled_allocations{ 0 };
    std::atomic<size_t> realignments_forced{ 0 };

    // Deepened MLTC Subsystems
    RedjetMLTCPacer          pacer;
    RedjetMLTCLeakDetector   leak_detector;
    RedjetMLTCArbitrator     arbitrator;
    std::atomic<uint64_t>    last_telemetry_sample_tsc{ 0 };
    RedjetMLTCTelemetrySnapshot latest_snapshot{};

    RJ_API bool check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps);
    RJ_API void reset();

    // Advanced Bus Pacing & Congestion Telemetry APIs
    RJ_API void record_l3_dram_telemetry(uint64_t l3_misses, uint64_t dram_cycles, uint64_t bus_stalls, float alloc_mbps);
    RJ_API void pace_mutator_allocation(size_t bytes, uint32_t thread_id);
    RJ_API RedjetLeakRiskTier evaluate_leak_risk(uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes);
    RJ_API bool arbitrate_core_bandwidth(uint32_t core_id, size_t requested_bytes, uint32_t* out_delay_us = nullptr);
    RJ_API RedjetBusCongestionLevel current_congestion_level() const noexcept;
    RJ_API RedjetMLTCTelemetrySnapshot get_latest_telemetry() const;
};

class RedjetRMRP;

extern hs_mltc_engine_t g_hs_mltc_solver;
using RedjetMLTC = hs_mltc_engine_t;

#endif // SHARE_GC_REDJET_REDJETMLTC_HPP
