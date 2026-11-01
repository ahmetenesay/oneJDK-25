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

#if defined(REDLET_STANDALONE_BUILD)

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#include "gc/redlet/self_advance/redletSelfAdvanceHooks.hpp"
#include "gc/redlet/self_advance/redletSelfAdvanceHeuristics.hpp"
#include "gc/redlet/self_advance/redletSelfAdvancePrecision.hpp"
#include "gc/redlet/self_advance/redletSelfAdvanceOrchestrator.hpp"
#include "gc/redlet/self_advance/redletCoach.hpp"
#include "gc/redlet/redletSelfHeal.hpp"
#include "gc/redlet/redletSIMD.hpp"

// ============================================================================
// External Stubs / Definitions for Standalone Verification Build
// ============================================================================

void sf_alu_dispatch_nexus_t::complete_op(unsigned int) {}
void SF_custominiVm_live_header_t::writeTailCanary() noexcept {}
bool SF_custominiVm_live_header_t::isTailCanaryValid() const noexcept { return true; }
void RedletPuzzleArena::run_all_self_tuning_benchmarks() {}

extern "C" {
    int SF_custominiVm_has_3d_vcache(void) { return 0; }
    RedletPuzzleArena* sf_get_default_puzzle_arena(void) { return nullptr; }

    void sf_hotlog_emit(SF_custominiVm_event_category_e, SF_custominiVm_severity_e,
                        uint32_t, uint16_t, uint8_t, uint8_t, uint16_t, uint64_t, uint32_t, const char*) {}

    void sf_unified_log_emit(SF_custominiVm_event_category_e, SF_custominiVm_severity_e,
                             uint32_t, const char*, const char*) {}

    bool sf_bind_current_thread(SF_custominiVm_core_tier_t) { return true; }

    bool sf_is_hybrid_cpu(void) { return true; }

    const SF_custominiVm_hardware_topology_t* SF_custominiVm_hardware_topology(void) {
        static SF_custominiVm_hardware_topology_t topo{};
        topo.cpu_cur_freq_mhz = 3200;
        topo.p_cores = 8;
        topo.e_cores = 8;
        return &topo;
    }

    void sf_gc_pausepoint_set_gate(bool) {}
    void SF_custominiVm_set_pausepoint_gate(int) {}
    int  SF_custominiVm_get_pausepoint_gate(void) { return 0; }
}

void sf_accumulate_ieee754_error_f32(const float* cand_ptr, const float* ref_ptr, size_t n_elems,
                                     float& inout_max_err, float& inout_sum_sq, bool& inout_finite_ok) noexcept {
    for (size_t k = 0; k < n_elems; ++k) {
        float c_v = cand_ptr[k];
        float r_v = ref_ptr[k];
        if (!std::isfinite(c_v) || !std::isfinite(r_v)) {
            inout_finite_ok = false;
            inout_max_err = 1e30f;
            inout_sum_sq = 1e30f;
            return;
        }
        float diff = std::fabs(c_v - r_v);
        if (diff > inout_max_err) inout_max_err = diff;
        inout_sum_sq += diff * diff;
    }
}

bool RedletSIMD::has_avx2() {
#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}

bool RedletSIMD::has_avx512() {
    return false; // Test gracefully with AVX-512 off
}

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] Line %d: Assertion '%s' failed: %s\n", __LINE__, #cond, msg); \
            exit(1); \
        } \
    } while (0)

// ============================================================================
// Test 1: Hookpoint TSC Cycle Counter & Telemetry
// ============================================================================
static void test_tsc_hooks_and_telemetry() {
    printf("[TEST 1] Hookpoint TSC Counter & Telemetry Event Ring Buffer...\n");

    g_redlet_tsc_hook_counter.reset();
    g_redlet_tsc_hook_counter.set_cpu_frequency_mhz(3000);
    TEST_ASSERT(g_redlet_tsc_hook_counter.cpu_frequency_mhz() == 3000, "CPU frequency must match 3000 MHz");

    uint64_t t0 = SF_self_advance_hook_enter(0);
    // Simulate short workload
    volatile uint64_t sink = 0;
    for (int i = 0; i < 1000; ++i) sink += i;
    (void)sink;
    uint64_t elapsed = SF_self_advance_hook_exit(0, t0);

    TEST_ASSERT(elapsed > 0, "Elapsed cycles must be > 0");
    TEST_ASSERT(g_redlet_tsc_hook_counter.get_total_invocations(0) == 1, "Invocation count must be 1");
    TEST_ASSERT(g_redlet_tsc_hook_counter.get_average_cycles(0) == elapsed, "Average cycles must match single run");

    // Test out of bounds hook_id
    uint64_t t_oob = SF_self_advance_hook_enter(99);
    SF_self_advance_hook_exit(99, t_oob);
    TEST_ASSERT(g_redlet_tsc_hook_counter.get_total_invocations(99) == 0, "OOB hook_id must be ignored");

    // Test Telemetry Ring Buffer
    g_redlet_self_advance_telemetry.reset();
    TEST_ASSERT(SF_self_advance_telemetry_count() == 0, "Telemetry count must be 0 after reset");

    SF_self_advance_record_event(SF_EVENT_GC_HOTSWAP, "test_tag", "Swapped routine", 1.0, 2.0);
    TEST_ASSERT(SF_self_advance_telemetry_count() == 1, "Telemetry count must be 1");

    printf("  -> PASS: Hookpoint TSC and Telemetry Ring verified.\n");
}

