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

#ifndef SHARE_GC_REDLET_REDLETDISPATCHISA_HPP
#define SHARE_GC_REDLET_REDLETDISPATCHISA_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <memory>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletEntangle.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletIntentCLI.hpp"
#include "gc/redlet/redletALU.hpp"

#ifndef SACFOR_MAX_INSTANCES
#define SACFOR_MAX_INSTANCES 4096
#endif

#ifndef SACFOR_MAX_DISPATCH_SLOTS
#define SACFOR_MAX_DISPATCH_SLOTS 4096
#endif

/* Vector ALU Opcodes */
enum sf_vector_alu_op_e : uint32_t {
    SF_VEC_OP_ADD               = 0x0001,
    SF_VEC_OP_SUB               = 0x0002,
    SF_VEC_OP_MUL               = 0x0003,
    SF_VEC_OP_DIV               = 0x0004,
    SF_VEC_OP_FMA               = 0x0005,
    SF_VEC_OP_DOT               = 0x0006,
    SF_VEC_OP_MIN               = 0x0007,
    SF_VEC_OP_MAX               = 0x0008,
    SF_VEC_OP_AND               = 0x0009,
    SF_VEC_OP_OR                = 0x000A,
    SF_VEC_OP_XOR               = 0x000B,
    SF_VEC_OP_MULTI_LIMB_ADD    = 0x0010,
    SF_VEC_OP_MULTI_LIMB_MUL    = 0x0011,
    SF_VEC_OP_CARD_TABLE_SCAN   = 0x0020,
    SF_VEC_OP_MEMZERO_FAST      = 0x0021,
    SF_VEC_OP_MAX_OP            = 0x0030
};

typedef void (*SF_custominiVm_kernel_fn_t)(const SF_custominiVm_tensor_t* inputs, int n_in,
                                           SF_custominiVm_tensor_t* outputs, int n_out);
using sacculus_kernel_fn_t = SF_custominiVm_kernel_fn_t;
using sfKernelFn           = SF_custominiVm_kernel_fn_t;

/* ========================================================================= */
/* 1. LOCK-FREE CAS INSTANCE POOL & SEQLOCK KERNEL DISPATCH TABLE            */
/* ========================================================================= */

struct SACFOR_ALIGN_CL sacculus_slot_t {
    sacculus_live_header_t* obj{ nullptr };
    std::atomic<int>        free{ 1 };
};
using sfSlot = sacculus_slot_t;

struct sacculus_instance_pool_t {
    uint32_t              function_id{ 0 };
    char                  name[32]{ 0 };
    sacculus_slot_t       slots[SACFOR_MAX_INSTANCES]{};
    std::atomic<uint32_t> used_count{ 0 };
    uint64_t              type_sig_hash{ 0 };

    int32_t acquire(sacculus_live_header_t* obj) noexcept;
    void    release(int32_t idx) noexcept;
};
using sfInstancePool = sacculus_instance_pool_t;

struct SACFOR_ALIGN_CL sacculus_dispatch_entry_t {
    std::atomic<sacculus_kernel_fn_t>    fn{ nullptr };
    std::atomic<uint32_t>                seqlock_version{ 0 };
    uint32_t                             function_id{ 0 };
    std::atomic<sacculus_live_header_t*> cached_live_header{ nullptr };
    uint64_t                             best_cycles{ UINT64_MAX };
    uint64_t                             call_count{ 0 };
};
using sfDispatchEntry = sacculus_dispatch_entry_t;

struct sacculus_dispatch_table_t {
    sacculus_dispatch_entry_t entries[SACFOR_MAX_DISPATCH_SLOTS]{};

    sacculus_dispatch_entry_t* get(uint32_t fn_id) noexcept;
    void                       register_fn(uint32_t fn_id, sacculus_kernel_fn_t fn) noexcept;
    void                       swap(uint32_t fn_id, sacculus_kernel_fn_t new_fn, uint64_t best) noexcept;
    sacculus_kernel_fn_t       read_fn_seqlock(uint32_t fn_id, uint64_t* out_best_cycles = nullptr) const noexcept;
    void                       init_default_kernels() noexcept;
};
using sfDispatchTable = sacculus_dispatch_table_t;

extern sacculus_dispatch_table_t g_sf_dispatch_table;
extern sacculus_instance_pool_t  g_sf_instance_pool;

