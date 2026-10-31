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

#include "gc/redlet/redletDispatchISA.hpp"
#include <cstring>
#include <cstdio>
#include <chrono>

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_MSC_VER)
#include <cpuid.h>
#endif

namespace {

static inline void sf_dispatch_flush_icache(void* addr, size_t len) noexcept {
    if (!addr || len == 0) return;
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), addr, len);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(static_cast<char*>(addr), static_cast<char*>(addr) + len);
#endif
}

static inline void sf_dispatch_full_fence() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

struct sf_batch_scope_frame_t {
    uint32_t group_id{ 0 };
    uint8_t  priority{ 5 };
    bool     bypass_warmup{ false };
};

static std::atomic<uint32_t> g_sf_group_counter{ 0x1000u };
static std::atomic<uint32_t> g_sf_active_batch_id{ 0 };

static thread_local uint32_t               tl_sf_group_hex_id        = 0;
static thread_local uint8_t                tl_sf_group_priority      = 5;
static thread_local bool                   tl_sf_group_bypass_warmup = false;
static thread_local sf_batch_scope_frame_t tl_sf_batch_stack[16]{};
static thread_local uint32_t               tl_sf_batch_depth         = 0;

} // namespace

uint32_t sf_get_thread_group_hex_id() noexcept        { return tl_sf_group_hex_id; }
uint8_t  sf_get_thread_group_priority() noexcept      { return tl_sf_group_priority; }
bool     sf_get_thread_group_bypass_warmup() noexcept { return tl_sf_group_bypass_warmup; }

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
        sf_dispatch_flush_icache(reinterpret_cast<void*>(fn), 64);
    }
    sf_dispatch_full_fence();

    entry.seqlock_version.fetch_add(1, std::memory_order_release);
}