// ============================================================================
// Test 2: TLAB & Falline Allocation & Leak Analyzer
// ============================================================================
static void test_falline_analyzer() {
    printf("[TEST 2] TLAB & Falline Allocation Analyzer & Autonomous Leak Detection...\n");

    g_redlet_falline_analyzer.reset();
    TEST_ASSERT(g_redlet_falline_analyzer.active_thread_count() == 0, "Active threads must start at 0");

    uint64_t tid1 = 0x1001;
    uint64_t tid2 = 0x1002;

    // Record allocations across size bins
    SF_self_advance_record_alloc(tid1, 32, 1);    // <64B, from falline
    SF_self_advance_record_alloc(tid1, 128, 0);   // 64-256B, miss
    SF_self_advance_record_alloc(tid1, 512, 1);   // 256-1KB, falline
    SF_self_advance_record_alloc(tid1, 2048, 0);  // 1KB-4KB, miss
    SF_self_advance_record_alloc(tid1, 8192, 1);  // >4KB, falline

    TEST_ASSERT(g_redlet_falline_analyzer.active_thread_count() == 1, "Active threads must be 1");

    RedletThreadAllocStats stats{};
    bool found = g_redlet_falline_analyzer.query_thread_stats(tid1, &stats);
    TEST_ASSERT(found, "Thread stats must be found for tid1");
    TEST_ASSERT(stats.total_allocations == 5, "Total allocations must be 5");
    TEST_ASSERT(stats.falline_snatches == 3, "Falline snatches must be 3");
    TEST_ASSERT(stats.falline_misses == 2, "Falline misses must be 2");
    TEST_ASSERT(stats.size_bin_under_64 == 1, "<64 bin must be 1");
    TEST_ASSERT(stats.size_bin_64_256 == 1, "64-256 bin must be 1");
    TEST_ASSERT(stats.size_bin_256_1k == 1, "256-1k bin must be 1");
    TEST_ASSERT(stats.size_bin_1k_4k == 1, "1k-4k bin must be 1");
    TEST_ASSERT(stats.size_bin_over_4k == 1, ">4k bin must be 1");

    // Test deallocation
    SF_self_advance_record_free(tid1, 1000);
    g_redlet_falline_analyzer.query_thread_stats(tid1, &stats);
    TEST_ASSERT(stats.total_freed_bytes == 1000, "Freed bytes must be 1000");

    // Test leak heuristic: simulate 150MB allocation on tid2 with 0 freed
    for (int k = 0; k < 60000; ++k) {
        SF_self_advance_record_alloc(tid2, 3000, 1);
    }
    TEST_ASSERT(SF_self_advance_get_leak_alerts() == 0, "No alerts before evaluation");
    g_redlet_falline_analyzer.evaluate_leak_heuristics();
    TEST_ASSERT(SF_self_advance_get_leak_alerts() == 1, "Leak alert must be triggered once");

    // Second evaluation must NOT duplicate alert for same thread
    g_redlet_falline_analyzer.evaluate_leak_heuristics();
    TEST_ASSERT(SF_self_advance_get_leak_alerts() == 1, "Leak alert must not duplicate");

    // Test thread unregistration
    SF_self_advance_unregister_thread(tid1);
    TEST_ASSERT(!g_redlet_falline_analyzer.query_thread_stats(tid1, &stats), "Unregistered thread must not be found");
    TEST_ASSERT(g_redlet_falline_analyzer.active_thread_count() == 1, "Active threads must be 1 after unregister");

    // Boundary: tid=0
    SF_self_advance_record_alloc(0, 1024, 0);
    TEST_ASSERT(g_redlet_falline_analyzer.active_thread_count() == 1, "tid=0 must be rejected safely");

    printf("  -> PASS: TLAB & Falline Analyzer and Leak Heuristics verified.\n");
}

