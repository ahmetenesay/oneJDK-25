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

#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/simd/redjetSIMD.hpp"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <algorithm>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_MSC_VER)
#include <cpuid.h>
#endif

/* Global ISA Dispatch Table and Pool Instances */
sacculus_dispatch_table_t g_hs_dispatch_table;
sacculus_instance_pool_t  g_hs_instance_pool;

namespace {

static inline void hs_dispatch_flush_icache(void* addr, size_t len) noexcept {
    if (!addr || len == 0) return;
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), addr, len);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(static_cast<char*>(addr), static_cast<char*>(addr) + len);
#endif
}

static inline void hs_dispatch_full_fence() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

struct hs_batch_scope_frame_t {
    uint32_t group_id{ 0 };
    uint8_t  priority{ 5 };
    bool     bypass_warmup{ false };
};

static std::atomic<uint32_t> g_hs_group_counter{ 0x1000u };
static std::atomic<uint32_t> g_hs_active_batch_id{ 0 };

static thread_local uint32_t               tl_hs_group_hex_id        = 0;
static thread_local uint8_t                tl_hs_group_priority      = 5;
static thread_local bool                   tl_hs_group_bypass_warmup = false;
static thread_local hs_batch_scope_frame_t tl_hs_batch_stack[16]{};
static thread_local uint32_t               tl_hs_batch_depth         = 0;

} // namespace

uint32_t hs_get_thread_group_hex_id() noexcept        { return tl_hs_group_hex_id; }
uint8_t  hs_get_thread_group_priority() noexcept      { return tl_hs_group_priority; }
bool     hs_get_thread_group_bypass_warmup() noexcept { return tl_hs_group_bypass_warmup; }

/* ========================================================================= */
/* 1. LOCK-FREE CAS INSTANCE POOL & SEQLOCK KERNEL DISPATCH TABLE            */
/* ========================================================================= */

int32_t sacculus_instance_pool_t::acquire(sacculus_live_header_t* obj) noexcept {
    if (used_count.load(std::memory_order_relaxed) >= SACFOR_MAX_INSTANCES) return -1;
    for (uint32_t i = 0; i < SACFOR_MAX_INSTANCES; ++i) {
        int expected = 1;
        if (slots[i].free.compare_exchange_weak(expected, 0,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            slots[i].obj = obj;
            used_count.fetch_add(1, std::memory_order_relaxed);
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void sacculus_instance_pool_t::release(int32_t idx) noexcept {
    if (idx < 0 || static_cast<uint32_t>(idx) >= SACFOR_MAX_INSTANCES) return;
    int expected = 0;
    if (slots[idx].free.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        slots[idx].obj = nullptr;
        used_count.fetch_sub(1, std::memory_order_relaxed);
    }
}

sacculus_dispatch_entry_t* sacculus_dispatch_table_t::get(uint32_t fn_id) noexcept {
    if (fn_id >= SACFOR_MAX_DISPATCH_SLOTS) return nullptr;
    return &entries[fn_id];
}

void sacculus_dispatch_table_t::register_fn(uint32_t fn_id, sacculus_kernel_fn_t fn) noexcept {
    if (fn_id >= SACFOR_MAX_DISPATCH_SLOTS) return;
    auto& entry = entries[fn_id];
    entry.seqlock_version.fetch_add(1, std::memory_order_acq_rel);

    entry.function_id = fn_id;
    entry.fn.store(fn, std::memory_order_relaxed);
    entry.best_cycles = UINT64_MAX;
    entry.call_count  = 0;

    if (fn) {
        hs_dispatch_flush_icache(reinterpret_cast<void*>(fn), 64);
    }
    hs_dispatch_full_fence();

    entry.seqlock_version.fetch_add(1, std::memory_order_release);
}

void sacculus_dispatch_table_t::swap(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t best) noexcept {
    if (fn_id >= SACFOR_MAX_DISPATCH_SLOTS) return;
    auto& entry = entries[fn_id];
    entry.seqlock_version.fetch_add(1, std::memory_order_acq_rel);

    entry.fn.store(new_fn, std::memory_order_relaxed);
    entry.best_cycles = best;

    if (new_fn) {
        hs_dispatch_flush_icache(reinterpret_cast<void*>(new_fn), 64);
    }
    hs_dispatch_full_fence();

    entry.seqlock_version.fetch_add(1, std::memory_order_release);
}

sacculus_kernel_fn_t sacculus_dispatch_table_t::read_fn_seqlock(uint32_t fn_id, uint64_t* out_best_cycles) const noexcept {
    if (fn_id >= SACFOR_MAX_DISPATCH_SLOTS) return nullptr;
    const auto& entry = entries[fn_id];
    sacculus_kernel_fn_t f = nullptr;
    uint64_t best = UINT64_MAX;
    while (true) {
        uint32_t v1 = entry.seqlock_version.load(std::memory_order_acquire);
        if ((v1 & 1u) != 0u) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#endif
            continue;
        }
        f    = entry.fn.load(std::memory_order_relaxed);
        best = entry.best_cycles;
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t v2 = entry.seqlock_version.load(std::memory_order_relaxed);
        if (v1 == v2) break;
    }
    if (out_best_cycles) *out_best_cycles = best;
    return f;
}

void sacculus_dispatch_table_t::init_default_kernels() noexcept {
    static std::atomic<bool> initialized{ false };
    if (initialized.exchange(true, std::memory_order_acq_rel)) return;

    // Register FP32 Vector Kernels
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_F32, hs_kernel_vec_add_f32);
    register_fn((HS_VEC_OP_SUB << 8) | HS_DTYPE_F32, hs_kernel_vec_sub_f32);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_F32, hs_kernel_vec_mul_f32);
    register_fn((HS_VEC_OP_FMA << 8) | HS_DTYPE_F32, hs_kernel_vec_fma_f32);
    register_fn((HS_VEC_OP_DOT << 8) | HS_DTYPE_F32, hs_kernel_vec_dot_f32);
    register_fn((HS_VEC_OP_MIN << 8) | HS_DTYPE_F32, hs_kernel_vec_min_f32);
    register_fn((HS_VEC_OP_MAX << 8) | HS_DTYPE_F32, hs_kernel_vec_max_f32);

    // Register FP64 Vector Kernels
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_F64, hs_kernel_vec_add_f64);
    register_fn((HS_VEC_OP_SUB << 8) | HS_DTYPE_F64, hs_kernel_vec_sub_f64);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_F64, hs_kernel_vec_mul_f64);
    register_fn((HS_VEC_OP_FMA << 8) | HS_DTYPE_F64, hs_kernel_vec_fma_f64);
    register_fn((HS_VEC_OP_DOT << 8) | HS_DTYPE_F64, hs_kernel_vec_dot_f64);
    register_fn((HS_VEC_OP_MIN << 8) | HS_DTYPE_F64, hs_kernel_vec_min_f64);
    register_fn((HS_VEC_OP_MAX << 8) | HS_DTYPE_F64, hs_kernel_vec_max_f64);

    // Register INT32 Vector Kernels
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_I32, hs_kernel_vec_add_i32);
    register_fn((HS_VEC_OP_SUB << 8) | HS_DTYPE_I32, hs_kernel_vec_sub_i32);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_I32, hs_kernel_vec_mul_i32);
    register_fn((HS_VEC_OP_AND << 8) | HS_DTYPE_I32, hs_kernel_vec_and_i32);
    register_fn((HS_VEC_OP_OR  << 8) | HS_DTYPE_I32, hs_kernel_vec_or_i32);
    register_fn((HS_VEC_OP_XOR << 8) | HS_DTYPE_I32, hs_kernel_vec_xor_i32);

    // Register INT64 Vector Kernels
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_I64, hs_kernel_vec_add_i64);
    register_fn((HS_VEC_OP_SUB << 8) | HS_DTYPE_I64, hs_kernel_vec_sub_i64);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_I64, hs_kernel_vec_mul_i64);
    register_fn((HS_VEC_OP_AND << 8) | HS_DTYPE_I64, hs_kernel_vec_and_i64);
    register_fn((HS_VEC_OP_OR  << 8) | HS_DTYPE_I64, hs_kernel_vec_or_i64);
    register_fn((HS_VEC_OP_XOR << 8) | HS_DTYPE_I64, hs_kernel_vec_xor_i64);

    // Register Multi-Limb Emulation Kernels
    register_fn((HS_VEC_OP_MULTI_LIMB_ADD << 8) | HS_DTYPE_F128, hs_kernel_emu_double_double_add);
    register_fn((HS_VEC_OP_MULTI_LIMB_MUL << 8) | HS_DTYPE_F128, hs_kernel_emu_double_double_mul);
    register_fn((HS_VEC_OP_MULTI_LIMB_ADD << 8) | HS_DTYPE_F256, hs_kernel_emu_quad_double_add);
    register_fn((HS_VEC_OP_MULTI_LIMB_MUL << 8) | HS_DTYPE_F256, hs_kernel_emu_quad_double_mul);
    register_fn((HS_VEC_OP_MULTI_LIMB_ADD << 8) | HS_DTYPE_I128, hs_kernel_emu_int128_add);
    register_fn((HS_VEC_OP_MULTI_LIMB_MUL << 8) | HS_DTYPE_I128, hs_kernel_emu_int128_mul);
    register_fn((HS_VEC_OP_MULTI_LIMB_ADD << 8) | HS_DTYPE_I256, hs_kernel_emu_int256_add);
    register_fn((HS_VEC_OP_MULTI_LIMB_MUL << 8) | HS_DTYPE_I256, hs_kernel_emu_int256_mul);

    // Aliases for generic arithmetic operations on wide types
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_F128, hs_kernel_emu_double_double_add);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_F128, hs_kernel_emu_double_double_mul);
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_F256, hs_kernel_emu_quad_double_add);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_F256, hs_kernel_emu_quad_double_mul);
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_I128, hs_kernel_emu_int128_add);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_I128, hs_kernel_emu_int128_mul);
    register_fn((HS_VEC_OP_ADD << 8) | HS_DTYPE_I256, hs_kernel_emu_int256_add);
    register_fn((HS_VEC_OP_MUL << 8) | HS_DTYPE_I256, hs_kernel_emu_int256_mul);

    // Register GC Fast-Path Kernels
    register_fn((HS_VEC_OP_CARD_TABLE_SCAN << 8) | HS_DTYPE_U8, hs_kernel_gc_card_table_scan);
    register_fn((HS_VEC_OP_MEMZERO_FAST << 8) | HS_DTYPE_U8,    hs_kernel_gc_memzero);
}