void sacculus_dispatch_table_t::swap(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t best) noexcept {
    if (fn_id >= SACFOR_MAX_DISPATCH_SLOTS) return;
    auto& entry = entries[fn_id];
    entry.seqlock_version.fetch_add(1, std::memory_order_acq_rel);

    entry.fn.store(new_fn, std::memory_order_relaxed);
    entry.best_cycles = best;

    if (new_fn) {
        sf_dispatch_flush_icache(reinterpret_cast<void*>(new_fn), 64);
    }
    sf_dispatch_full_fence();

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

/* ========================================================================= */
/* 2. GLOBAL 65+ WORLD ISA CATALOG & FALLBACK HIERARCHY                      */
/* ========================================================================= */

static const sf_world_isa_entry_t g_world_isa_catalog[] = {
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

const sf_world_isa_entry_t* sf_find_world_isa(const char* name) noexcept {
    if (!name || !*name) return nullptr;
    for (const auto& item : g_world_isa_catalog) {
        if (_stricmp(item.isa_name, name) == 0) return &item;
    }
    return nullptr;
}

size_t sf_world_isa_catalog_count() noexcept {
    return sizeof(g_world_isa_catalog) / sizeof(g_world_isa_catalog[0]);
}

const sf_world_isa_entry_t* sf_world_isa_catalog_at(size_t idx) noexcept {
    if (idx >= sf_world_isa_catalog_count()) return nullptr;
    return &g_world_isa_catalog[idx];
}

size_t sf_resolve_isa_fallback_chain(const char* start_isa,
                                     const sf_world_isa_entry_t** out_chain,
                                     size_t max_depth) noexcept {
    if (!start_isa || !out_chain || max_depth == 0) return 0;
    size_t count = 0;
    const sf_world_isa_entry_t* cur = sf_find_world_isa(start_isa);
    while (cur && count < max_depth) {
        out_chain[count++] = cur;
        if (!cur->fallback_isa) break;
        cur = sf_find_world_isa(cur->fallback_isa);
    }
    return count;
}

/* ========================================================================= */
/* 3. STANDALONE PINNED FALLBACK WORKER THREAD POOL                          */
/* ========================================================================= */

sf_fallback_pool_t::sf_fallback_pool_t(int num_threads) : thread_count(num_threads) {
    if (thread_count <= 0) {
        auto* nexus = sf_get_hardware_nexus();
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
            (void)sf_thread_pin_to_core(i);

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

sf_fallback_pool_t::~sf_fallback_pool_t() {
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

void sf_fallback_pool_t::submit(std::function<void()> task) {
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

void sf_fallback_pool_t::submit_to_worker(int worker_idx, std::function<void()> task) {
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

void sf_fallback_pool_t::wait_all() {
    if (active_tasks.load(std::memory_order_acquire) == 0) return;
    std::unique_lock<std::mutex> lock(queue_mtx);
    cv_done.wait(lock, [this]() {
        return active_tasks.load(std::memory_order_acquire) == 0;
    });
}

/* ========================================================================= */
/* 4. NESTED BATCH/GROUP SCOPES, BMI/MORTON, RDTSC & MICRO-ARCH HINTERS      */
/* ========================================================================= */

struct sf_batch_scope {
    uint32_t batch_id{ 0 };
    uint8_t  priority{ 5 };
    int      bypass_warmup{ 0 };
    bool     completed{ false };
};

struct sf_group_scope {
    uint32_t group_id{ 0 };
    uint8_t  priority{ 5 };
    int      bypass_warmup{ 0 };
    bool     completed{ false };
};

extern "C" {

MSIMD_API uint32_t SF_custominiVm_group_compute(uint8_t priority, int bypass_warmup) {
    uint32_t gid = g_sf_group_counter.fetch_add(1, std::memory_order_relaxed);
    if (priority > 10) priority = 10;
    tl_sf_group_hex_id        = gid;
    tl_sf_group_priority      = priority;
    tl_sf_group_bypass_warmup = (bypass_warmup != 0);

    if (priority >= 8 || bypass_warmup) {
        (void)sf_bind_current_thread(SF_CORE_TIER_PERFORMANCE);
        sf_upclocker_set_boost(1);
        sf_upclocker_t::pin_max_frequency_warmup(10);
    }

    if (SF_custominiVm_get_allow_operation_dispatchings() && priority > 0) {
        char p_msg[256];
        if (priority >= 8) {
            std::snprintf(p_msg, sizeof(p_msg),
                "[SFVM Flag Conflict Warning] High priority (%u) pins execution to performance vector units and overrides queue dispatching.",
                static_cast<unsigned int>(priority));
            sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_BEGIN, SF_Ebin1_WARN, gid, "prio_override_disp", p_msg);
        } else {
            std::snprintf(p_msg, sizeof(p_msg),
                "[SFVM Notice] 'priority' (%u) assigned under active 'Allow_Operation_Dispatchings'.",
                static_cast<unsigned int>(priority));
            sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_BEGIN, SF_Ebin1_INFO, gid, "prio_under_disp", p_msg);
        }
    }
    return gid;
}

MSIMD_API void SF_custominiVm_group_compute_reset(void) {
    uint32_t prev_gid = tl_sf_group_hex_id;
    tl_sf_group_hex_id        = 0;
    tl_sf_group_priority      = 5;
    tl_sf_group_bypass_warmup = false;
    sf_hotlog_emit(SF_Ebin0_GROUP_COMPUTE_RESET, SF_Ebin1_INFO, prev_gid, 0, 0, 0, 0, 0, 0, "group_compute_reset");
    auto* nexus = sf_get_hardware_nexus();
    if (nexus && nexus->focus.is_homogeneous) {
        (void)sf_bind_current_thread(SF_CORE_TIER_ANY);
    }
}

MSIMD_API uint32_t SF_custominiVm_batch_begin(uint8_t priority, int bypass_warmup) {
    if (tl_sf_batch_depth < 16) {
        tl_sf_batch_stack[tl_sf_batch_depth].group_id      = tl_sf_group_hex_id;
        tl_sf_batch_stack[tl_sf_batch_depth].priority      = tl_sf_group_priority;
        tl_sf_batch_stack[tl_sf_batch_depth].bypass_warmup = tl_sf_group_bypass_warmup;
        tl_sf_batch_depth++;
    }
    uint32_t gid = SF_custominiVm_group_compute(priority, bypass_warmup);
    g_sf_active_batch_id.store(gid, std::memory_order_release);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "[SFVM Entangle/BatchScope] Opened batch processing block [0x%08X] (priority=%u, bypass=%d, depth=%u)",
        static_cast<unsigned int>(gid), static_cast<unsigned int>(priority), bypass_warmup,
        static_cast<unsigned int>(tl_sf_batch_depth));
    sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_BEGIN, SF_Ebin1_INFO, gid, "batch_scope_begin", buf);
    return gid;
}

MSIMD_API void SF_custominiVm_batch_end(void) {
    uint32_t gid = tl_sf_group_hex_id;
    if (tl_sf_batch_depth > 0) {
        tl_sf_batch_depth--;
        const auto& prev          = tl_sf_batch_stack[tl_sf_batch_depth];
        tl_sf_group_hex_id        = prev.group_id;
        tl_sf_group_priority      = prev.priority;
        tl_sf_group_bypass_warmup = prev.bypass_warmup;
        g_sf_active_batch_id.store(prev.group_id, std::memory_order_release);
        if (tl_sf_batch_depth == 0) {
            SF_custominiVm_group_compute_reset();
        }
    } else {
        SF_custominiVm_group_compute_reset();
        g_sf_active_batch_id.store(0, std::memory_order_release);
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "[SFVM Entangle/BatchScope] Closed batch processing block [0x%08X] (depth=%u)",
        static_cast<unsigned int>(gid), static_cast<unsigned int>(tl_sf_batch_depth));
    sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_RESET, SF_Ebin1_INFO, gid, "batch_scope_end", buf);
}

MSIMD_API void SF_custominiVm_batch_commit(uint32_t batch_id) {
    (void)batch_id;
    SF_custominiVm_entangle_commit();
    SF_custominiVm_batch_end();
}

MSIMD_API void SF_custominiVm_batch_abort(uint32_t batch_id) {
    (void)batch_id;
    g_sf_entangle_engine.reset();
    SF_custominiVm_batch_end();
}

MSIMD_API uint32_t SF_custominiVm_get_current_batch_id(void) {
    return g_sf_active_batch_id.load(std::memory_order_acquire);
}

MSIMD_API sf_batch_handle SF_custominiVm_batch_scope_begin(uint8_t priority, int bypass_warmup) {
    auto* h          = new sf_batch_scope();
    h->batch_id      = SF_custominiVm_batch_begin(priority, bypass_warmup);
    h->priority      = priority;
    h->bypass_warmup = bypass_warmup;
    h->completed     = false;
    SF_custominiVm_entangle_begin();
    return h;
}

MSIMD_API void SF_custominiVm_batch_scope_commit(sf_batch_handle handle) {
    if (handle && !handle->completed) {
        SF_custominiVm_batch_commit(handle->batch_id);
        handle->completed = true;
    }
}

MSIMD_API void SF_custominiVm_batch_scope_abort(sf_batch_handle handle) {
    if (handle && !handle->completed) {
        SF_custominiVm_batch_abort(handle->batch_id);
        handle->completed = true;
    }
}

MSIMD_API void SF_custominiVm_batch_scope_destroy(sf_batch_handle handle) {
    if (handle) {
        if (!handle->completed) {
            SF_custominiVm_batch_commit(handle->batch_id);
        }
        delete handle;
    }
}

MSIMD_API uint32_t SF_custominiVm_batch_scope_get_id(sf_batch_handle handle) {
    return handle ? handle->batch_id : 0;
}

MSIMD_API sf_group_handle SF_custominiVm_group_scope_begin(uint8_t priority, int bypass_warmup) {
    auto* h          = new sf_group_scope();
    h->group_id      = SF_custominiVm_group_compute(priority, bypass_warmup);
    h->priority      = priority;
    h->bypass_warmup = bypass_warmup;
    h->completed     = false;
    return h;
}

MSIMD_API uint32_t SF_custominiVm_group_scope_get_id(sf_group_handle handle) {
    return handle ? handle->group_id : 0;
}

MSIMD_API void SF_custominiVm_group_scope_end(sf_group_handle handle) {
    if (handle && !handle->completed) {
        SF_custominiVm_group_compute_reset();
        handle->completed = true;
    }
}

MSIMD_API void SF_custominiVm_group_scope_destroy(sf_group_handle handle) {
    if (handle) {
        if (!handle->completed) {
            SF_custominiVm_group_compute_reset();
        }
        delete handle;
    }
}

/* ── BMI1 / BMI2 / Morton-3D Intrinsics ── */

MSIMD_API uint32_t sf_bmi_tzcnt32(uint32_t x) {
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

MSIMD_API uint32_t sf_bmi_tzcnt64(uint64_t x) {
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

MSIMD_API uint32_t sf_bmi_lzcnt32(uint32_t x) {
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

MSIMD_API uint32_t sf_bmi_lzcnt64(uint64_t x) {
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

MSIMD_API uint32_t sf_bmi_popcnt32(uint32_t x) {
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

MSIMD_API uint32_t sf_bmi_popcnt64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_popcountll(x));
#elif defined(_M_X64) && defined(_MSC_VER)
    return static_cast<uint32_t>(__popcnt64(x));
#else
    return sf_bmi_popcnt32(static_cast<uint32_t>(x)) + sf_bmi_popcnt32(static_cast<uint32_t>(x >> 32));
#endif
}

MSIMD_API uint64_t sf_bmi_blsr64(uint64_t x) {
    return x & (x - 1ULL);
}

MSIMD_API uint64_t sf_bmi2_pext64(uint64_t src, uint64_t mask) {
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

MSIMD_API uint64_t sf_bmi2_pdep64(uint64_t src, uint64_t mask) {
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

MSIMD_API uint64_t sf_morton_encode3d(uint32_t x, uint32_t y, uint32_t z) {
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

MSIMD_API uint32_t SF_custominiVm_tzcnt32(uint32_t x) { return sf_bmi_tzcnt32(x); }
MSIMD_API uint32_t SF_custominiVm_tzcnt64(uint64_t x) { return sf_bmi_tzcnt64(x); }
MSIMD_API uint32_t SF_custominiVm_lzcnt32(uint32_t x) { return sf_bmi_lzcnt32(x); }
MSIMD_API uint32_t SF_custominiVm_lzcnt64(uint64_t x) { return sf_bmi_lzcnt64(x); }
MSIMD_API uint32_t SF_custominiVm_popcnt32(uint32_t x) { return sf_bmi_popcnt32(x); }
MSIMD_API uint32_t SF_custominiVm_popcnt64(uint64_t x) { return sf_bmi_popcnt64(x); }
MSIMD_API uint64_t SF_custominiVm_blsr64(uint64_t x) { return sf_bmi_blsr64(x); }
MSIMD_API uint64_t SF_custominiVm_pext64(uint64_t src, uint64_t mask) { return sf_bmi2_pext64(src, mask); }
MSIMD_API uint64_t SF_custominiVm_pdep64(uint64_t src, uint64_t mask) { return sf_bmi2_pdep64(src, mask); }
MSIMD_API uint64_t SF_custominiVm_morton_encode3d(uint32_t x, uint32_t y, uint32_t z) { return sf_morton_encode3d(x, y, z); }

/* ── Cycle-Accurate RDTSC / CNTVCT_EL0 Telemetry ── */

MSIMD_API uint64_t sf_rdtsc_begin(void) {
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

MSIMD_API uint64_t sf_rdtsc_end(void) {
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

MSIMD_API uint64_t sf_rdtsc_freq_hz(void) {
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
    uint64_t start = sf_rdtsc_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t end = sf_rdtsc_end();
    return (end > start) ? ((end - start) * 200ULL) : 1000000000ULL;
}

MSIMD_API double sf_cycles_to_ns(uint64_t cycles, uint64_t freq_hz) {
    if (freq_hz == 0) return 0.0;
    return static_cast<double>(cycles) / static_cast<double>(freq_hz) * 1e9;
}

MSIMD_API uint64_t SF_custominiVm_rdtsc_begin(void) { return sf_rdtsc_begin(); }
MSIMD_API uint64_t SF_custominiVm_rdtsc_end(void) { return sf_rdtsc_end(); }
MSIMD_API uint64_t SF_custominiVm_rdtsc_freq_hz(void) { return sf_rdtsc_freq_hz(); }
MSIMD_API double   SF_custominiVm_cycles_to_ns(uint64_t cycles, uint64_t freq_hz) { return sf_cycles_to_ns(cycles, freq_hz); }

/* ── Core Thread Pinning & Priority Steering ── */

MSIMD_API int sf_thread_pin_to_core(int logical_cpu) {
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

MSIMD_API int sf_thread_set_priority_high(void) {
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

MSIMD_API int SF_custominiVm_thread_pin_to_core(int logical_cpu) { return sf_thread_pin_to_core(logical_cpu); }
MSIMD_API int SF_custominiVm_thread_set_priority_high(void) { return sf_thread_set_priority_high(); }

/* ── Micro-Architectural Branchless Hinters & SIMD Vector Blend ── */

MSIMD_API int32_t sf_hinter_cmov_i32(int condition, int32_t val_true, int32_t val_false, const void* precision_target) {
    (void)precision_target;
    if (!SF_custominiVm_is_iasm_active(0)) {
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

MSIMD_API int64_t sf_hinter_cmov_i64(int condition, int64_t val_true, int64_t val_false, const void* precision_target) {
    (void)precision_target;
    if (!SF_custominiVm_is_iasm_active(0)) {
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

MSIMD_API float sf_hinter_cmov_f32(int condition, float val_true, float val_false, const SF_custominiVm_tensor_t* precision_target) {
    (void)precision_target;
    uint32_t u_true = 0, u_false = 0, u_res = 0;
    float result = 0.0f;
    std::memcpy(&u_true, &val_true, sizeof(float));
    std::memcpy(&u_false, &val_false, sizeof(float));
    u_res = static_cast<uint32_t>(sf_hinter_cmov_i32(condition, static_cast<int32_t>(u_true), static_cast<int32_t>(u_false), nullptr));
    std::memcpy(&result, &u_res, sizeof(float));
    return result;
}

MSIMD_API double sf_hinter_cmov_f64(int condition, double val_true, double val_false, const SF_custominiVm_tensor_t* precision_target) {
    (void)precision_target;
    uint64_t u_true = 0, u_false = 0, u_res = 0;
    double result = 0.0;
    std::memcpy(&u_true, &val_true, sizeof(double));
    std::memcpy(&u_false, &val_false, sizeof(double));
    u_res = static_cast<uint64_t>(sf_hinter_cmov_i64(condition, static_cast<int64_t>(u_true), static_cast<int64_t>(u_false), nullptr));
    std::memcpy(&result, &u_res, sizeof(double));
    return result;
}

MSIMD_API void sf_hinter_blend_f32(float* dst, const float* a, const float* b, const uint32_t* mask, size_t count, const SF_custominiVm_tensor_t* precision_target) {
    (void)precision_target;
    if (!dst || !a || !b || !mask) return;
    size_t i = 0;
#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
    // Note: _mm256_blendv_ps selects via sign bit (bit 31), so compare mask != 0 to all-ones mask first!
    const __m256i vzero = _mm256_setzero_si256();
    for (; i + 8 <= count; i += 8) {
        __m256  va     = _mm256_loadu_ps(a + i);
        __m256  vb     = _mm256_loadu_ps(b + i);
        __m256i vm_raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mask + i));
        __m256i veq0   = _mm256_cmpeq_epi32(vm_raw, vzero);
        __m256  vsel   = _mm256_castsi256_ps(veq0);
        // where veq0 is all-1s (mask[k] == 0), pick vb; otherwise pick va
        __m256  vres   = _mm256_blendv_ps(va, vb, vsel);
        _mm256_storeu_ps(dst + i, vres);
    }
#endif
    for (; i < count; ++i) {
        dst[i] = mask[i] ? a[i] : b[i];
    }
}

MSIMD_API int sf_hinter_should_stream(size_t dataset_bytes) {
    auto* nexus = sf_get_hardware_nexus();
    if (nexus && nexus->initialized.load(std::memory_order_acquire)) {
        return nexus->sram.should_stream_store(dataset_bytes) ? 1 : 0;
    }
    return (dataset_bytes >= 64 * 1024) ? 1 : 0;
}

MSIMD_API int sf_hinter_should_pin_pcore(double flops_per_byte) {
    return (flops_per_byte >= 4.0) ? 1 : 0;
}

MSIMD_API size_t sf_hinter_optimal_prefetch_distance(size_t cacheline_bytes) {
    if (cacheline_bytes == 0) cacheline_bytes = 64;
    return cacheline_bytes * 4;
}

MSIMD_API int32_t SF_custominiVm_hinter_cmov_i32(int c, int32_t vt, int32_t vf, const SF_custominiVm_tensor_t* p) { return sf_hinter_cmov_i32(c, vt, vf, p); }
MSIMD_API int64_t SF_custominiVm_hinter_cmov_i64(int c, int64_t vt, int64_t vf, const SF_custominiVm_tensor_t* p) { return sf_hinter_cmov_i64(c, vt, vf, p); }
MSIMD_API float   SF_custominiVm_hinter_cmov_f32(int c, float vt, float vf, const SF_custominiVm_tensor_t* p) { return sf_hinter_cmov_f32(c, vt, vf, p); }
MSIMD_API double  SF_custominiVm_hinter_cmov_f64(int c, double vt, double vf, const SF_custominiVm_tensor_t* p) { return sf_hinter_cmov_f64(c, vt, vf, p); }
MSIMD_API void    SF_custominiVm_hinter_blend_f32(float* dst, const float* a, const float* b, const uint32_t* m, size_t count, const SF_custominiVm_tensor_t* p) { sf_hinter_blend_f32(dst, a, b, m, count, p); }
MSIMD_API int     SF_custominiVm_hinter_should_stream(size_t dataset_bytes) { return sf_hinter_should_stream(dataset_bytes); }
MSIMD_API int     SF_custominiVm_hinter_should_pin_pcore(double flops_per_byte) { return sf_hinter_should_pin_pcore(flops_per_byte); }
MSIMD_API size_t  SF_custominiVm_hinter_optimal_prefetch_distance(size_t cacheline_bytes) { return sf_hinter_optimal_prefetch_distance(cacheline_bytes); }

} // extern "C"