// ============================================================================
// Test 3: SRAM & Cache Pressure Tracker
// ============================================================================
static void test_cache_pressure_tracker() {
    printf("[TEST 3] SRAM & Cache Pressure Tracker...\n");

    g_redlet_cache_tracker.reset();
    TEST_ASSERT(!g_redlet_cache_tracker.is_high_cache_pressure(), "Pressure must start false");
    TEST_ASSERT(!g_redlet_cache_tracker.is_bus_saturated(), "Bus saturated must start false");

    // Low miss rate: 100k cycles, 200k instrs, 5k l1d misses (5%), 1k llc misses (1%)
    g_redlet_cache_tracker.record_manual_sample(100000, 200000, 5000, 1000);
    RedletCachePressureMetrics m{};
    SF_self_advance_query_cache_pressure(&m);

    TEST_ASSERT(!m.high_cache_pressure, "Pressure must be low (<15% L1D, <5% LLC)");
    TEST_ASSERT(!m.bus_bandwidth_saturated, "Bus saturation must be false");
    TEST_ASSERT(m.instructions_per_cycle == 2.0, "IPC must be 2.0");

    // High miss rate: 100k cycles, 20k l1d misses (20%), 12k llc misses (12%)
    g_redlet_cache_tracker.record_manual_sample(100000, 100000, 20000, 12000);
    m = g_redlet_cache_tracker.query_metrics();
    TEST_ASSERT(m.high_cache_pressure, "Pressure must be flagged as high");
    TEST_ASSERT(m.bus_bandwidth_saturated, "Bus saturation must be flagged (>10% LLC)");

    printf("  -> PASS: Cache Pressure Tracker verified.\n");
}

// ============================================================================
// Test 4: Card Table & Bitmap Scan Evolver (Exact 0-Bit Invariance)
// ============================================================================
static void test_bitmap_evolver() {
    printf("[TEST 4] Card Table & Bitmap Scanner (Exact Algebraic 0-Bit Invariance)...\n");

    g_redlet_bitmap_evolver.reset();
    TEST_ASSERT(SF_self_advance_get_active_bitmap_kernel() == static_cast<uint8_t>(RedletBitmapScanKernel::SCALAR_BASELINE),
                "Initial kernel must be SCALAR_BASELINE");

    // Test with varied bit patterns
    constexpr size_t WORDS = 64;
    constexpr size_t MAX_MARKS = 512;
    uintptr_t test_bm[WORDS]{ 0 };

    // Fill with specific patterns (empty, sparse, dense)
    test_bm[0] = 1;
    test_bm[1] = 0x8000000000000000ULL;
    test_bm[2] = 0xAAAAAAAAAAAAAAAAULL;
    test_bm[10] = 0x5555555555555555ULL;
    test_bm[30] = 0xFFFFFFFFFFFFFFFFULL;

    size_t ref_indices[MAX_MARKS]{ 0 };
    size_t bmi2_indices[MAX_MARKS]{ 0 };
    size_t avx2_indices[MAX_MARKS]{ 0 };

    size_t ref_n = RedletCardTableBitmapEvolver::scan_scalar(test_bm, WORDS, ref_indices, MAX_MARKS);
    size_t bmi2_n = RedletCardTableBitmapEvolver::scan_bmi2(test_bm, WORDS, bmi2_indices, MAX_MARKS);
    size_t avx2_n = RedletCardTableBitmapEvolver::scan_avx2(test_bm, WORDS, avx2_indices, MAX_MARKS);

    TEST_ASSERT(ref_n > 0, "Reference scan must find marks");
    TEST_ASSERT(ref_n == bmi2_n, "BMI2 scan count must match reference count exactly");
    TEST_ASSERT(std::memcmp(ref_indices, bmi2_indices, ref_n * sizeof(size_t)) == 0,
                "BMI2 scan indices must achieve exact 0-bit divergence (memcmp == 0)");

    TEST_ASSERT(ref_n == avx2_n, "AVX2 scan count must match reference count exactly");
    TEST_ASSERT(std::memcmp(ref_indices, avx2_indices, ref_n * sizeof(size_t)) == 0,
                "AVX2 scan indices must achieve exact 0-bit divergence (memcmp == 0)");

    // Test kernel evolution
    int evo_ret = SF_self_advance_evolve_bitmap_kernel();
    TEST_ASSERT(evo_ret == 1, "Bitmap evolution experiment must succeed");
    TEST_ASSERT(g_redlet_bitmap_evolver.get_evolution_count() == 1, "Evolution count must be 1");
    TEST_ASSERT(g_redlet_bitmap_evolver.get_exact_matches_verified() >= 1, "Exact matches must be verified");
    TEST_ASSERT(g_redlet_bitmap_evolver.get_divergences_rejected() == 0, "Zero divergences must be rejected");

    // Boundary: empty bitmap
    uintptr_t empty_bm[8]{ 0 };
    size_t empty_res = g_redlet_bitmap_evolver.scan(empty_bm, 8, ref_indices, MAX_MARKS);
    TEST_ASSERT(empty_res == 0, "Empty bitmap must return 0 marks");

    // Boundary: null / 0 max_indices
    TEST_ASSERT(g_redlet_bitmap_evolver.scan(nullptr, 8, ref_indices, MAX_MARKS) == 0, "Null bitmap must return 0");
    TEST_ASSERT(g_redlet_bitmap_evolver.scan(test_bm, 8, nullptr, MAX_MARKS) == 0, "Null out_indices must return 0");
    TEST_ASSERT(g_redlet_bitmap_evolver.scan(test_bm, 8, ref_indices, 0) == 0, "0 max_indices must return 0");

    printf("  -> PASS: Bitmap Scanner and 0-Bit Invariance verified.\n");
}