/* ========================================================================= */
/* 2. GLOBAL 65+ WORLD ISA CATALOG & FALLBACK HIERARCHY                      */
/* ========================================================================= */

static const hs_world_isa_entry_t g_world_isa_catalog[] = {
    // x86 / x86_64 Matrix & Vector Extensions
    { "AMX_COMPLEX",        "x86",       1024, "AMX_FP16" },
    { "AMX_FP16",           "x86",       1024, "AMX_TILE" },
    { "AMX_TILE",           "x86",       1024, "AVX512F" },
    { "AMX_BF16",           "x86",       1024, "AVX512_BF16" },
    { "AMX_INT8",           "x86",       1024, "AVX512VNNI" },
    { "AVX10",              "x86",        512, "AVX512F" },
    { "APX_F",              "x86",          0, "AVX2" },
    { "AVX_IFMA",           "x86",        512, "AVX512F" },
    { "AVX512_VP2INTERSECT","x86",        512, "AVX512F" },
    { "AVX512_BF16",        "x86",        512, "AVX512BW" },
    { "AVX512VBMI2",        "x86",        512, "AVX512VBMI" },
    { "AVX512VBMI",         "x86",        512, "AVX512BW" },
    { "INTEL_DLBOOST",      "x86",        512, "AVX512VNNI" },
    { "AVX512VNNI",         "x86",        512, "AVX512BW" },
    { "AVX512BITALG",       "x86",        512, "AVX512BW" },
    { "AVX512VPOPCNTDQ",    "x86",        512, "AVX512BW" },
    { "AVX512FP16",         "x86",        512, "AVX512BW" },
    { "AVX512BW",           "x86",        512, "AVX512DQ" },
    { "AVX512DQ",           "x86",        512, "AVX512F" },
    { "AVX512CD",           "x86",        512, "AVX512F" },
    { "AVX512VL",           "x86",        256, "AVX2" },
    { "AVX512F",            "x86",        512, "AVX2" },
    { "AVX_VNNI",           "x86",        256, "AVX2" },
    { "FMA3",               "x86",        256, "AVX" },
    { "FMA4",               "x86",        256, "AVX" },
    { "XOP",                "x86",        128, "AVX" },
    { "AVX2",               "x86",        256, "AVX" },
    { "F16C",               "x86",        128, "AVX" },
    { "AVX",                "x86",        256, "SSE4_2" },
    { "BMI2",               "x86",          0, "BMI1" },
    { "BMI1",               "x86",          0, "SSE4_2" },
    { "SSE4_2",             "x86",        128, "SSE4_1" },
    { "SSE4_1",             "x86",        128, "SSSE3" },
    { "SSE4A",              "x86",        128, "SSE3" },
    { "SSSE3",              "x86",        128, "SSE3" },
    { "SSE3",               "x86",        128, "SSE2" },
    { "SSE2",               "x86",        128, "SSE" },
    { "SSE",                "x86",        128, "MMX" },
    { "MMX",                "x86",         64, "SCALAR" },

    // ARM / AArch64 / Apple Silicon
    { "SME2",               "ARM",       2048, "SME" },
    { "SME",                "ARM",       1024, "SVE2" },
    { "SVE2",               "ARM",        512, "SVE" },
    { "ARM_SVE_512",        "ARM",        512, "SVE" },
    { "ARM_SVE_256",        "ARM",        256, "SVE" },
    { "SVE",                "ARM",        512, "ARM_DOTPROD" },
    { "ARM_I8MM",           "ARM",        128, "NEON" },
    { "ARM_BF16",           "ARM",        128, "NEON" },
    { "ARM_DOTPROD",        "ARM",        128, "NEON" },
    { "ARM_FP16",           "ARM",        128, "NEON" },
    { "ARM_ASIMD_HP",       "ARM",        128, "NEON" },
    { "ASIMD",              "ARM",        128, "NEON" },
    { "NEON",               "ARM",        128, "ARM_V8" },
    { "ARM_LSE_ATOMICS",    "ARM",          0, "ARM_V8" },
    { "ARM_CRYPTO",         "ARM",        128, "NEON" },
    { "ARM_V81",            "ARM",          0, "ARM_V8" },
    { "ARM_V8",             "ARM",          0, "SCALAR" },
    { "APPLE_AMX",          "ARM",        512, "NEON" },

    // RISC-V & AI Accelerators
    { "TENSTORRENT_TENSIX", "RISC-V",    1024, "RVV" },
    { "ESPERANTO_ET_MAX",   "RISC-V",     512, "RVV" },
    { "RVV",                "RISC-V",     256, "RV64" },
    { "RV64",               "RISC-V",       0, "SCALAR" },

    // WebAssembly
    { "WASM_RELAXED_SIMD",  "WASM",       128, "WASM_SIMD128" },
    { "WASM_SIMD128",       "WASM",       128, "SCALAR" },

    // IBM POWER
    { "VSX3",               "POWER",      128, "VSX" },
    { "VSX",                "POWER",      128, "ALTIVEC" },
    { "ALTIVEC",            "POWER",      128, "SCALAR" },

    // MIPS
    { "MIPS_MSA",           "MIPS",       128, "MIPS64" },
    { "MIPS64",             "MIPS",         0, "SCALAR" },

    // LoongArch
    { "LASX",               "LOONGARCH",  256, "LSX" },
    { "LSX",                "LOONGARCH",  128, "LA64" },
    { "LA64",               "LOONGARCH",    0, "SCALAR" },

    // Manycore / VLIW
    { "SUNWAY_SW26010",     "SUNWAY",     512, "SCALAR" },
    { "KALRAY_MPPA_VLIW",   "KALRAY",     128, "SCALAR" },

    // IBM Z / S390X
    { "S390_NNPA",          "S390X",     1024, "S390_VXE2" },
    { "S390_VXE2",          "S390X",      128, "S390_VXE"  },
    { "S390_VXE",           "S390X",      128, "S390_VX"   },
    { "S390_VX",            "S390X",      128, "SCALAR"    },

    // Universal Scalar Base
    { "SCALAR",             "UNIVERSAL",    0, nullptr }
};

const hs_world_isa_entry_t* hs_find_world_isa(const char* name) noexcept {
    if (!name || !*name) return nullptr;
    for (const auto& item : g_world_isa_catalog) {
        if (_stricmp(item.isa_name, name) == 0) return &item;
    }
    return nullptr;
}

size_t hs_world_isa_catalog_count() noexcept {
    return sizeof(g_world_isa_catalog) / sizeof(g_world_isa_catalog[0]);
}

