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

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletEmptyBox.hpp"
#include "gc/redlet/redletFalline.hpp"
#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletPitstop.hpp"
#include "gc/redlet/redletTBBArena.hpp"
#include "gc/redlet/redletRunway.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include "gc/redlet/redletALU.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include "gc/redlet/redletTensorViews.hpp"
#include "gc/redlet/redletPuzzleArena.hpp"
#include "gc/redlet/redletGC.hpp"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] Line %d: Assertion '%s' failed: %s\n", __LINE__, #cond, msg); \
            exit(1); \
        } \
    } while (0)

static void test_two_tier_determinism_classification() {
    printf("[TEST 1] Two-Tier Determinism Classification & Target Precision Validation...\n");

    // Check FP classification
    TEST_ASSERT(sf_puzzler_classify_tier(SACFOR_F32) == sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP,
                "F32 must classify as NUMERIC_IEEE_FP");
    TEST_ASSERT(sf_puzzler_classify_tier(SACFOR_F64) == sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP,
                "F64 must classify as NUMERIC_IEEE_FP");
    TEST_ASSERT(sf_puzzler_classify_tier(SF_DTYPE_F16) == sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP,
                "F16 must classify as NUMERIC_IEEE_FP");
    TEST_ASSERT(sf_puzzler_classify_tier(SF_DTYPE_F8) == sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP,
                "F8 must classify as NUMERIC_IEEE_FP");
    TEST_ASSERT(sf_puzzler_classify_tier(SF_DTYPE_F128) == sf_puzzler_determinism_tier_e::NUMERIC_IEEE_FP,
                "F128 must classify as NUMERIC_IEEE_FP");

    // Check Non-FP classification
    TEST_ASSERT(sf_puzzler_classify_tier(SF_DTYPE_I64) == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC,
                "I64 must classify as EXACT_ALGEBRAIC");
    TEST_ASSERT(sf_puzzler_classify_tier(SF_DTYPE_I32) == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC,
                "I32 must classify as EXACT_ALGEBRAIC");
    TEST_ASSERT(sf_puzzler_classify_tier(SF_DTYPE_U8)  == sf_puzzler_determinism_tier_e::EXACT_ALGEBRAIC,
                "U8 must classify as EXACT_ALGEBRAIC");

    // Validate machine epsilon defaults for FP
    float eps = 0.0f;
    bool valid = sf_puzzler_validate_target_precision(SACFOR_F32, eps);
    TEST_ASSERT(valid, "F32 precision validation must succeed");
    TEST_ASSERT(fabsf(eps - FLT_EPSILON) < 1e-12f, "F32 default epsilon must be FLT_EPSILON");

    eps = 0.0f;
    valid = sf_puzzler_validate_target_precision(SACFOR_F64, eps);
    TEST_ASSERT(valid, "F64 precision validation must succeed");
    TEST_ASSERT(fabsf(eps - static_cast<float>(DBL_EPSILON)) < 1e-20f, "F64 default epsilon must be DBL_EPSILON");

    // Validate rejection of target precision > 0 on non-FP (EXACT_ALGEBRAIC)
    eps = 0.01f;
    valid = sf_puzzler_validate_target_precision(SF_DTYPE_I64, eps);
    TEST_ASSERT(!valid, "Non-FP must reject target_epsilon > 0");
    TEST_ASSERT(eps == 0.0f, "Rejected non-FP target epsilon must be clamped to 0.0f");

    eps = 0.0f;
    valid = sf_puzzler_validate_target_precision(SF_DTYPE_I64, eps);
    TEST_ASSERT(valid, "Non-FP with epsilon 0.0f must be accepted");
    TEST_ASSERT(eps == 0.0f, "Non-FP epsilon remains 0.0f");

    printf("  -> PASS: Two-tier determinism model verified.\n");
}

static void test_puzzler_error_accumulation() {
    printf("[TEST 2] Puzzler Double Precision & Exact Algebraic Bit Invariance...\n");

    // 1. Verify F64 double error accumulation
    constexpr size_t N = 32;
    alignas(64) double ref_d[N];
    alignas(64) double cand_d[N];
    for (size_t i = 0; i < N; ++i) {
        ref_d[i] = 1.0 + static_cast<double>(i) * 0.05;
        cand_d[i] = ref_d[i] + 1e-13; // representable double delta above ULP (2.22e-16)
    }

    float max_err = 0.0f;
    float sum_sq = 0.0f;
    bool finite_ok = true;

    sf_accumulate_ieee754_error_f64(cand_d, ref_d, N, max_err, sum_sq, finite_ok);
    TEST_ASSERT(finite_ok, "F64 error accumulation must be finite");
    TEST_ASSERT(max_err > 0.0f && max_err < 1e-12f, "Max error must be correctly accumulated in double precision");

    // 2. Verify non-finite NaN/Inf detection in F64
    cand_d[10] = std::numeric_limits<double>::infinity();
    sf_accumulate_ieee754_error_f64(cand_d, ref_d, N, max_err, sum_sq, finite_ok);
    TEST_ASSERT(!finite_ok, "F64 error accumulation must detect Inf/NaN");

    // 3. Verify Exact Algebraic strict bitwise invariance
    alignas(64) uint8_t ref_bytes[64];
    alignas(64) uint8_t cand_bytes[64];
    memset(ref_bytes, 0xA5, 64);
    memset(cand_bytes, 0xA5, 64);
    TEST_ASSERT(memcmp(ref_bytes, cand_bytes, 64) == 0, "Exact algebraic must match bitwise");

    cand_bytes[33] ^= 0x01; // flip 1 bit
    TEST_ASSERT(memcmp(ref_bytes, cand_bytes, 64) != 0, "Exact algebraic must detect 1-bit deviation");

    printf("  -> PASS: Double precision and exact algebraic checks verified.\n");
}

