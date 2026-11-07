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

// ============================================================================
// MLTC (Multi-Layer Tolerance Check) Architecture (PLAN.md Section 9.7 & 11.1)
//
// 3-Tier Hardware Protection & Bus Congestion Defense:
// 1. Memory Leak Detector:
//    - Linear regression slope modeling: m = (N \sum xy - \sum x \sum y) / (N \sum x^2 - (\sum x)^2)
//    - 1D Kalman Filter on heap memory telemetry to reject transient noise.
//    - Retained set autocorrelation analysis R(\tau) to verify structural leaks.
//    - 4-Tier classification: CLEAN -> WATCH -> SUSPECT -> CRITICAL.
// 2. Bus Traffic Pacing Subsystem:
//    - Token bucket credit pacing with microsecond drift compensation.
//    - DDR4/DDR5 memory bus saturation metrics and dynamic mutator backpressure.
// 3. Core Memory Arbitrator:
//    - P-Core vs E-Core quota arbitration and NUMA node latency matrices.
// 4. Alignment Hazard Auto-Repair Engine:
//    - Detection of 64-byte SRAM/DRAM cacheline crossing hazards and vector realignment.
// ============================================================================

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
/* 1. KALMAN FILTER ON HEAP MEASUREMENTS                                     */
/* ========================================================================= */

class RedjetKalmanFilter {
private:
    float _x_est{ 0.0f }; // State estimate
    float _p_est{ 1.0f }; // Estimate error covariance
    float _q_proc{ 0.02f }; // Process noise covariance
    float _r_meas{ 0.15f }; // Measurement noise covariance

public:
    RedjetKalmanFilter(float initial_value = 0.0f, float q = 0.02f, float r = 0.15f)
        : _x_est(initial_value), _p_est(1.0f), _q_proc(q), _r_meas(r) {}

    float update(float measurement) noexcept {
        // Time update (prediction)
        _p_est = _p_est + _q_proc;

        // Measurement update (correction)
        float k = _p_est / (_p_est + _r_meas); // Kalman gain
        _x_est = _x_est + k * (measurement - _x_est);
        _p_est = (1.0f - k) * _p_est;

        return _x_est;
    }

    float current_estimate() const noexcept { return _x_est; }
    void reset(float initial_value = 0.0f) noexcept {
        _x_est = initial_value;
        _p_est = 1.0f;
    }
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
    std::atomic<uint64_t> _last_replenish_tsc{ 0 };

public:
    RedjetMLTCPacer();

    // Evaluates allocation request against available bus credits
    bool should_pace_allocation(size_t requested_bytes, float current_velocity_mbps) noexcept;

    // Computes microsecond stall delay based on congestion severity
    uint32_t compute_pace_delay_us(size_t requested_bytes, RedjetBusCongestionLevel level) noexcept;

    // Executes inline spinning or yield to pace traffic
    void apply_bus_pacing(size_t requested_bytes, uint32_t thread_id);

    // Replenishes credits periodically based on clock tick with drift compensation
    void replenish_credits(uint64_t elapsed_us, float target_mbps);