const hs_world_isa_entry_t* hs_world_isa_catalog_at(size_t idx) noexcept {
    if (idx >= hs_world_isa_catalog_count()) return nullptr;
    return &g_world_isa_catalog[idx];
}

size_t hs_resolve_isa_fallback_chain(const char* start_isa,
                                     const hs_world_isa_entry_t** out_chain,
                                     size_t max_depth) noexcept {
    if (!start_isa || !out_chain || max_depth == 0) return 0;
    size_t count = 0;
    const hs_world_isa_entry_t* cur = hs_find_world_isa(start_isa);
    while (cur && count < max_depth) {
        out_chain[count++] = cur;
        if (!cur->fallback_isa) break;
        cur = hs_find_world_isa(cur->fallback_isa);
    }
    return count;
}

/* ========================================================================= */
/* 3. STANDALONE PINNED FALLBACK WORKER THREAD POOL                          */
/* ========================================================================= */

hs_fallback_pool_t::hs_fallback_pool_t(int num_threads) : thread_count(num_threads) {
    if (thread_count <= 0) {
        auto* nexus = hs_get_hardware_nexus();
        if (nexus && nexus->initialized.load(std::memory_order_acquire)) {
            auto budget = nexus->smt.calculate_thread_budget();
            thread_count = static_cast<int>(budget.vm_threads);
        }
        if (thread_count <= 0) thread_count = static_cast<int>(std::thread::hardware_concurrency());
        if (thread_count <= 0) thread_count = 1;
    }

    for (int i = 0; i < thread_count; ++i) {
        worker_slots.push_back(std::make_unique<worker_slot_t>());
    }

    for (int i = 0; i < thread_count; ++i) {
        workers.emplace_back([this, i]() {
            (void)hs_thread_pin_to_core(i);

            while (true) {
                std::function<void()> cur_task;
                {
                    std::unique_lock<std::mutex> lock(worker_slots[i]->mtx);
                    worker_slots[i]->cv.wait(lock, [this, i]() {
                        if (stop.load(std::memory_order_acquire)) return true;
                        if (!worker_slots[i]->slot_tasks.empty()) return true;
                        std::unique_lock<std::mutex> qlock(queue_mtx);
                        return !tasks.empty();
                    });

                    if (!worker_slots[i]->slot_tasks.empty()) {
                        cur_task = std::move(worker_slots[i]->slot_tasks.front());
                        worker_slots[i]->slot_tasks.pop();
                    } else {
                        std::unique_lock<std::mutex> qlock(queue_mtx);
                        if (!tasks.empty()) {
                            cur_task = std::move(tasks.front());
                            tasks.pop();
                        } else if (stop.load(std::memory_order_acquire)) {
                            return;
                        }
                    }
                }

                if (cur_task) {
                    cur_task();
                    cur_task = nullptr;
                    if (active_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::unique_lock<std::mutex> lock(queue_mtx);
                        cv_done.notify_all();
                    }
                }
            }
        });
    }
}

hs_fallback_pool_t::~hs_fallback_pool_t() {
    {
        std::unique_lock<std::mutex> qlock(queue_mtx);
        stop.store(true, std::memory_order_release);
    }
    for (auto& slot : worker_slots) {
        std::unique_lock<std::mutex> slock(slot->mtx);
        slot->cv.notify_all();
    }
    cv_task.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }
}

void hs_fallback_pool_t::submit(std::function<void()> task) {
    active_tasks.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lock(queue_mtx);
        tasks.push(std::move(task));
    }
    cv_task.notify_one();
    for (auto& slot : worker_slots) {
        slot->cv.notify_one();
    }
}

void hs_fallback_pool_t::submit_to_worker(int worker_idx, std::function<void()> task) {
    if (worker_idx < 0 || worker_idx >= thread_count) {
        submit(std::move(task));
        return;
    }
    active_tasks.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lock(worker_slots[worker_idx]->mtx);
        worker_slots[worker_idx]->slot_tasks.push(std::move(task));
    }
    worker_slots[worker_idx]->cv.notify_one();
}

void hs_fallback_pool_t::wait_all() {
    if (active_tasks.load(std::memory_order_acquire) == 0) return;
    std::unique_lock<std::mutex> lock(queue_mtx);
    cv_done.wait(lock, [this]() {
        return active_tasks.load(std::memory_order_acquire) == 0;
    });
}

/* ========================================================================= */
/* 4. VECTOR ALU KERNELS (Built-in Hardware-Accelerated & Emulated)          */
/* ========================================================================= */

// Helper to extract element count from output tensor
static inline size_t hs_tensor_element_count(const HS_hotspot_tensor_t* t, size_t elem_size) {
    if (!t) return 0;
    if (t->byte_size > 0 && elem_size > 0) return t->byte_size / elem_size;
    if (t->shape[0] > 0) return static_cast<size_t>(t->shape[0]);
    return 0;
}

// ── FP32 Kernels ──

void hs_kernel_vec_add_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(float));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        _mm256_storeu_ps(dst + i, _mm256_add_ps(va, vb));
    }
#endif
    for (; i < count; ++i) dst[i] = a[i] + b[i];
}

void hs_kernel_vec_sub_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(float));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        _mm256_storeu_ps(dst + i, _mm256_sub_ps(va, vb));
    }
#endif
    for (; i < count; ++i) dst[i] = a[i] - b[i];
}

void hs_kernel_vec_mul_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(float));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(va, vb));
    }
#endif
    for (; i < count; ++i) dst[i] = a[i] * b[i];
}

void hs_kernel_vec_fma_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    const float* c = (nin >= 3 && in[2].data) ? static_cast<const float*>(in[2].data) : nullptr;
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(float));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__) && defined(__FMA__)
    for (; i + 8 <= count; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 vc = c ? _mm256_loadu_ps(c + i) : _mm256_loadu_ps(dst + i);
        _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(va, vb, vc));
    }
#endif
    for (; i < count; ++i) {
        float addend = c ? c[i] : dst[i];
        dst[i] = std::fma(a[i], b[i], addend);
    }
}

void hs_kernel_vec_dot_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&in[0], sizeof(float));
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) sum += a[i] * b[i];
    dst[0] = sum;
}

void hs_kernel_vec_min_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(float));
    for (size_t i = 0; i < count; ++i) dst[i] = std::min(a[i], b[i]);
}

void hs_kernel_vec_max_f32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const float* a = static_cast<const float*>(in[0].data);
    const float* b = static_cast<const float*>(in[1].data);
    float* dst     = static_cast<float*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(float));
    for (size_t i = 0; i < count; ++i) dst[i] = std::max(a[i], b[i]);
}

// ── FP64 Kernels ──

void hs_kernel_vec_add_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(double));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    for (; i + 4 <= count; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        _mm256_storeu_pd(dst + i, _mm256_add_pd(va, vb));
    }
#endif
    for (; i < count; ++i) dst[i] = a[i] + b[i];
}

void hs_kernel_vec_sub_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(double));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    for (; i + 4 <= count; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        _mm256_storeu_pd(dst + i, _mm256_sub_pd(va, vb));
    }
#endif
    for (; i < count; ++i) dst[i] = a[i] - b[i];
}

void hs_kernel_vec_mul_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(double));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    for (; i + 4 <= count; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        _mm256_storeu_pd(dst + i, _mm256_mul_pd(va, vb));
    }
#endif
    for (; i < count; ++i) dst[i] = a[i] * b[i];
}

void hs_kernel_vec_fma_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    const double* c = (nin >= 3 && in[2].data) ? static_cast<const double*>(in[2].data) : nullptr;
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(double));
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__) && defined(__FMA__)
    for (; i + 4 <= count; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        __m256d vc = c ? _mm256_loadu_pd(c + i) : _mm256_loadu_pd(dst + i);
        _mm256_storeu_pd(dst + i, _mm256_fmadd_pd(va, vb, vc));
    }
#endif
    for (; i < count; ++i) {
        double addend = c ? c[i] : dst[i];
        dst[i] = std::fma(a[i], b[i], addend);
    }
}

void hs_kernel_vec_dot_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&in[0], sizeof(double));
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += a[i] * b[i];
    dst[0] = sum;
}

void hs_kernel_vec_min_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(double));
    for (size_t i = 0; i < count; ++i) dst[i] = std::min(a[i], b[i]);
}

void hs_kernel_vec_max_f64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(double));
    for (size_t i = 0; i < count; ++i) dst[i] = std::max(a[i], b[i]);
}

// ── INT32 Kernels ──

void hs_kernel_vec_add_i32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int32_t* a = static_cast<const int32_t*>(in[0].data);
    const int32_t* b = static_cast<const int32_t*>(in[1].data);
    int32_t* dst     = static_cast<int32_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] + b[i];
}