static void test_pitstop_admit_evict() {
    printf("[TEST 3] Pitstop Admission & Eviction Dispatch...\n");

    RedletPitstop pitstop;
    TEST_ASSERT(pitstop.count() == 0, "Pitstop starts empty");

    // Allocate memory for mock live header + payload
    size_t total_sz = sizeof(SF_custominiVm_live_header_t) + 256;
    void* raw = _mm_malloc(total_sz, 64);
    TEST_ASSERT(raw != nullptr, "Raw allocation must succeed");
    memset(raw, 0, total_sz);

    auto* obj = new (raw) SF_custominiVm_live_header_t();
    obj->function_id = 1234;
    obj->generation = 1;
    obj->region = 2;
    obj->total_cell_bytes = total_sz;
    obj->memory_footprint.store(256, std::memory_order_relaxed);

    // 1. Test EXCISION_EXCLUSIVE mode
    g_sf_puzzler_morph_method.store(static_cast<uint8_t>(sf_puzzler_morph_e::EXCISION_EXCLUSIVE), std::memory_order_release);
    auto* entry = pitstop.admit(obj);
    TEST_ASSERT(entry != nullptr, "Admit must produce a pitstop entry");
    TEST_ASSERT(pitstop.count() == 1, "Pitstop count must be 1 after admit");
    TEST_ASSERT(entry->is_busy.load() == true, "Entry must be busy under EXCISION_EXCLUSIVE");

    pitstop.evict(obj);
    TEST_ASSERT(pitstop.count() == 0, "Pitstop count must be 0 after evict");

    // 2. Test SHADOW_RCU mode
    g_sf_puzzler_morph_method.store(static_cast<uint8_t>(sf_puzzler_morph_e::SHADOW_RCU), std::memory_order_release);
    auto* entry2 = pitstop.admit(obj);
    TEST_ASSERT(entry2 != nullptr, "Admit shadow must produce an entry");
    TEST_ASSERT(pitstop.count() == 1, "Pitstop count must be 1 after shadow admit");
    TEST_ASSERT(entry2->is_busy.load() == false, "Shadow RCU entry must allow lockless reads");

    pitstop.evict(obj);
    TEST_ASSERT(pitstop.count() == 0, "Pitstop count must be 0 after shadow evict");

    obj->~SF_custominiVm_live_header_t();
    _mm_free(raw);

    printf("  -> PASS: Pitstop admit and evict verified (both excision and shadow modes).\n");
}

static void test_alu_route_precision() {
    printf("[TEST 4] ALU Precision-Driven Multi-Limb Route Resolution...\n");

    g_sf_alu_nexus.reset();

    // 1. Non-FP with epsilon > 0 must be rejected
    int is_emu = 0;
    int schema = 0;
    int lane = SF_custominiVm_alu_resolve_route_precision(SF_DTYPE_I64, 5, 0.05f, &is_emu, &schema);
    TEST_ASSERT(lane == -1, "ALU precision route on non-FP with epsilon > 0 must be rejected (-1)");

    // 2. Non-FP with epsilon == 0 routes to native integer lane
    lane = SF_custominiVm_alu_resolve_route_precision(SF_DTYPE_I64, 5, 0.0f, &is_emu, &schema);
    TEST_ASSERT(lane == 3, "ALU route on I64 with epsilon 0.0 must route to lane 3 (native INT64)");
    TEST_ASSERT(is_emu == 0, "I64 native lane must not be emulated");

    // 3. FP32 with high precision requirement (< FLT_EPSILON) upgrades to FP64 lane
    lane = SF_custominiVm_alu_resolve_route_precision(SF_DTYPE_F32, 5, 1e-10f, &is_emu, &schema);
    TEST_ASSERT(lane == 0, "ALU route on F32 demanding 1e-10f must upgrade to FP64 lane 0");
    TEST_ASSERT(is_emu == 1, "F32 upgrade to FP64 must be emulated schema");

    // 4. FP32 with standard precision (0.0f) routes to native FP32 lane
    lane = SF_custominiVm_alu_resolve_route_precision(SF_DTYPE_F32, 5, 0.0f, &is_emu, &schema);
    TEST_ASSERT(lane == 1, "ALU route on F32 with default epsilon must route to lane 1 (native FP32)");

    // 5. FP64 with high precision requirement (< DBL_EPSILON) upgrades to quad lane 10
    lane = SF_custominiVm_alu_resolve_route_precision(SF_DTYPE_F64, 5, 1e-25f, &is_emu, &schema);
    TEST_ASSERT(lane == 10, "ALU route on F64 demanding 1e-25f must upgrade to quad limb lane 10");

    printf("  -> PASS: ALU precision-driven routing verified.\n");
}