// ============================================================================
// Test 5: TrashPit & Landfill Reset Heuristics (Memzero)
// ============================================================================
static void test_memory_reset_heuristics() {
    printf("[TEST 5] TrashPit & Landfill Reset Heuristics (libc vs NT streaming vs E-Core chunked)...\n");

    g_redlet_memreset_evolver.reset();
    TEST_ASSERT(g_redlet_memreset_evolver.get_active_strategy() == RedletMemzeroStrategy::LIBC_MEMSET,
                "Default strategy must be LIBC_MEMSET");

    constexpr size_t BUF_SIZE = 128 * 1024; // 128KB
    std::vector<uint8_t> buffer(BUF_SIZE + 64, uint8_t(0xAA));

    // 1. Test libc memset
    std::fill(buffer.begin(), buffer.end(), uint8_t(0xAA));
    RedletMemoryResetEvolver::zero_libc(buffer.data(), BUF_SIZE);
    for (size_t i = 0; i < BUF_SIZE; ++i) {
        TEST_ASSERT(buffer[i] == 0, "All bytes must be zeroed by zero_libc");
    }
    TEST_ASSERT(buffer[BUF_SIZE] == 0xAA, "Guard byte after buffer must not be overwritten");

    // 2. Test E-core chunked reset
    std::fill(buffer.begin(), buffer.end(), uint8_t(0xBB));
    RedletMemoryResetEvolver::zero_ecore_chunked(buffer.data(), BUF_SIZE);
    for (size_t i = 0; i < BUF_SIZE; ++i) {
        TEST_ASSERT(buffer[i] == 0, "All bytes must be zeroed by zero_ecore_chunked");
    }
    TEST_ASSERT(buffer[BUF_SIZE] == 0xBB, "Guard byte after buffer must not be overwritten");

    // 3. Test Non-Temporal streaming store with unaligned pointer offset
    std::fill(buffer.begin(), buffer.end(), uint8_t(0xCC));
    uint8_t* unaligned_ptr = buffer.data() + 3; // deliberate 3-byte misalignment
    size_t test_bytes = 65536;
    RedletMemoryResetEvolver::zero_streaming_nt(unaligned_ptr, test_bytes);
    for (size_t i = 0; i < test_bytes; ++i) {
        TEST_ASSERT(unaligned_ptr[i] == 0, "All bytes must be zeroed by zero_streaming_nt");
    }
    TEST_ASSERT(buffer[2] == 0xCC, "Preceding byte must remain untouched");
    TEST_ASSERT(unaligned_ptr[test_bytes] == 0xCC, "Succeeding byte must remain untouched");

    // 4. Test adaptive selection
    // Large region on P-core with high cache pressure -> NT streaming stores
    SF_self_advance_adaptive_memzero(buffer.data(), 128 * 1024, 1 /* on_p_core */, 1 /* high_pressure */);
    TEST_ASSERT(g_redlet_memreset_evolver.get_active_strategy() == RedletMemzeroStrategy::STREAMING_STORES_NT,
                "Strategy must be STREAMING_STORES_NT for large buffer on P-core with high pressure");

    // Medium region on E-core -> ECORE_CHUNKED
    SF_self_advance_adaptive_memzero(buffer.data(), 8192, 0 /* on_e_core */, 0);
    TEST_ASSERT(g_redlet_memreset_evolver.get_active_strategy() == RedletMemzeroStrategy::ECORE_CHUNKED,
                "Strategy must be ECORE_CHUNKED on E-core");

    // Small region -> LIBC_MEMSET
    SF_self_advance_adaptive_memzero(buffer.data(), 512, 1, 0);
    TEST_ASSERT(g_redlet_memreset_evolver.get_active_strategy() == RedletMemzeroStrategy::LIBC_MEMSET,
                "Strategy must be LIBC_MEMSET for small buffer");

    // Null / 0 size safety
    SF_self_advance_adaptive_memzero(nullptr, 1024, 1, 1);
    SF_self_advance_adaptive_memzero(buffer.data(), 0, 1, 1);

    printf("  -> PASS: Memory Reset Heuristics verified.\n");
}