void hs_kernel_vec_sub_i32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int32_t* a = static_cast<const int32_t*>(in[0].data);
    const int32_t* b = static_cast<const int32_t*>(in[1].data);
    int32_t* dst     = static_cast<int32_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] - b[i];
}

void hs_kernel_vec_mul_i32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int32_t* a = static_cast<const int32_t*>(in[0].data);
    const int32_t* b = static_cast<const int32_t*>(in[1].data);
    int32_t* dst     = static_cast<int32_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] * b[i];
}

void hs_kernel_vec_and_i32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int32_t* a = static_cast<const int32_t*>(in[0].data);
    const int32_t* b = static_cast<const int32_t*>(in[1].data);
    int32_t* dst     = static_cast<int32_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] & b[i];
}

void hs_kernel_vec_or_i32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int32_t* a = static_cast<const int32_t*>(in[0].data);
    const int32_t* b = static_cast<const int32_t*>(in[1].data);
    int32_t* dst     = static_cast<int32_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] | b[i];
}

void hs_kernel_vec_xor_i32(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int32_t* a = static_cast<const int32_t*>(in[0].data);
    const int32_t* b = static_cast<const int32_t*>(in[1].data);
    int32_t* dst     = static_cast<int32_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int32_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] ^ b[i];
}

// ── INT64 Kernels ──

void hs_kernel_vec_add_i64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int64_t* a = static_cast<const int64_t*>(in[0].data);
    const int64_t* b = static_cast<const int64_t*>(in[1].data);
    int64_t* dst     = static_cast<int64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int64_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] + b[i];
}

void hs_kernel_vec_sub_i64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int64_t* a = static_cast<const int64_t*>(in[0].data);
    const int64_t* b = static_cast<const int64_t*>(in[1].data);
    int64_t* dst     = static_cast<int64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int64_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] - b[i];
}

void hs_kernel_vec_mul_i64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int64_t* a = static_cast<const int64_t*>(in[0].data);
    const int64_t* b = static_cast<const int64_t*>(in[1].data);
    int64_t* dst     = static_cast<int64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int64_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] * b[i];
}

void hs_kernel_vec_and_i64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int64_t* a = static_cast<const int64_t*>(in[0].data);
    const int64_t* b = static_cast<const int64_t*>(in[1].data);
    int64_t* dst     = static_cast<int64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int64_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] & b[i];
}

void hs_kernel_vec_or_i64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int64_t* a = static_cast<const int64_t*>(in[0].data);
    const int64_t* b = static_cast<const int64_t*>(in[1].data);
    int64_t* dst     = static_cast<int64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int64_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] | b[i];
}

void hs_kernel_vec_xor_i64(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const int64_t* a = static_cast<const int64_t*>(in[0].data);
    const int64_t* b = static_cast<const int64_t*>(in[1].data);
    int64_t* dst     = static_cast<int64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t count = hs_tensor_element_count(&out[0], sizeof(int64_t));
    for (size_t i = 0; i < count; ++i) dst[i] = a[i] ^ b[i];
}

// ── Multi-Limb Emulated Kernels (Section 14 & 15) ──

struct hs_double_double_t {
    double hi{ 0.0 };
    double lo{ 0.0 };
};

static inline hs_double_double_t hs_two_sum(double a, double b) noexcept {
    double s = a + b;
    double v = s - a;
    double e = (a - (s - v)) + (b - v);
    return { s, e };
}

static inline hs_double_double_t hs_quick_two_sum(double a, double b) noexcept {
    double s = a + b;
    double e = b - (s - a);
    return { s, e };
}

static inline hs_double_double_t hs_two_prod(double a, double b) noexcept {
    double p = a * b;
    double e = std::fma(a, b, -p);
    return { p, e };
}

static inline hs_double_double_t hs_dd_add(hs_double_double_t a, hs_double_double_t b) noexcept {
    hs_double_double_t s = hs_two_sum(a.hi, b.hi);
    hs_double_double_t t = hs_two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = hs_quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    s = hs_quick_two_sum(s.hi, s.lo);
    return s;
}

static inline hs_double_double_t hs_dd_mul(hs_double_double_t a, hs_double_double_t b) noexcept {
    hs_double_double_t p = hs_two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return hs_quick_two_sum(p.hi, p.lo);
}

static inline uint64_t hs_mul64(uint64_t u, uint64_t v, uint64_t* hi) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
    return _umul128(u, v, hi);
#else
    unsigned __int128 prod = static_cast<unsigned __int128>(u) * v;
    *hi = static_cast<uint64_t>(prod >> 64);
    return static_cast<uint64_t>(prod);
#endif
}

void hs_kernel_emu_double_double_add(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_pairs = hs_tensor_element_count(&out[0], sizeof(double)) / 2;
    for (size_t i = 0; i < num_pairs; ++i) {
        hs_double_double_t da = { a[2 * i + 0], a[2 * i + 1] };
        hs_double_double_t db = { b[2 * i + 0], b[2 * i + 1] };
        hs_double_double_t res = hs_dd_add(da, db);
        dst[2 * i + 0] = res.hi;
        dst[2 * i + 1] = res.lo;
    }
}

void hs_kernel_emu_double_double_mul(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_pairs = hs_tensor_element_count(&out[0], sizeof(double)) / 2;
    for (size_t i = 0; i < num_pairs; ++i) {
        hs_double_double_t da = { a[2 * i + 0], a[2 * i + 1] };
        hs_double_double_t db = { b[2 * i + 0], b[2 * i + 1] };
        hs_double_double_t res = hs_dd_mul(da, db);
        dst[2 * i + 0] = res.hi;
        dst[2 * i + 1] = res.lo;
    }
}

void hs_kernel_emu_quad_double_add(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_quads = hs_tensor_element_count(&out[0], sizeof(double)) / 4;
    for (size_t q = 0; q < num_quads; ++q) {
        hs_double_double_t a_hi = { a[4 * q + 0], a[4 * q + 1] };
        hs_double_double_t a_lo = { a[4 * q + 2], a[4 * q + 3] };
        hs_double_double_t b_hi = { b[4 * q + 0], b[4 * q + 1] };
        hs_double_double_t b_lo = { b[4 * q + 2], b[4 * q + 3] };

        hs_double_double_t s_hi = hs_dd_add(a_hi, b_hi);
        hs_double_double_t s_lo = hs_dd_add(a_lo, b_lo);

        hs_double_double_t mid = hs_two_sum(s_hi.lo, s_lo.hi);
        hs_double_double_t r01 = hs_quick_two_sum(s_hi.hi, mid.hi);
        hs_double_double_t r23 = hs_two_sum(mid.lo, s_lo.lo);
        hs_double_double_t final_mid = hs_quick_two_sum(r01.lo, r23.hi);

        dst[4 * q + 0] = r01.hi;
        dst[4 * q + 1] = final_mid.hi;
        dst[4 * q + 2] = final_mid.lo;
        dst[4 * q + 3] = r23.lo;
    }
}

void hs_kernel_emu_quad_double_mul(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const double* a = static_cast<const double*>(in[0].data);
    const double* b = static_cast<const double*>(in[1].data);
    double* dst     = static_cast<double*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_quads = hs_tensor_element_count(&out[0], sizeof(double)) / 4;
    for (size_t q = 0; q < num_quads; ++q) {
        hs_double_double_t a_hi = { a[4 * q + 0], a[4 * q + 1] };
        hs_double_double_t a_lo = { a[4 * q + 2], a[4 * q + 3] };
        hs_double_double_t b_hi = { b[4 * q + 0], b[4 * q + 1] };
        hs_double_double_t b_lo = { b[4 * q + 2], b[4 * q + 3] };

        hs_double_double_t p0 = hs_dd_mul(a_hi, b_hi);
        hs_double_double_t p1 = hs_dd_mul(a_hi, b_lo);
        hs_double_double_t p2 = hs_dd_mul(a_lo, b_hi);

        hs_double_double_t cross = hs_dd_add(p1, p2);
        hs_double_double_t mid = hs_two_sum(p0.lo, cross.hi);
        hs_double_double_t r01 = hs_quick_two_sum(p0.hi, mid.hi);
        hs_double_double_t r23 = hs_two_sum(mid.lo, cross.lo);
        hs_double_double_t final_mid = hs_quick_two_sum(r01.lo, r23.hi);

        dst[4 * q + 0] = r01.hi;
        dst[4 * q + 1] = final_mid.hi;
        dst[4 * q + 2] = final_mid.lo;
        dst[4 * q + 3] = r23.lo;
    }
}