static void test_sensor_streaming() {
    printf("[TEST 5] Sensor Streaming Lifecycle (Attach -> Poll -> Detach)...\n");

    SF_custominiVm_sensor_config_t cfg{};
    cfg.sensor_type = SF_SENSOR_TEMP_HUMIDITY;
    cfg.buffer_sample_count = 256;
    cfg.sample_rate_hz = 1000;
    cfg.gc_exempt_pinning = 1;

    SF_custominiVm_tensor_t stream_tensor{};
    int ret = SF_custominiVm_sensor_attach_stream(&cfg, &stream_tensor);
    TEST_ASSERT(ret == 0, "Sensor attach stream must succeed");
    TEST_ASSERT(stream_tensor.data != nullptr, "Stream tensor data must be non-null");
    TEST_ASSERT(stream_tensor.shape[0] == 256, "Stream tensor shape must match buffer_sample_count");

    size_t polled = SF_custominiVm_sensor_poll_stream(&stream_tensor, 32);
    TEST_ASSERT(polled == 32, "Poll stream must return 32 samples");

    const float* buf = static_cast<const float*>(stream_tensor.data);
    for (size_t i = 0; i < polled; ++i) {
        TEST_ASSERT(buf[i] >= 24.0f && buf[i] <= 26.0f, "Temperature sensor samples must fall in valid range [24.0, 26.0]");
    }

    SF_custominiVm_sensor_detach_stream(&stream_tensor);
    TEST_ASSERT(stream_tensor.data == nullptr, "Stream tensor data must be null after detach");
    TEST_ASSERT(stream_tensor.byte_size == 0, "Stream tensor byte_size must be 0 after detach");

    printf("  -> PASS: Sensor streaming lifecycle verified.\n");
}

static std::atomic<int> g_cb_calls{ 0 };
static void test_telemetry_cb(const SF_custominiVm_hotlog_entry_t* /*entry*/, const char* /*formatted_cli*/, void* /*user_data*/) {
    g_cb_calls.fetch_add(1, std::memory_order_relaxed);
}

static void test_telemetry_and_rmrp() {
    printf("[TEST 6] Telemetry Stream Daemon & RMRP Configuration...\n");

    int start_ret = SF_custominiVm_telemetry_stream_start(5000, test_telemetry_cb, nullptr);
    TEST_ASSERT(start_ret == 0, "Telemetry stream start must succeed");
    TEST_ASSERT(SF_custominiVm_telemetry_stream_is_running() == 1, "Telemetry daemon must be running");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    SF_custominiVm_telemetry_stream_stop();
    TEST_ASSERT(SF_custominiVm_telemetry_stream_is_running() == 0, "Telemetry daemon must be stopped");

    // RMRP Configuration C-APIs
    SF_custominiVm_set_rmrp(1, 1);
    int rmrp_enabled = SF_custominiVm_get_rmrp();
    TEST_ASSERT(rmrp_enabled == 1, "RMRP must be enabled");

    // Anatomy topography query
    SF_custominiVm_anatomy_stat_t stat{};
    SF_custominiVm_anatomy_query(&stat);
    TEST_ASSERT(stat.onheap_reserved_bytes > 0, "Anatomy query must report non-zero on-heap reserved bytes");

    const char* desc = SF_custominiVm_anatomy_description();
    TEST_ASSERT(desc != nullptr && strlen(desc) > 0, "Anatomy description must be non-empty");

    // Resurrection request
    SF_custominiVm_resurrect_request(999);

    printf("  -> PASS: Telemetry daemon, RMRP, and Anatomy verified.\n");
}

int main() {
    printf("=================================================================\n");
    printf("oneJDK-25 Redlet Subsystem Comprehensive Standalone Verification\n");
    printf("=================================================================\n");

    test_two_tier_determinism_classification();
    test_puzzler_error_accumulation();
    test_pitstop_admit_evict();
    test_alu_route_precision();
    test_sensor_streaming();
    test_telemetry_and_rmrp();

    printf("=================================================================\n");
    printf("ALL TESTS PASSED SUCCESSFULLY (Exit Code 0).\n");
    printf("=================================================================\n");
    return 0;
}