// ============================================================================
// Test 6: Dynamic RMRP Strategy (Sequential vs Plump/Prune)
// ============================================================================
static void test_rmrp_heuristics() {
    printf("[TEST 6] Dynamic RMRP Strategy Switching...\n");

    g_redlet_rmrp_heuristics.reset();
    TEST_ASSERT(g_redlet_rmrp_heuristics.get_current_strategy() == RedletRMRPStrategy::PLUMP_PRUNE,
                "Initial RMRP strategy must be PLUMP_PRUNE");

    size_t total_reserved = 1000 * 1024 * 1024; // 1000 MB

    // Low usage (20% < 40%) -> SEQUENTIAL_INSTALLMENT
    size_t low_used = 200 * 1024 * 1024;
    auto s1 = static_cast<RedletRMRPStrategy>(
        SF_self_advance_evaluate_rmrp(low_used, low_used, total_reserved, 50.0));
    TEST_ASSERT(s1 == RedletRMRPStrategy::SEQUENTIAL_INSTALLMENT, "Low pressure must switch to SEQUENTIAL");
    TEST_ASSERT(g_redlet_rmrp_heuristics.get_strategy_switch_count() == 1, "Switch count must be 1");

    // Hysteresis zone (50% usage: between 40% and 80%) -> Retain current (SEQUENTIAL)
    size_t mid_used = 500 * 1024 * 1024;
    auto s2 = static_cast<RedletRMRPStrategy>(
        SF_self_advance_evaluate_rmrp(mid_used, mid_used, total_reserved, 50.0));
    TEST_ASSERT(s2 == RedletRMRPStrategy::SEQUENTIAL_INSTALLMENT, "Hysteresis zone must retain SEQUENTIAL");
    TEST_ASSERT(g_redlet_rmrp_heuristics.get_strategy_switch_count() == 1, "Switch count must still be 1");

    // High usage (85% > 80%) -> PLUMP_PRUNE
    size_t high_used = 850 * 1024 * 1024;
    auto s3 = static_cast<RedletRMRPStrategy>(
        SF_self_advance_evaluate_rmrp(high_used, high_used, total_reserved, 50.0));
    TEST_ASSERT(s3 == RedletRMRPStrategy::PLUMP_PRUNE, "High pressure must switch to PLUMP_PRUNE");
    TEST_ASSERT(g_redlet_rmrp_heuristics.get_strategy_switch_count() == 2, "Switch count must be 2");

    g_redlet_rmrp_heuristics.record_reclamation(50 * 1024 * 1024);
    TEST_ASSERT(g_redlet_rmrp_heuristics.get_total_reclaimed() == 50 * 1024 * 1024, "Total reclaimed must match");

    printf("  -> PASS: Dynamic RMRP Strategy verified.\n");
}

// ============================================================================
// Test 7: Resurrection Heuristics (Rehab Arena Allocation Rate Throttling)
// ============================================================================
static void test_resurrection_heuristics() {
    printf("[TEST 7] Resurrection Heuristics & Rehab Arena Quotas...\n");

    g_redlet_resurrection_heuristics.reset();
    TEST_ASSERT(g_redlet_resurrection_heuristics.get_active_quota() == 32, "Default quota must be 32");

    // High velocity (>500 MB/s) -> boost quota to 128
    uint32_t q1 = SF_self_advance_compute_resurrect_quota(600, 100, 1000);
    TEST_ASSERT(q1 == 128, "Quota must be boosted to 128 under high alloc velocity");

    // Moderate velocity (>100 MB/s) -> quota 64
    uint32_t q2 = SF_self_advance_compute_resurrect_quota(250, 100, 1000);
    TEST_ASSERT(q2 == 64, "Quota must be 64 under moderate velocity");

    // Low velocity -> default 32
    uint32_t q3 = SF_self_advance_compute_resurrect_quota(50, 100, 1000);
    TEST_ASSERT(q3 == 32, "Quota must be 32 under low velocity");

    // Landfill capacity constraint (>85% full) -> throttled to max 16
    uint32_t q4 = SF_self_advance_compute_resurrect_quota(600, 900, 1000);
    TEST_ASSERT(q4 == 16, "Quota must be throttled to 16 when Landfill >85% full");

    // Test admission gating
    TEST_ASSERT(g_redlet_resurrection_heuristics.should_admit_resurrection(10) == true, "Queue size 10 < 16 must admit");
    TEST_ASSERT(g_redlet_resurrection_heuristics.should_admit_resurrection(20) == false, "Queue size 20 >= 16 must reject");

    uint64_t app = 0, rej = 0;
    g_redlet_resurrection_heuristics.query_counters(&app, &rej);
    TEST_ASSERT(app == 1, "Approved count must be 1");
    TEST_ASSERT(rej == 1, "Rejected count must be 1");

    printf("  -> PASS: Resurrection Heuristics verified.\n");
}