void hs_kernel_emu_int128_add(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const uint64_t* a = static_cast<const uint64_t*>(in[0].data);
    const uint64_t* b = static_cast<const uint64_t*>(in[1].data);
    uint64_t* dst     = static_cast<uint64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_pairs = hs_tensor_element_count(&out[0], sizeof(uint64_t)) / 2;
    for (size_t i = 0; i < num_pairs; ++i) {
        uint64_t a_lo = a[2 * i + 0];
        uint64_t a_hi = a[2 * i + 1];
        uint64_t b_lo = b[2 * i + 0];
        uint64_t b_hi = b[2 * i + 1];

        uint64_t r_lo = a_lo + b_lo;
        uint64_t carry = (r_lo < a_lo) ? 1ULL : 0ULL;
        uint64_t r_hi = a_hi + b_hi + carry;

        dst[2 * i + 0] = r_lo;
        dst[2 * i + 1] = r_hi;
    }
}

void hs_kernel_emu_int128_mul(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const uint64_t* a = static_cast<const uint64_t*>(in[0].data);
    const uint64_t* b = static_cast<const uint64_t*>(in[1].data);
    uint64_t* dst     = static_cast<uint64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_pairs = hs_tensor_element_count(&out[0], sizeof(uint64_t)) / 2;
    for (size_t i = 0; i < num_pairs; ++i) {
        uint64_t a_lo = a[2 * i + 0];
        uint64_t a_hi = a[2 * i + 1];
        uint64_t b_lo = b[2 * i + 0];
        uint64_t b_hi = b[2 * i + 1];

        uint64_t r_hi = 0;
        uint64_t r_lo = hs_mul64(a_lo, b_lo, &r_hi);
        r_hi += (a_lo * b_hi + a_hi * b_lo);

        dst[2 * i + 0] = r_lo;
        dst[2 * i + 1] = r_hi;
    }
}

void hs_kernel_emu_int256_add(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const uint64_t* a = static_cast<const uint64_t*>(in[0].data);
    const uint64_t* b = static_cast<const uint64_t*>(in[1].data);
    uint64_t* dst     = static_cast<uint64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_quads = hs_tensor_element_count(&out[0], sizeof(uint64_t)) / 4;
    for (size_t q = 0; q < num_quads; ++q) {
        uint64_t carry = 0;
        for (int k = 0; k < 4; ++k) {
            uint64_t ak = a[4 * q + k];
            uint64_t bk = b[4 * q + k];
            uint64_t sum = ak + bk + carry;
            carry = (sum < ak || (carry && sum == ak)) ? 1ULL : 0ULL;
            dst[4 * q + k] = sum;
        }
    }
}

void hs_kernel_emu_int256_mul(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 2 || nout < 1) return;
    const uint64_t* a = static_cast<const uint64_t*>(in[0].data);
    const uint64_t* b = static_cast<const uint64_t*>(in[1].data);
    uint64_t* dst     = static_cast<uint64_t*>(out[0].data);
    if (!a || !b || !dst) return;
    size_t num_quads = hs_tensor_element_count(&out[0], sizeof(uint64_t)) / 4;
    for (size_t q = 0; q < num_quads; ++q) {
        const uint64_t* aq = a + 4 * q;
        const uint64_t* bq = b + 4 * q;
        uint64_t res[4] = { 0, 0, 0, 0 };

        for (int i = 0; i < 4; ++i) {
            uint64_t carry = 0;
            for (int j = 0; i + j < 4; ++j) {
                uint64_t hi = 0;
                uint64_t lo = hs_mul64(aq[i], bq[j], &hi);

                uint64_t s1 = res[i + j] + lo;
                uint64_t c1 = (s1 < res[i + j]) ? 1ULL : 0ULL;

                uint64_t s2 = s1 + carry;
                uint64_t c2 = (s2 < s1) ? 1ULL : 0ULL;

                res[i + j] = s2;
                carry = hi + c1 + c2;
            }
        }

        dst[4 * q + 0] = res[0];
        dst[4 * q + 1] = res[1];
        dst[4 * q + 2] = res[2];
        dst[4 * q + 3] = res[3];
    }
}

// ── GC Internal Fast-Path Kernels ──

void hs_kernel_gc_card_table_scan(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    if (!in || !out || nin < 1 || nout < 1) return;
    const uint8_t* cards = static_cast<const uint8_t*>(in[0].data);
    uint32_t* dirty_indices = static_cast<uint32_t*>(out[0].data);
    if (!cards || !dirty_indices) return;
    size_t card_count = in[0].byte_size;
    size_t max_dirty = out[0].byte_size / sizeof(uint32_t);

    size_t dirty_count = RedjetSIMD::scan_card_table(cards, card_count, dirty_indices, max_dirty);
    out[0].shape[0] = static_cast<int32_t>(dirty_count);
}

void hs_kernel_gc_memzero(const HS_hotspot_tensor_t* in, int nin, HS_hotspot_tensor_t* out, int nout) {
    (void)in; (void)nin;
    if (!out || nout < 1 || !out[0].data) return;
    void* dst = out[0].data;
    size_t sz = out[0].byte_size;
    std::memset(dst, 0, sz);
}

/* ========================================================================= */
/* 5. FAST-PATH ARITHMETIC ROUTING                                           */
/* ========================================================================= */

RJ_API int hs_dispatch_fast_path_arithmetic(uint32_t op_id, uint8_t dtype,
                                               const void* a, const void* b, void* out,
                                               size_t count, uint8_t priority) {
    if (!a || !out || count == 0) return -1;

    // Ensure ISA dispatch table is initialized
    g_hs_dispatch_table.init_default_kernels();

    // 1. Acquire single ALU lane from Nexus via route resolution (no double-dispatch)
    int is_emu = 0;
    int schema_id = 0;
    uint32_t alu_lane = 0;

    if (op_id == HS_VEC_OP_CARD_TABLE_SCAN) {
        alu_lane = RedjetALU::select_card_table_lane(priority < 8);
    } else if (op_id == HS_VEC_OP_MEMZERO_FAST) {
        alu_lane = RedjetALU::select_memzero_lane(false, priority < 8);
    } else {
        int r = RedjetALU::resolve_route(dtype, priority, &is_emu, &schema_id);
        alu_lane = (r >= 0) ? static_cast<uint32_t>(r) : 1u;
    }

    // 2. Package tensors with exact element byte sizes
    size_t elem_size = 4;
    switch (dtype) {
    case HS_DTYPE_F256:
    case HS_DTYPE_I256:
    case HS_DTYPE_U256:
        elem_size = 32;
        break;
    case HS_DTYPE_F128:
    case HS_DTYPE_I128:
    case HS_DTYPE_U128:
        elem_size = 16;
        break;
    case HS_DTYPE_F64:
    case HS_DTYPE_I64:
    case HS_DTYPE_U64:
        elem_size = 8;
        break;
    case HS_DTYPE_F32:
    case HS_DTYPE_I32:
    case HS_DTYPE_U32:
        elem_size = 4;
        break;
    case HS_DTYPE_F16:
    case HS_DTYPE_I16:
    case HS_DTYPE_U16:
        elem_size = 2;
        break;
    case HS_DTYPE_F8:
    case HS_DTYPE_I8:
    case HS_DTYPE_U8:
        elem_size = 1;
        break;
    default:
        elem_size = 4;
        break;
    }

    HS_hotspot_tensor_t in_tensors[2]{};
    in_tensors[0].data = const_cast<void*>(a);
    in_tensors[0].dtype = dtype;
    in_tensors[0].ndim = 1;
    in_tensors[0].shape[0] = static_cast<int32_t>(count);
    in_tensors[0].byte_size = count * elem_size;

    int num_in = 1;
    if (b) {
        in_tensors[1].data = const_cast<void*>(b);
        in_tensors[1].dtype = dtype;
        in_tensors[1].ndim = 1;
        in_tensors[1].shape[0] = static_cast<int32_t>(count);
        in_tensors[1].byte_size = count * elem_size;
        num_in = 2;
    }

    HS_hotspot_tensor_t out_tensor{};
    out_tensor.data = out;
    out_tensor.dtype = dtype;
    out_tensor.ndim = 1;
    out_tensor.shape[0] = static_cast<int32_t>(count);
    out_tensor.byte_size = count * elem_size;

    // 3. Resolve kernel function from ISA dispatch table
    uint32_t fn_id = (op_id << 8) | dtype;
    sacculus_kernel_fn_t fn = g_hs_dispatch_table.read_fn_seqlock(fn_id);
    if (fn) {
        fn(in_tensors, num_in, &out_tensor, 1);
    } else {
        // Fallback default dispatch based on opcode and dtype
        switch (op_id) {
        case HS_VEC_OP_ADD:
        case HS_VEC_OP_MULTI_LIMB_ADD:
            if (dtype == HS_DTYPE_F128) hs_kernel_emu_double_double_add(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F256) hs_kernel_emu_quad_double_add(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I128 || dtype == HS_DTYPE_U128) hs_kernel_emu_int128_add(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I256 || dtype == HS_DTYPE_U256) hs_kernel_emu_int256_add(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F64) hs_kernel_vec_add_f64(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F32) hs_kernel_vec_add_f32(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) hs_kernel_vec_add_i64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_add_i32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_SUB:
            if (dtype == HS_DTYPE_F64) hs_kernel_vec_sub_f64(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F32) hs_kernel_vec_sub_f32(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) hs_kernel_vec_sub_i64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_sub_i32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_MUL:
        case HS_VEC_OP_MULTI_LIMB_MUL:
            if (dtype == HS_DTYPE_F128) hs_kernel_emu_double_double_mul(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F256) hs_kernel_emu_quad_double_mul(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I128 || dtype == HS_DTYPE_U128) hs_kernel_emu_int128_mul(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I256 || dtype == HS_DTYPE_U256) hs_kernel_emu_int256_mul(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F64) hs_kernel_vec_mul_f64(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_F32) hs_kernel_vec_mul_f32(in_tensors, num_in, &out_tensor, 1);
            else if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) hs_kernel_vec_mul_i64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_mul_i32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_FMA:
            if (dtype == HS_DTYPE_F64) hs_kernel_vec_fma_f64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_fma_f32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_DOT:
            if (dtype == HS_DTYPE_F64) hs_kernel_vec_dot_f64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_dot_f32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_MIN:
            if (dtype == HS_DTYPE_F64) hs_kernel_vec_min_f64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_min_f32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_MAX:
            if (dtype == HS_DTYPE_F64) hs_kernel_vec_max_f64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_max_f32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_AND:
            if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) hs_kernel_vec_and_i64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_and_i32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_OR:
            if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) hs_kernel_vec_or_i64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_or_i32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_XOR:
            if (dtype == HS_DTYPE_I64 || dtype == HS_DTYPE_U64) hs_kernel_vec_xor_i64(in_tensors, num_in, &out_tensor, 1);
            else hs_kernel_vec_xor_i32(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_CARD_TABLE_SCAN:
            hs_kernel_gc_card_table_scan(in_tensors, num_in, &out_tensor, 1);
            break;
        case HS_VEC_OP_MEMZERO_FAST:
            hs_kernel_gc_memzero(in_tensors, num_in, &out_tensor, 1);
            break;
        default:
            break;
        }
    }

    // 4. Complete ALU lane in Nexus (exactly once)
    RedjetALU::complete(alu_lane);
    return 0;
}