/* ========================================================================= */
/* 2. GLOBAL 65+ WORLD ISA CATALOG & FALLBACK HIERARCHY                      */
/* ========================================================================= */

struct sf_world_isa_entry_t {
    const char* isa_name;
    const char* family;
    uint16_t    vector_bits;
    const char* fallback_isa;
};

const sf_world_isa_entry_t* sf_find_world_isa(const char* name) noexcept;
size_t                      sf_world_isa_catalog_count() noexcept;
const sf_world_isa_entry_t* sf_world_isa_catalog_at(size_t idx) noexcept;
size_t                      sf_resolve_isa_fallback_chain(const char* start_isa,
                                                          const sf_world_isa_entry_t** out_chain,
                                                          size_t max_depth) noexcept;

/* ========================================================================= */
/* 3. STANDALONE PINNED FALLBACK WORKER THREAD POOL                          */
/* ========================================================================= */

struct sf_fallback_pool_t {
    struct worker_slot_t {
        std::mutex                        mtx;
        std::condition_variable           cv;
        std::queue<std::function<void()>> slot_tasks;
    };

    std::vector<std::thread>                    workers;
    std::vector<std::unique_ptr<worker_slot_t>> worker_slots;
    std::queue<std::function<void()>>           tasks;
    std::mutex                                  queue_mtx;
    std::condition_variable                     cv_task;
    std::condition_variable                     cv_done;
    std::atomic<bool>                           stop{ false };
    std::atomic<size_t>                         active_tasks{ 0 };
    int                                         thread_count{ 1 };

    explicit sf_fallback_pool_t(int num_threads = 0);
    ~sf_fallback_pool_t();

    void submit(std::function<void()> task);
    void submit_to_worker(int worker_idx, std::function<void()> task);
    void wait_all();
};
using FallbackThreadPool = sf_fallback_pool_t;

/* ========================================================================= */
/* 4. VECTOR ALU KERNELS (Built-in Hardware-Accelerated & Emulated)          */
/* ========================================================================= */

// FP32 Vector Kernels
MSIMD_API void sf_kernel_vec_add_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_sub_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_mul_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_fma_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_dot_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_min_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_max_f32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);

// FP64 Vector Kernels
MSIMD_API void sf_kernel_vec_add_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_sub_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_mul_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_fma_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_dot_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_min_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_max_f64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);

// INT32 Vector Kernels
MSIMD_API void sf_kernel_vec_add_i32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_sub_i32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_mul_i32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_and_i32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_or_i32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_xor_i32(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);

// INT64 Vector Kernels
MSIMD_API void sf_kernel_vec_add_i64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_sub_i64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_mul_i64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_and_i64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_or_i64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_vec_xor_i64(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);