// ============================================================================
// Test 8: Precision Guard Engine & Java Bytecode Descriptor Verifier
// ============================================================================
static void test_precision_guard_and_verifier() {
    printf("[TEST 8] Precision Guard Engine & Java Method Bytecode Verifier...\n");

    g_redlet_precision_guard.reset();

    // 1. Runtime Precision Guard: FP vs Non-FP Dtype Validation
    TEST_ASSERT(SF_precision_guard_validate(SF_DTYPE_F32, 1e-4f, "test_f32") == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "F32 with 1e-4 must validate as GUARD_OK");
    TEST_ASSERT(SF_precision_guard_validate(SF_DTYPE_F64, 1e-4f, "test_f64") == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "F64 with 1e-4 must validate as GUARD_OK");

    // Exact algebraic: non-FP with epsilon=0 is OK
    TEST_ASSERT(SF_precision_guard_validate(SF_DTYPE_I32, 0.0f, "test_i32_0") == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "Integer with 0.0 epsilon must validate as GUARD_OK");

    // Violation: non-FP with epsilon > 0 MUST FAIL with ERR_TYPE_NOT_FP
    TEST_ASSERT(SF_precision_guard_validate(SF_DTYPE_I32, 1e-4f, "test_i32_err") == static_cast<int>(RedletGuardStatus::ERR_TYPE_NOT_FP),
                "Integer with >0 epsilon must be REJECTED as ERR_TYPE_NOT_FP");
    TEST_ASSERT(SF_precision_guard_get_violations() == 1, "Violations caught must be 1");

    // Negative / NaN epsilon rejection
    TEST_ASSERT(SF_precision_guard_validate(SF_DTYPE_F32, -0.01f, "neg") == static_cast<int>(RedletGuardStatus::ERR_EPSILON_OUT_OF_BOUNDS),
                "Negative epsilon must be rejected");

    // Numeric F32 / F64 verification
    float ref_f32[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    float cand_f32_ok[4] = { 1.00001f, 2.00001f, 3.00001f, 4.00001f };
    float cand_f32_bad[4] = { 1.1f, 2.0f, 3.0f, 4.0f };

    TEST_ASSERT(SF_precision_guard_verify_f32(cand_f32_ok, ref_f32, 4, 1e-3f) == 1,
                "F32 candidate within epsilon must pass");
    TEST_ASSERT(SF_precision_guard_verify_f32(cand_f32_bad, ref_f32, 4, 1e-3f) == 0,
                "F32 candidate exceeding epsilon must fail");

    double ref_f64[4] = { 1.0, 2.0, 3.0, 4.0 };
    double cand_f64_ok[4] = { 1.000000001, 2.000000001, 3.000000001, 4.000000001 };
    double cand_f64_bad[4] = { 1.05, 2.0, 3.0, 4.0 };

    TEST_ASSERT(SF_precision_guard_verify_f64(cand_f64_ok, ref_f64, 4, 1e-6) == 1,
                "F64 candidate within epsilon must pass");
    TEST_ASSERT(SF_precision_guard_verify_f64(cand_f64_bad, ref_f64, 4, 1e-6) == 0,
                "F64 candidate exceeding epsilon must fail");

    // Exact Algebraic bitwise check
    uint8_t buf_a[16] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t buf_b[16] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t buf_c[16] = { 0x11, 0x22, 0x33, 0x45 }; // 1-bit difference

    TEST_ASSERT(SF_precision_guard_verify_exact(buf_a, buf_b, 16) == 1, "Identical buffers must pass exact verification");
    TEST_ASSERT(SF_precision_guard_verify_exact(buf_a, buf_c, 16) == 0, "Different buffers must fail exact verification");

    // 2. Java Bytecode Method / Descriptor Verifier
    char reason[256]{ 0 };

    // Float methods -> VALID
    TEST_ASSERT(SF_precision_verifier_verify_descriptor("(FF)F", 1e-4f, reason, sizeof(reason)) == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "(FF)F must be valid for loose precision");
    TEST_ASSERT(SF_precision_verifier_verify_descriptor("(D)V", 1e-4f, reason, sizeof(reason)) == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "(D)V must be valid for loose precision");
    TEST_ASSERT(SF_precision_verifier_verify_descriptor("([F)F", 1e-4f, reason, sizeof(reason)) == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "([F)F must be valid for loose precision");

    // Non-FP methods -> REJECTED with ERR_TYPE_NOT_FP (IncompatiblePrecisionTargetException)
    TEST_ASSERT(SF_precision_verifier_verify_descriptor("(Ljava/lang/String;)V", 1e-4f, reason, sizeof(reason)) == static_cast<int>(RedletGuardStatus::ERR_TYPE_NOT_FP),
                "String method must reject loose precision");
    TEST_ASSERT(strstr(reason, "IncompatiblePrecisionTargetException") != nullptr,
                "Reason must mention IncompatiblePrecisionTargetException");

    TEST_ASSERT(SF_precision_verifier_verify_descriptor("(II)I", 1e-4f, reason, sizeof(reason)) == static_cast<int>(RedletGuardStatus::ERR_TYPE_NOT_FP),
                "Integer method must reject loose precision");

    // Exact Algebraic (0.0 epsilon) is allowed for any method
    TEST_ASSERT(SF_precision_verifier_verify_descriptor("(Ljava/lang/String;)V", 0.0f, reason, sizeof(reason)) == static_cast<int>(RedletGuardStatus::GUARD_OK),
                "Exact algebraic (0.0 eps) is valid for any signature");

    // Method validator C API
    char err_buf[256]{ 0 };
    int v_ok = SF_precision_verifier_validate_method("com.example.Matrix", "mul", "(FF)F", 1e-4f, err_buf, sizeof(err_buf));
    TEST_ASSERT(v_ok == 1, "Validate method on float signature must return 1");

    int v_fail = SF_precision_verifier_validate_method("com.example.User", "setName", "(Ljava/lang/String;)V", 1e-4f, err_buf, sizeof(err_buf));
    TEST_ASSERT(v_fail == 0, "Validate method on non-FP signature must return 0");
    TEST_ASSERT(strstr(err_buf, "IncompatiblePrecisionTargetException") != nullptr, "Error buf must format exception");

    // RAII Scope Guard
    {
        RedletPrecisionScope scope(1e-4f);
        TEST_ASSERT(scope.is_active(), "Scope must be active");
        TEST_ASSERT(fabsf(scope.active_epsilon() - 1e-4f) < 1e-7f, "Scope epsilon must be 1e-4");
    }

    printf("  -> PASS: Precision Guard and Java Descriptor Verifier verified.\n");
}

// ============================================================================
// Test 9: Self-Advancement Central Orchestrator, Hot-Swap & Rollback
// ============================================================================
static void test_orchestrator_and_hot_swap() {
    printf("[TEST 9] Central Orchestrator, Pausepoint Hot-Swap & Rollback...\n");

    SF_self_advance_init();
    TEST_ASSERT(SF_self_advance_is_running() == 1, "Engine must be running after init");

    // Test periodic tick
    SF_self_advance_tick(500 * 1024 * 1024, 600 * 1024 * 1024, 1000 * 1024 * 1024, 50.0);
    SF_self_advance_tick(850 * 1024 * 1024, 900 * 1024 * 1024, 1000 * 1024 * 1024, 600.0);

    RedletSelfAdvancementStats stats = SF_self_advance_query_stats();
    TEST_ASSERT(stats.total_ticks == 2, "Total ticks must be 2");
    TEST_ASSERT(stats.bitmap_evolutions == 1, "Bitmap evolution must have executed in tick");

    // Test Lane Stealing
    int mig_ok = SF_self_advance_migrate_gc_ecore();
    TEST_ASSERT(mig_ok == 1, "Lane migration must succeed on hybrid CPU");
    stats = SF_self_advance_query_stats();
    TEST_ASSERT(stats.lane_migrations == 1, "Lane migration count must be 1");

    int rest_ok = SF_self_advance_restore_gc_affinity();
    TEST_ASSERT(rest_ok == 1, "Lane affinity restoration must succeed");

    // Test Pausepoint Hot-Swap
    void* dummy_kernel_old = (void*)0x1111;
    void* dummy_kernel_new = (void*)0x2222;
    std::atomic<void*> target_ptr{ dummy_kernel_old };

    bool swap_ok = g_redlet_self_advancement.hot_swap().hot_swap_routine(&target_ptr, dummy_kernel_new, "gc_sweep");
    TEST_ASSERT(swap_ok, "Hot swap must succeed");
    TEST_ASSERT(target_ptr.load() == dummy_kernel_new, "Target pointer must be updated");
    TEST_ASSERT(g_redlet_self_advancement.hot_swap().get_hot_swaps_completed() == 1, "Hot swaps count must be 1");

    // Test Deterministic Rollback
    SF_self_advance_rollback("Testing emergency recovery");
    stats = SF_self_advance_query_stats();
    TEST_ASSERT(stats.rollbacks == 1, "Rollback count must be 1");
    TEST_ASSERT(stats.active_bitmap_kernel == static_cast<uint8_t>(RedletBitmapScanKernel::SCALAR_BASELINE),
                "Active bitmap kernel must be rolled back to SCALAR_BASELINE");
    TEST_ASSERT(stats.active_memzero_strategy == static_cast<uint8_t>(RedletMemzeroStrategy::LIBC_MEMSET),
                "Active memzero strategy must be rolled back to LIBC_MEMSET");

    printf("  -> PASS: Central Orchestrator, Hot-Swap, and Rollback verified.\n");
}

// ============================================================================
// Test 10: Self-Healing Integration & Autonomous Rollback Bridge
// ============================================================================
static void test_self_heal_integration() {
    printf("[TEST 10] Self-Healing Integration (EmptyBox chain rebuild & rollback trigger)...\n");

    g_sf_self_heal.reset();

    // Create a 4-node EmptyBox chain with intentional corruptions:
    // Node 0: good
    // Node 1: bad magic, dangling forwarding address
    // Node 2: broken prev pointer
    // Node 3: cyclic loop back to Node 1 (Floyd cycle)
    SF_custominiVm_emptybox_header_t nodes[4]{};

    nodes[0].sfMN = SF_EMPTYBOX_MAGIC;
    nodes[0].next = &nodes[1];
    nodes[0].prev = nullptr;

    nodes[1].sfMN = 0xDEADBEEF; // corrupt magic
    nodes[1].next = &nodes[2];
    nodes[1].prev = &nodes[0];
    nodes[1].forwarding_address.store((void*)0x123, std::memory_order_relaxed); // dangling < 0x10000

    nodes[2].sfMN = SF_EMPTYBOX_MAGIC;
    nodes[2].next = &nodes[3];
    nodes[2].prev = reinterpret_cast<SF_custominiVm_emptybox_header_t*>(static_cast<uintptr_t>(0xBAADF00D)); // broken prev

    nodes[3].sfMN = SF_EMPTYBOX_MAGIC;
    nodes[3].next = &nodes[1]; // cycle!
    nodes[3].prev = &nodes[2];

    int rebuilt = SF_custominiVm_self_heal_rebuild_emptyboxes(&nodes[0], 1024);
    TEST_ASSERT(rebuilt == 1, "EmptyBox chain rebuild must report modifications");

    // Verify repairs
    TEST_ASSERT(nodes[1].sfMN == SF_EMPTYBOX_MAGIC, "Node 1 magic must be repaired to SF_EMPTYBOX_MAGIC");
    TEST_ASSERT(nodes[1].forwarding_address.load() == nullptr, "Node 1 dangling fwd pointer must be cleared");
    TEST_ASSERT(nodes[2].prev == &nodes[1], "Node 2 prev pointer must be repaired to &nodes[1]");

    // Verify cycle was severed
    TEST_ASSERT(nodes[3].next == nullptr || nodes[1].next == nullptr, "Cycle must be severed");

    SF_custominiVm_self_heal_stats_t heal_stats = SF_custominiVm_self_heal_query_stats();
    TEST_ASSERT(heal_stats.header_magics_repaired >= 1, "Header magics repaired must be >= 1");
    TEST_ASSERT(heal_stats.brooks_cycles_broken >= 1, "Brooks cycles broken must be >= 1");
    TEST_ASSERT(heal_stats.emptybox_chains_rebuilt >= 1, "EmptyBox chains rebuilt must be >= 1");

    // Test Bitcode Exception Shielding
    int shield_ret = SF_custominiVm_self_heal_shield_bitcode((void*)0x4000, 0xC0000005);
    TEST_ASSERT(shield_ret == 1, "Shield bitcode exception must succeed");
    heal_stats = SF_custominiVm_self_heal_query_stats();
    TEST_ASSERT(heal_stats.bitcode_exceptions_shielded == 1, "Bitcode exceptions shielded must be 1");

    // Test Autonomous Rollback Trigger at threshold (>= 20 faults)
    uint64_t rollbacks_before = g_redlet_self_advancement.hot_swap().get_rollbacks_executed();

    // Accumulate faults up to 25
    for (int k = 0; k < 25; ++k) {
        g_sf_self_heal.shield_bitcode_exception((void*)0x5000, 0x1234);
    }

    uint64_t rollbacks_after = g_redlet_self_advancement.hot_swap().get_rollbacks_executed();
    TEST_ASSERT(rollbacks_after == rollbacks_before + 1, "Autonomous rollback must be triggered exactly once upon crossing threshold");

    printf("  -> PASS: Self-Healing Integration verified.\n");
}

// ============================================================================
// Test 11: RedletCoach Plan 4 Integration (GC Ops, Affinity, Self-Advance)
// ============================================================================
static void test_coach_operations() {
    printf("[TEST 11] RedletCoach Plan 4 Lifecycle (GC Ops, Thread Affinity, Self-Advance)...\n");

    g_redlet_coach.reset();
    TEST_ASSERT(g_redlet_coach.get_gc_coach_ticks() == 0, "Coach GC ticks start at 0");
    TEST_ASSERT(g_redlet_coach.get_affinity_coach_ticks() == 0, "Coach affinity ticks start at 0");
    TEST_ASSERT(g_redlet_coach.get_self_advance_ticks() == 0, "Coach self-advance ticks start at 0");

    // 1. Test coach_gc_operations
    SF_coach_gc_operations();
    TEST_ASSERT(g_redlet_coach.get_gc_coach_ticks() == 1, "GC coach ticks must be 1");
    TEST_ASSERT(g_redlet_bitmap_evolver.get_evolution_count() >= 1, "Bitmap evolver must be invoked");

    // 2. Test coach_thread_affinity
    SF_coach_thread_affinity();
    TEST_ASSERT(g_redlet_coach.get_affinity_coach_ticks() == 1, "Affinity coach ticks must be 1");

    // 3. Test self_advance_evaluate
    SF_coach_self_advance_evaluate();
    TEST_ASSERT(g_redlet_coach.get_self_advance_ticks() == 1, "Self-advance coach ticks must be 1");

    // 4. Test reset
    SF_coach_reset();
    TEST_ASSERT(g_redlet_coach.get_gc_coach_ticks() == 0, "Coach GC ticks must be 0 after reset");

    printf("  -> PASS: RedletCoach Plan 4 verification passed.\n");
}

// ============================================================================
// MAIN ENTRYPOINT
// ============================================================================
int main() {
    printf("=================================================================\n");
    printf("oneJDK-25 RedletGC Self-Advancement & Self-Heal Deep Verification\n");
    printf("=================================================================\n");

    test_tsc_hooks_and_telemetry();
    test_falline_analyzer();
    test_cache_pressure_tracker();
    test_bitmap_evolver();
    test_memory_reset_heuristics();
    test_rmrp_heuristics();
    test_resurrection_heuristics();
    test_precision_guard_and_verifier();
    test_orchestrator_and_hot_swap();
    test_self_heal_integration();
    test_coach_operations();

    printf("=================================================================\n");
    printf("ALL 11 VERIFICATION SUITES PASSED SUCCESSFULLY (Exit Code 0).\n");
    printf("=================================================================\n");
    return 0;
}

#endif // REDLET_STANDALONE_BUILD