/* ========================================================================= */
/* 6. NESTED BATCH/GROUP SCOPES, BMI/MORTON, RDTSC & MICRO-ARCH HINTERS      */
/* ========================================================================= */

struct hs_batch_scope {
    uint32_t batch_id{ 0 };
    uint8_t  priority{ 5 };
    int      bypass_warmup{ 0 };
    bool     completed{ false };
};

struct hs_group_scope {
    uint32_t group_id{ 0 };
    uint8_t  priority{ 5 };
    int      bypass_warmup{ 0 };
    bool     completed{ false };
};

extern "C" {

RJ_API uint32_t HS_hotspot_group_compute(uint8_t priority, int bypass_warmup) {
    uint32_t gid = g_hs_group_counter.fetch_add(1, std::memory_order_relaxed);
    if (priority > 10) priority = 10;
    tl_hs_group_hex_id        = gid;
    tl_hs_group_priority      = priority;
    tl_hs_group_bypass_warmup = (bypass_warmup != 0);

    if (priority >= 8 || bypass_warmup) {
        (void)hs_bind_current_thread(HS_CORE_TIER_PERFORMANCE);
        hs_upclocker_set_boost(1);
        hs_upclocker_t::pin_max_frequency_warmup(10);
    }

    if (HS_hotspot_get_allow_operation_dispatchings() && priority > 0) {
        char p_msg[256];
        if (priority >= 8) {
            std::snprintf(p_msg, sizeof(p_msg),
                "[HSVM Flag Conflict Warning] High priority (%u) pins execution to performance vector units and overrides queue dispatching.",
                static_cast<unsigned int>(priority));
            hs_unified_log_emit(HS_Ebin0_GROUP_COMPUTE_BEGIN, HS_Ebin1_WARN, gid, "prio_override_disp", p_msg);
        } else {
            std::snprintf(p_msg, sizeof(p_msg),
                "[HSVM Notice] 'priority' (%u) assigned under active 'Allow_Operation_Dispatchings'.",
                static_cast<unsigned int>(priority));
            hs_unified_log_emit(HS_Ebin0_GROUP_COMPUTE_BEGIN, HS_Ebin1_INFO, gid, "prio_under_disp", p_msg);
        }
    }
    return gid;
}

RJ_API void HS_hotspot_group_compute_reset(void) {
    uint32_t prev_gid = tl_hs_group_hex_id;
    tl_hs_group_hex_id        = 0;
    tl_hs_group_priority      = 5;
    tl_hs_group_bypass_warmup = false;
    hs_hotlog_emit(HS_Ebin0_GROUP_COMPUTE_RESET, HS_Ebin1_INFO, prev_gid, 0, 0, 0, 0, 0, 0, "group_compute_reset");
    auto* nexus = hs_get_hardware_nexus();
    if (nexus && nexus->focus.is_homogeneous) {
        (void)hs_bind_current_thread(HS_CORE_TIER_ANY);
    }
}

RJ_API uint32_t HS_hotspot_batch_begin(uint8_t priority, int bypass_warmup) {
    if (tl_hs_batch_depth < 16) {
        tl_hs_batch_stack[tl_hs_batch_depth].group_id      = tl_hs_group_hex_id;
        tl_hs_batch_stack[tl_hs_batch_depth].priority      = tl_hs_group_priority;
        tl_hs_batch_stack[tl_hs_batch_depth].bypass_warmup = tl_hs_group_bypass_warmup;
        tl_hs_batch_depth++;
    }
    uint32_t gid = HS_hotspot_group_compute(priority, bypass_warmup);
    g_hs_active_batch_id.store(gid, std::memory_order_release);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "[HSVM Entangle/BatchScope] Opened batch processing block [0x%08X] (priority=%u, bypass=%d, depth=%u)",
        static_cast<unsigned int>(gid), static_cast<unsigned int>(priority), bypass_warmup,
        static_cast<unsigned int>(tl_hs_batch_depth));
    hs_unified_log_emit(HS_Ebin0_GROUP_COMPUTE_BEGIN, HS_Ebin1_INFO, gid, "batch_scope_begin", buf);
    return gid;
}

RJ_API void HS_hotspot_batch_end(void) {
    uint32_t gid = tl_hs_group_hex_id;
    if (tl_hs_batch_depth > 0) {
        tl_hs_batch_depth--;
        const auto& prev          = tl_hs_batch_stack[tl_hs_batch_depth];
        tl_hs_group_hex_id        = prev.group_id;
        tl_hs_group_priority      = prev.priority;
        tl_hs_group_bypass_warmup = prev.bypass_warmup;
        g_hs_active_batch_id.store(prev.group_id, std::memory_order_release);
        if (tl_hs_batch_depth == 0) {
            HS_hotspot_group_compute_reset();
        }
    } else {
        HS_hotspot_group_compute_reset();
        g_hs_active_batch_id.store(0, std::memory_order_release);
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "[HSVM Entangle/BatchScope] Closed batch processing block [0x%08X] (depth=%u)",
        static_cast<unsigned int>(gid), static_cast<unsigned int>(tl_hs_batch_depth));
    hs_unified_log_emit(HS_Ebin0_GROUP_COMPUTE_RESET, HS_Ebin1_INFO, gid, "batch_scope_end", buf);
}

RJ_API void HS_hotspot_batch_commit(uint32_t batch_id) {
    (void)batch_id;
    HS_hotspot_entangle_commit();
    HS_hotspot_batch_end();
}

RJ_API void HS_hotspot_batch_abort(uint32_t batch_id) {
    (void)batch_id;
    g_hs_entangle_engine.reset();
    HS_hotspot_batch_end();
}

RJ_API uint32_t HS_hotspot_get_current_batch_id(void) {
    return g_hs_active_batch_id.load(std::memory_order_acquire);
}

