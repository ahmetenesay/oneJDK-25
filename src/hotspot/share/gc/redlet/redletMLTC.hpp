/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:9240-9280.
 */

#ifndef SHARE_GC_REDLET_REDLETMLTC_HPP
#define SHARE_GC_REDLET_REDLETMLTC_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

enum class sf_rmrp_method_e : uint8_t {
    PLUMP      = 0,
    SEQUENTIAL = 1
};

/* ========================================================================= */
/* 1. MLTC (MEMORY LEAK & TRAFFIC CONGESTION) SOLVER                         */
/* ========================================================================= */

struct sf_mltc_engine_t {
    float               velocity_mbps{ 0.0f };
    size_t              misaligned_count{ 0 };
    size_t              total_inspected{ 0 };
    std::atomic<bool>   continuous_upward_trend{ false };
    std::atomic<size_t> emergency_compactions{ 0 };
    std::atomic<size_t> throttled_allocations{ 0 };
    std::atomic<size_t> realignments_forced{ 0 };

    bool check_and_solve(size_t used_bytes, size_t capacity_bytes, float alloc_rate_mbps);
    void reset();
};

/* ========================================================================= */
/* 2. RMRP (REDUCE MEMORY RESIDENT SET RECLAMATION)                          */
/* ========================================================================= */

class RedletRMRP {
public:
    static size_t execute_reclamation(sf_rmrp_method_e method, size_t target_bytes);
};

extern sf_mltc_engine_t g_sf_mltc_solver;

#endif // SHARE_GC_REDLET_REDLETMLTC_HPP