// Multi-Limb Emulated Kernels (Section 14 & 15)
MSIMD_API void sf_kernel_emu_double_double_add(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_double_double_mul(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_quad_double_add(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_quad_double_mul(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_int128_add(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_int128_mul(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_int256_add(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_emu_int256_mul(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);

// GC Internal Fast-Path Kernels
MSIMD_API void sf_kernel_gc_card_table_scan(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);
MSIMD_API void sf_kernel_gc_memzero(const SF_custominiVm_tensor_t* in, int nin, SF_custominiVm_tensor_t* out, int nout);

/* Fast-Path Arithmetic Routing */
MSIMD_API int sf_dispatch_fast_path_arithmetic(uint32_t op_id, uint8_t dtype,
                                               const void* a, const void* b, void* out,
                                               size_t count, uint8_t priority);

/* ========================================================================= */
/* 5. HOTSPOT C++20 STATIC INTERFACE (RedletDispatchISA)                     */
/* ========================================================================= */

class RedletDispatchISA {
public:
    static inline void init() noexcept {
        g_sf_dispatch_table.init_default_kernels();
    }

    static inline sacculus_dispatch_table_t& table() noexcept {
        return g_sf_dispatch_table;
    }

    static inline sacculus_instance_pool_t& pool() noexcept {
        return g_sf_instance_pool;
    }

    static inline bool execute(uint32_t fn_id, const SF_custominiVm_tensor_t* in, int nin,
                               SF_custominiVm_tensor_t* out, int nout) noexcept {
        sacculus_kernel_fn_t fn = g_sf_dispatch_table.read_fn_seqlock(fn_id);
        if (!fn) return false;
        fn(in, nin, out, nout);
        return true;
    }

    static inline int dispatch_fast_path(uint32_t op_id, uint8_t dtype,
                                         const void* a, const void* b, void* out,
                                         size_t count, uint8_t priority = 5) {
        return sf_dispatch_fast_path_arithmetic(op_id, dtype, a, b, out, count, priority);
    }

    static inline const sf_world_isa_entry_t* find_isa(const char* name) noexcept {
        return sf_find_world_isa(name);
    }

    static inline size_t catalog_count() noexcept {
        return sf_world_isa_catalog_count();
    }

    static inline const sf_world_isa_entry_t* catalog_at(size_t idx) noexcept {
        return sf_world_isa_catalog_at(idx);
    }

    static inline size_t resolve_isa_chain(const char* start_isa, const sf_world_isa_entry_t** out_chain, size_t max_depth) noexcept {
        return sf_resolve_isa_fallback_chain(start_isa, out_chain, max_depth);
    }
};

/* ========================================================================= */
/* 6. NESTED BATCH/GROUP SCOPES, BMI/MORTON, RDTSC & MICRO-ARCH HINTERS      */
/* ========================================================================= */

struct sf_batch_scope;
struct sf_group_scope;
typedef sf_batch_scope* sf_batch_handle;
typedef sf_group_scope* sf_group_handle;

uint32_t sf_get_thread_group_hex_id() noexcept;
uint8_t  sf_get_thread_group_priority() noexcept;
bool     sf_get_thread_group_bypass_warmup() noexcept;

#ifdef __cplusplus
extern "C" {
#endif

// Nested GroupCompute & 16-Deep BatchScope Stack
MSIMD_API uint32_t        SF_custominiVm_group_compute(uint8_t priority, int bypass_warmup);
MSIMD_API void            SF_custominiVm_group_compute_reset(void);
MSIMD_API uint32_t        SF_custominiVm_batch_begin(uint8_t priority, int bypass_warmup);
MSIMD_API void            SF_custominiVm_batch_end(void);
MSIMD_API void            SF_custominiVm_batch_commit(uint32_t batch_id);
MSIMD_API void            SF_custominiVm_batch_abort(uint32_t batch_id);
MSIMD_API uint32_t        SF_custominiVm_get_current_batch_id(void);

MSIMD_API sf_batch_handle SF_custominiVm_batch_scope_begin(uint8_t priority, int bypass_warmup);
MSIMD_API void            SF_custominiVm_batch_scope_commit(sf_batch_handle handle);
MSIMD_API void            SF_custominiVm_batch_scope_abort(sf_batch_handle handle);
MSIMD_API void            SF_custominiVm_batch_scope_destroy(sf_batch_handle handle);
MSIMD_API uint32_t        SF_custominiVm_batch_scope_get_id(sf_batch_handle handle);

MSIMD_API sf_group_handle SF_custominiVm_group_scope_begin(uint8_t priority, int bypass_warmup);
MSIMD_API uint32_t        SF_custominiVm_group_scope_get_id(sf_group_handle handle);
MSIMD_API void            SF_custominiVm_group_scope_end(sf_group_handle handle);
MSIMD_API void            SF_custominiVm_group_scope_destroy(sf_group_handle handle);

// BMI1 / BMI2 / 3D Morton Spatial Indexing Intrinsics
MSIMD_API uint32_t sf_bmi_tzcnt32(uint32_t x);
MSIMD_API uint32_t sf_bmi_tzcnt64(uint64_t x);
MSIMD_API uint32_t sf_bmi_lzcnt32(uint32_t x);
MSIMD_API uint32_t sf_bmi_lzcnt64(uint64_t x);
MSIMD_API uint32_t sf_bmi_popcnt32(uint32_t x);
MSIMD_API uint32_t sf_bmi_popcnt64(uint64_t x);
MSIMD_API uint64_t sf_bmi_blsr64(uint64_t x);
MSIMD_API uint64_t sf_bmi2_pext64(uint64_t src, uint64_t mask);
MSIMD_API uint64_t sf_bmi2_pdep64(uint64_t src, uint64_t mask);
MSIMD_API uint64_t sf_morton_encode3d(uint32_t x, uint32_t y, uint32_t z);

MSIMD_API uint32_t SF_custominiVm_tzcnt32(uint32_t x);
MSIMD_API uint32_t SF_custominiVm_tzcnt64(uint64_t x);
MSIMD_API uint32_t SF_custominiVm_lzcnt32(uint32_t x);
MSIMD_API uint32_t SF_custominiVm_lzcnt64(uint64_t x);
MSIMD_API uint32_t SF_custominiVm_popcnt32(uint32_t x);
MSIMD_API uint32_t SF_custominiVm_popcnt64(uint64_t x);
MSIMD_API uint64_t SF_custominiVm_blsr64(uint64_t x);
MSIMD_API uint64_t SF_custominiVm_pext64(uint64_t src, uint64_t mask);
MSIMD_API uint64_t SF_custominiVm_pdep64(uint64_t src, uint64_t mask);
MSIMD_API uint64_t SF_custominiVm_morton_encode3d(uint32_t x, uint32_t y, uint32_t z);

// Cycle-Accurate RDTSC / CNTVCT_EL0 Telemetry
MSIMD_API uint64_t sf_rdtsc_begin(void);
MSIMD_API uint64_t sf_rdtsc_end(void);
MSIMD_API uint64_t sf_rdtsc_freq_hz(void);
MSIMD_API double   sf_cycles_to_ns(uint64_t cycles, uint64_t freq_hz);

MSIMD_API uint64_t SF_custominiVm_rdtsc_begin(void);
MSIMD_API uint64_t SF_custominiVm_rdtsc_end(void);
MSIMD_API uint64_t SF_custominiVm_rdtsc_freq_hz(void);
MSIMD_API double   SF_custominiVm_cycles_to_ns(uint64_t cycles, uint64_t freq_hz);

// Core Thread Pinning & Priority Steering
MSIMD_API int sf_thread_pin_to_core(int logical_cpu);
MSIMD_API int sf_thread_set_priority_high(void);
MSIMD_API int SF_custominiVm_thread_pin_to_core(int logical_cpu);
MSIMD_API int SF_custominiVm_thread_set_priority_high(void);

// Micro-Architectural Branchless Hinters & SIMD Vector Blend
MSIMD_API int32_t sf_hinter_cmov_i32(int condition, int32_t val_true, int32_t val_false, const void* precision_target);
MSIMD_API int64_t sf_hinter_cmov_i64(int condition, int64_t val_true, int64_t val_false, const void* precision_target);
MSIMD_API float   sf_hinter_cmov_f32(int condition, float val_true, float val_false, const SF_custominiVm_tensor_t* precision_target);
MSIMD_API double  sf_hinter_cmov_f64(int condition, double val_true, double val_false, const SF_custominiVm_tensor_t* precision_target);
MSIMD_API void    sf_hinter_blend_f32(float* dst, const float* a, const float* b, const uint32_t* mask, size_t count, const SF_custominiVm_tensor_t* precision_target);
MSIMD_API int     sf_hinter_should_stream(size_t dataset_bytes);
MSIMD_API int     sf_hinter_should_pin_pcore(double flops_per_byte);
MSIMD_API size_t  sf_hinter_optimal_prefetch_distance(size_t cacheline_bytes);

MSIMD_API int32_t SF_custominiVm_hinter_cmov_i32(int c, int32_t vt, int32_t vf, const SF_custominiVm_tensor_t* p);
MSIMD_API int64_t SF_custominiVm_hinter_cmov_i64(int c, int64_t vt, int64_t vf, const SF_custominiVm_tensor_t* p);
MSIMD_API float   SF_custominiVm_hinter_cmov_f32(int c, float vt, float vf, const SF_custominiVm_tensor_t* p);
MSIMD_API double  SF_custominiVm_hinter_cmov_f64(int c, double vt, double vf, const SF_custominiVm_tensor_t* p);
MSIMD_API void    SF_custominiVm_hinter_blend_f32(float* dst, const float* a, const float* b, const uint32_t* m, size_t count, const SF_custominiVm_tensor_t* p);
MSIMD_API int     SF_custominiVm_hinter_should_stream(size_t dataset_bytes);
MSIMD_API int     SF_custominiVm_hinter_should_pin_pcore(double flops_per_byte);
MSIMD_API size_t  SF_custominiVm_hinter_optimal_prefetch_distance(size_t cacheline_bytes);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETDISPATCHISA_HPP