RJ_API hs_batch_handle HS_hotspot_batch_scope_begin(uint8_t priority, int bypass_warmup) {
    auto* h          = new hs_batch_scope();
    h->batch_id      = HS_hotspot_batch_begin(priority, bypass_warmup);
    h->priority      = priority;
    h->bypass_warmup = bypass_warmup;
    h->completed     = false;
    HS_hotspot_entangle_begin();
    return h;
}

RJ_API void HS_hotspot_batch_scope_commit(hs_batch_handle handle) {
    if (handle && !handle->completed) {
        HS_hotspot_batch_commit(handle->batch_id);
        handle->completed = true;
    }
}

RJ_API void HS_hotspot_batch_scope_abort(hs_batch_handle handle) {
    if (handle && !handle->completed) {
        HS_hotspot_batch_abort(handle->batch_id);
        handle->completed = true;
    }
}

RJ_API void HS_hotspot_batch_scope_destroy(hs_batch_handle handle) {
    if (handle) {
        if (!handle->completed) {
            HS_hotspot_batch_commit(handle->batch_id);
        }
        delete handle;
    }
}

RJ_API uint32_t HS_hotspot_batch_scope_get_id(hs_batch_handle handle) {
    return handle ? handle->batch_id : 0;
}

RJ_API hs_group_handle HS_hotspot_group_scope_begin(uint8_t priority, int bypass_warmup) {
    auto* h          = new hs_group_scope();
    h->group_id      = HS_hotspot_group_compute(priority, bypass_warmup);
    h->priority      = priority;
    h->bypass_warmup = bypass_warmup;
    h->completed     = false;
    return h;
}

RJ_API uint32_t HS_hotspot_group_scope_get_id(hs_group_handle handle) {
    return handle ? handle->group_id : 0;
}

RJ_API void HS_hotspot_group_scope_end(hs_group_handle handle) {
    if (handle && !handle->completed) {
        HS_hotspot_group_compute_reset();
        handle->completed = true;
    }
}

RJ_API void HS_hotspot_group_scope_destroy(hs_group_handle handle) {
    if (handle) {
        if (!handle->completed) {
            HS_hotspot_group_compute_reset();
        }
        delete handle;
    }
}

/* ── BMI1 / BMI2 / Morton-3D Intrinsics ── */

RJ_API uint32_t hs_bmi_tzcnt32(uint32_t x) {
    if (x == 0) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanForward(&index, x);
    return static_cast<uint32_t>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_ctz(x));
#else
    uint32_t n = 0;
    while ((x & 1u) == 0u) { ++n; x >>= 1; }
    return n;
#endif
}

RJ_API uint32_t hs_bmi_tzcnt64(uint64_t x) {
    if (x == 0) return 64;
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    unsigned long index;
    _BitScanForward64(&index, x);
    return static_cast<uint32_t>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_ctzll(x));
#else
    uint32_t n = 0;
    while ((x & 1ULL) == 0ULL) { ++n; x >>= 1; }
    return n;
#endif
}

RJ_API uint32_t hs_bmi_lzcnt32(uint32_t x) {
    if (x == 0) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse(&index, x);
    return static_cast<uint32_t>(31 - index);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_clz(x));
#else
    uint32_t n = 0;
    while ((x & 0x80000000u) == 0u) { ++n; x <<= 1; }
    return n;
#endif
}

RJ_API uint32_t hs_bmi_lzcnt64(uint64_t x) {
    if (x == 0) return 64;
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse64(&index, x);
    return static_cast<uint32_t>(63 - index);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_clzll(x));
#else
    uint32_t n = 0;
    while ((x & 0x8000000000000000ULL) == 0ULL) { ++n; x <<= 1; }
    return n;
#endif
}

RJ_API uint32_t hs_bmi_popcnt32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcount(x));
#elif (defined(_M_X64) || defined(_M_IX86)) && defined(_MSC_VER)
    return static_cast<uint32_t>(__popcnt(x));
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
#endif
}

RJ_API uint32_t hs_bmi_popcnt64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcountll(x));
#elif defined(_M_X64) && defined(_MSC_VER)
    return static_cast<uint32_t>(__popcnt64(x));
#else
    return hs_bmi_popcnt32(static_cast<uint32_t>(x)) + hs_bmi_popcnt32(static_cast<uint32_t>(x >> 32));
#endif
}

RJ_API uint64_t hs_bmi_blsr64(uint64_t x) {
    return x & (x - 1ULL);
}

RJ_API uint64_t hs_bmi2_pext64(uint64_t src, uint64_t mask) {
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__BMI2__)
    return _pext_u64(src, mask);
#else
    uint64_t result = 0;
    uint64_t bit    = 1;
    for (int i = 0; i < 64; ++i) {
        if ((mask >> i) & 1ULL) {
            if ((src >> i) & 1ULL) result |= bit;
            bit <<= 1;
        }
    }
    return result;
#endif
}

RJ_API uint64_t hs_bmi2_pdep64(uint64_t src, uint64_t mask) {
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__BMI2__)
    return _pdep_u64(src, mask);
#else
    uint64_t result = 0;
    uint64_t bit    = 1;
    for (int i = 0; i < 64; ++i) {
        if ((mask >> i) & 1ULL) {
            if (src & bit) result |= (1ULL << i);
            bit <<= 1;
        }
    }
    return result;
#endif
}

RJ_API uint64_t hs_morton_encode3d(uint32_t x, uint32_t y, uint32_t z) {
    auto split_by_3 = [](uint32_t a) -> uint64_t {
        uint64_t v = a & 0x1fffffULL;
        v = (v | (v << 32)) & 0x1f00000000ffffULL;
        v = (v | (v << 16)) & 0x1f0000ff0000ffULL;
        v = (v | (v << 8))  & 0x100f00f00f00f00fULL;
        v = (v | (v << 4))  & 0x10c30c30c30c30c3ULL;
        v = (v | (v << 2))  & 0x1249249249249249ULL;
        return v;
    };
    return split_by_3(x) | (split_by_3(y) << 1) | (split_by_3(z) << 2);
}

RJ_API uint32_t HS_hotspot_tzcnt32(uint32_t x) { return hs_bmi_tzcnt32(x); }
RJ_API uint32_t HS_hotspot_tzcnt64(uint64_t x) { return hs_bmi_tzcnt64(x); }
RJ_API uint32_t HS_hotspot_lzcnt32(uint32_t x) { return hs_bmi_lzcnt32(x); }
RJ_API uint32_t HS_hotspot_lzcnt64(uint64_t x) { return hs_bmi_lzcnt64(x); }
RJ_API uint32_t HS_hotspot_popcnt32(uint32_t x) { return hs_bmi_popcnt32(x); }
RJ_API uint32_t HS_hotspot_popcnt64(uint64_t x) { return hs_bmi_popcnt64(x); }
RJ_API uint64_t HS_hotspot_blsr64(uint64_t x) { return hs_bmi_blsr64(x); }
RJ_API uint64_t HS_hotspot_pext64(uint64_t src, uint64_t mask) { return hs_bmi2_pext64(src, mask); }
RJ_API uint64_t HS_hotspot_pdep64(uint64_t src, uint64_t mask) { return hs_bmi2_pdep64(src, mask); }
RJ_API uint64_t HS_hotspot_morton_encode3d(uint32_t x, uint32_t y, uint32_t z) { return hs_morton_encode3d(x, y, z); }

/* ── Cycle-Accurate RDTSC / CNTVCT_EL0 Telemetry ── */

RJ_API uint64_t hs_rdtsc_begin(void) {
#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned int aux;
    _mm_lfence();
    return __rdtscp(&aux);
#else
    uint32_t lo, hi;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :: "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(val) :: "memory");
    return val;
#else
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
}

RJ_API uint64_t hs_rdtsc_end(void) {
#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned int aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    uint32_t lo, hi;
    __asm__ __volatile__(
        "rdtscp\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi)
        :: "ecx", "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0\n\tisb" : "=r"(val) :: "memory");
    return val;
#else
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
}

RJ_API uint64_t hs_rdtsc_freq_hz(void) {
#if defined(_M_X64) || defined(__x86_64__)
    int regs[4] = { 0 };
#if defined(_MSC_VER)
    __cpuid(regs, 0);
#else
    __cpuid(0, regs[0], regs[1], regs[2], regs[3]);
#endif
    if (regs[0] >= 0x15) {
#if defined(_MSC_VER)
        __cpuid(regs, 0x15);
#else
        __cpuid(0x15, regs[0], regs[1], regs[2], regs[3]);
#endif
        uint32_t denom   = static_cast<uint32_t>(regs[0]);
        uint32_t numer   = static_cast<uint32_t>(regs[1]);
        uint32_t crystal = static_cast<uint32_t>(regs[2]);
        if (denom != 0 && numer != 0 && crystal != 0) {
            return (static_cast<uint64_t>(crystal) * numer) / denom;
        }
    }
#endif
    uint64_t start = hs_rdtsc_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t end = hs_rdtsc_end();
    return (end > start) ? ((end - start) * 200ULL) : 1000000000ULL;
}