    // Getters
    uint64_t total_paced_allocations() const noexcept { return _total_paced_allocations.load(std::memory_order_relaxed); }
    uint64_t total_stall_delay_ns() const noexcept { return _total_stall_delay_ns.load(std::memory_order_relaxed); }
    uint32_t active_throttle_rate() const noexcept { return _active_throttle_rate_percent.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 3. STAGNANT CYCLE LEAK DETECTOR WITH AUTOCORRELATION                      */
/* ========================================================================= */

class RedjetMLTCLeakDetector {
public:
    static constexpr size_t HISTORY_CAPACITY = 64;
    static constexpr size_t STAGNANT_CYCLE_THRESHOLD = 5;

    struct HeapSample {
        uint64_t timestamp_ns{ 0 };
        size_t   used_bytes{ 0 };
        size_t   retained_bytes{ 0 };
        uint64_t sweep_cycle{ 0 };
        float    filtered_used_mb{ 0.0f };
    };

private:
    HeapSample            _history[HISTORY_CAPACITY];
    size_t                _history_count{ 0 };
    size_t                _history_head{ 0 };
    hs_ticket_lock_t      _history_lock;

    RedjetKalmanFilter    _kalman_filter;

    std::atomic<size_t>   _consecutive_stagnant_cycles{ 0 };
    std::atomic<float>    _memory_growth_slope{ 0.0f };
    std::atomic<float>    _autocorrelation_lag1{ 0.0f };
    std::atomic<uint8_t>  _current_leak_tier{ static_cast<uint8_t>(RedjetLeakRiskTier::LEAK_CLEAN) };
    std::atomic<uint64_t> _total_leak_alerts{ 0 };

public:
    RedjetMLTCLeakDetector();

    // Inspects heap retention after sweep cycle
    RedjetLeakRiskTier record_sweep_observation(uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes);

    // Computes linear regression growth slope over history buffer
    float compute_growth_slope() noexcept;

    // Computes autocorrelation coefficient R(lag) over filtered history
    float compute_autocorrelation(size_t lag = 1) noexcept;

    // Checks if emergency compaction should be dispatched
    bool requires_emergency_mitigation() const noexcept;

    RedjetLeakRiskTier current_risk_tier() const noexcept {
        return static_cast<RedjetLeakRiskTier>(_current_leak_tier.load(std::memory_order_relaxed));
    }
    size_t stagnant_cycles() const noexcept { return _consecutive_stagnant_cycles.load(std::memory_order_relaxed); }
    uint64_t total_leak_alerts() const noexcept { return _total_leak_alerts.load(std::memory_order_relaxed); }
    float memory_growth_slope() const noexcept { return _memory_growth_slope.load(std::memory_order_relaxed); }
    float autocorrelation_lag1() const noexcept { return _autocorrelation_lag1.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 4. CORE MEMORY ARBITRATOR (P-Core vs E-Core, NUMA Pacing)                 */
/* ========================================================================= */

class RedjetMLTCArbitrator {
public:
    static constexpr size_t MAX_TRACKED_CORES = 128;
    static constexpr size_t MAX_NUMA_NODES    = 8;

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
    float                _numa_latency_matrix[MAX_NUMA_NODES][MAX_NUMA_NODES];

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
/* 5. ALIGNMENT HAZARD AUTO-REPAIR ENGINE                                    */
/* ========================================================================= */

class RedjetAlignmentHazardEngine {
private:
    std::atomic<uint64_t> _alignment_checks{ 0 };
    std::atomic<uint64_t> _hazards_detected{ 0 };
    std::atomic<uint64_t> _realignments_performed{ 0 };

public:
    RedjetAlignmentHazardEngine();

    // Inspects address for 64-byte SRAM/DRAM cache line boundary crossing
    static bool is_alignment_hazard(uintptr_t addr, size_t access_width) noexcept {
        uintptr_t line_offset = addr & 63;
        return (line_offset + access_width) > 64;
    }

    // Aligns address to next 64-byte boundary
    static uintptr_t align_to_cacheline(uintptr_t addr) noexcept {
        return (addr + 63) & ~static_cast<uintptr_t>(63);
    }

    void record_hazard() noexcept {
        _hazards_detected.fetch_add(1, std::memory_order_relaxed);
    }

    void record_realignment() noexcept {
        _realignments_performed.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t hazards_detected() const noexcept { return _hazards_detected.load(std::memory_order_relaxed); }
    uint64_t realignments_performed() const noexcept { return _realignments_performed.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 6. MLTC (MEMORY LEAK & TRAFFIC CONGESTION) SOLVER NEXUS                   */
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
    RedjetMLTCPacer             pacer;
    RedjetMLTCLeakDetector      leak_detector;
    RedjetMLTCArbitrator        arbitrator;
    RedjetAlignmentHazardEngine alignment_engine;
    std::atomic<uint64_t>       last_telemetry_sample_tsc{ 0 };
    RedjetMLTCTelemetrySnapshot latest_snapshot{};

    // Autonomous VFS Compaction 3-Signal Telemetry & Control
    std::atomic<uint64_t> autonomous_vfs_compactions_triggered{ 0 };
    std::atomic<uint64_t> compaction_lull_windows_utilized{ 0 };

    RJ_API bool check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps);
    RJ_API void reset();

    // Advanced Bus Pacing & Congestion Telemetry APIs
    RJ_API void record_l3_dram_telemetry(uint64_t l3_misses, uint64_t dram_cycles, uint64_t bus_stalls, float alloc_mbps);
    RJ_API void pace_mutator_allocation(size_t bytes, uint32_t thread_id);
    RJ_API RedjetLeakRiskTier evaluate_leak_risk(uint64_t sweep_cycle, size_t live_bytes, size_t capacity_bytes, size_t reclaimed_bytes);
    RJ_API bool arbitrate_core_bandwidth(uint32_t core_id, size_t requested_bytes, uint32_t* out_delay_us = nullptr);
    RJ_API RedjetBusCongestionLevel current_congestion_level() const noexcept;
    RJ_API RedjetMLTCTelemetrySnapshot get_latest_telemetry() const;

    // Autonomous VFS Compaction 3-Signal Evaluations:
    RJ_API float compute_vfs_fragmentation_score(size_t used_bytes, size_t capacity_bytes) const noexcept;
    RJ_API bool is_bus_traffic_lull_window() const noexcept;
    RJ_API bool check_autonomous_vfs_compaction_trigger(size_t used_bytes, size_t capacity_bytes, size_t target_reclaim_bytes = 64 * 1024 * 1024);
};

class RedjetRMRP;

extern hs_mltc_engine_t g_hs_mltc_solver;
using RedjetMLTC = hs_mltc_engine_t;

#endif // SHARE_GC_REDJET_REDJETMLTC_HPP