RJ_API double hs_cycles_to_ns(uint64_t cycles, uint64_t freq_hz) {
    if (freq_hz == 0) return 0.0;
    return static_cast<double>(cycles) / static_cast<double>(freq_hz) * 1e9;
}

RJ_API uint64_t HS_hotspot_rdtsc_begin(void) { return hs_rdtsc_begin(); }
RJ_API uint64_t HS_hotspot_rdtsc_end(void) { return hs_rdtsc_end(); }
RJ_API uint64_t HS_hotspot_rdtsc_freq_hz(void) { return hs_rdtsc_freq_hz(); }
RJ_API double   HS_hotspot_cycles_to_ns(uint64_t cycles, uint64_t freq_hz) { return hs_cycles_to_ns(cycles, freq_hz); }

/* ── Core Thread Pinning & Priority Steering ── */

RJ_API int hs_thread_pin_to_core(int logical_cpu) {
    if (logical_cpu < 0) return 0;
#ifdef _WIN32
    DWORD_PTR mask = (logical_cpu < 64) ? (static_cast<DWORD_PTR>(1ULL) << logical_cpu) : 0;
    if (mask == 0) return 0;
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
    return (prev != 0) ? 1 : 0;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(logical_cpu, &cpuset);
    return (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) ? 1 : 0;
#else
    (void)logical_cpu;
    return 0;
#endif
}

RJ_API int hs_thread_set_priority_high(void) {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) ? 1 : 0;
#elif defined(__linux__)
    struct sched_param sp{};
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        sp.sched_priority = sched_get_priority_max(SCHED_RR);
        (void)sched_setscheduler(0, SCHED_RR, &sp);
    }
    return 1;
#else
    return 0;
#endif
}

RJ_API int HS_hotspot_thread_pin_to_core(int logical_cpu) { return hs_thread_pin_to_core(logical_cpu); }
RJ_API int HS_hotspot_thread_set_priority_high(void) { return hs_thread_set_priority_high(); }

/* ── Micro-Architectural Branchless Hinters & SIMD Vector Blend ── */

RJ_API int32_t hs_hinter_cmov_i32(int condition, int32_t val_true, int32_t val_false, const void* precision_target) {
    (void)precision_target;
    if (!HS_hotspot_is_iasm_active(0)) {
        return condition ? val_true : val_false;
    }
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
    int32_t result = val_false;
    __asm__ __volatile__(
        "testl %1, %1\n\tcmovnel %2, %0"
        : "+r"(result) : "r"(condition), "r"(val_true) : "cc"
    );
    return result;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    int32_t result;
    __asm__ __volatile__(
        "cmp %w1, #0\n\tcsel %w0, %w2, %w3, ne"
        : "=r"(result) : "r"(condition), "r"(val_true), "r"(val_false) : "cc"
    );
    return result;
#else
    return condition ? val_true : val_false;
#endif
}

RJ_API int64_t hs_hinter_cmov_i64(int condition, int64_t val_true, int64_t val_false, const void* precision_target) {
    (void)precision_target;
    if (!HS_hotspot_is_iasm_active(0)) {
        return condition ? val_true : val_false;
    }
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
    int64_t result = val_false;
    __asm__ __volatile__(
        "testl %1, %1\n\tcmovneq %2, %0"
        : "+r"(result) : "r"(condition), "r"(val_true) : "cc"
    );
    return result;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    int64_t result;
    __asm__ __volatile__(
        "cmp %w1, #0\n\tcsel %0, %2, %3, ne"
        : "=r"(result) : "r"(condition), "r"(val_true), "r"(val_false) : "cc"
    );
    return result;
#else
    return condition ? val_true : val_false;
#endif
}

RJ_API float hs_hinter_cmov_f32(int condition, float val_true, float val_false, const HS_hotspot_tensor_t* precision_target) {
    (void)precision_target;
    uint32_t u_true = 0, u_false = 0, u_res = 0;
    float result = 0.0f;
    std::memcpy(&u_true, &val_true, sizeof(float));
    std::memcpy(&u_false, &val_false, sizeof(float));
    u_res = static_cast<uint32_t>(hs_hinter_cmov_i32(condition, static_cast<int32_t>(u_true), static_cast<int32_t>(u_false), nullptr));
    std::memcpy(&result, &u_res, sizeof(float));
    return result;
}

RJ_API double hs_hinter_cmov_f64(int condition, double val_true, double val_false, const HS_hotspot_tensor_t* precision_target) {
    (void)precision_target;
    uint64_t u_true = 0, u_false = 0, u_res = 0;
    double result = 0.0;
    std::memcpy(&u_true, &val_true, sizeof(double));
    std::memcpy(&u_false, &val_false, sizeof(double));
    u_res = static_cast<uint64_t>(hs_hinter_cmov_i64(condition, static_cast<int64_t>(u_true), static_cast<int64_t>(u_false), nullptr));
    std::memcpy(&result, &u_res, sizeof(double));
    return result;
}

RJ_API void hs_hinter_blend_f32(float* dst, const float* a, const float* b, const uint32_t* mask, size_t count, const HS_hotspot_tensor_t* precision_target) {
    (void)precision_target;
    if (!dst || !a || !b || !mask) return;
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    const __m256i vzero = _mm256_setzero_si256();
    for (; i + 8 <= count; i += 8) {
        __m256  va     = _mm256_loadu_ps(a + i);
        __m256  vb     = _mm256_loadu_ps(b + i);
        __m256i vm_raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mask + i));
        __m256i veq0   = _mm256_cmpeq_epi32(vm_raw, vzero);
        __m256  vsel   = _mm256_castsi256_ps(veq0);
        __m256  vres   = _mm256_blendv_ps(va, vb, vsel);
        _mm256_storeu_ps(dst + i, vres);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = mask[i] ? a[i] : b[i];
    }
}

RJ_API int hs_hinter_should_stream(size_t dataset_bytes) {
    auto* nexus = hs_get_hardware_nexus();
    if (nexus && nexus->initialized.load(std::memory_order_acquire)) {
        return nexus->sram.should_stream_store(dataset_bytes) ? 1 : 0;
    }
    return (dataset_bytes >= 64 * 1024) ? 1 : 0;
}

RJ_API int hs_hinter_should_pin_pcore(double flops_per_byte) {
    return (flops_per_byte >= 4.0) ? 1 : 0;
}

RJ_API size_t hs_hinter_optimal_prefetch_distance(size_t cacheline_bytes) {
    if (cacheline_bytes == 0) cacheline_bytes = 64;
    return cacheline_bytes * 4;
}

RJ_API int32_t HS_hotspot_hinter_cmov_i32(int c, int32_t vt, int32_t vf, const HS_hotspot_tensor_t* p) { return hs_hinter_cmov_i32(c, vt, vf, p); }
RJ_API int64_t HS_hotspot_hinter_cmov_i64(int c, int64_t vt, int64_t vf, const HS_hotspot_tensor_t* p) { return hs_hinter_cmov_i64(c, vt, vf, p); }
RJ_API float   HS_hotspot_hinter_cmov_f32(int c, float vt, float vf, const HS_hotspot_tensor_t* p) { return hs_hinter_cmov_f32(c, vt, vf, p); }
RJ_API double  HS_hotspot_hinter_cmov_f64(int c, double vt, double vf, const HS_hotspot_tensor_t* p) { return hs_hinter_cmov_f64(c, vt, vf, p); }
RJ_API void    HS_hotspot_hinter_blend_f32(float* dst, const float* a, const float* b, const uint32_t* m, size_t count, const HS_hotspot_tensor_t* p) { hs_hinter_blend_f32(dst, a, b, m, count, p); }
RJ_API int     HS_hotspot_hinter_should_stream(size_t dataset_bytes) { return hs_hinter_should_stream(dataset_bytes); }
RJ_API int     HS_hotspot_hinter_should_pin_pcore(double flops_per_byte) { return hs_hinter_should_pin_pcore(flops_per_byte); }
RJ_API size_t  HS_hotspot_hinter_optimal_prefetch_distance(size_t cacheline_bytes) { return hs_hinter_optimal_prefetch_distance(cacheline_bytes); }

} // extern "C"
