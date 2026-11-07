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

#ifndef SHARE_GC_REDJET_REDJETTENSORVIEWS_HPP
#define SHARE_GC_REDJET_REDJETTENSORVIEWS_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/simd/redjetALU.hpp"
#include "gc/redjet/engine/redjetEntangle.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/allocator/redjetPagefile.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/runtime/redjetIntentCLI.hpp"

/* ========================================================================= */
/* 1. VFS STAT, PANIC HOOK, SBC/GPIO & RING-0 BRIDGE STRUCTURES              */
/* ========================================================================= */

typedef struct HS_hotspot_vfs_stat_t {
    uint64_t inode_no;
    size_t   size_bytes;
    size_t   used_bytes;
    size_t   quota_bytes;
    uint32_t child_count;
    uint8_t  is_dir;
    uint8_t  _pad[3];
} HS_hotspot_vfs_stat_t;

typedef void (*hs_vfs_panic_hook_t)(const char* missing_path, const char* operation_context, void* user_data);

typedef enum HS_hotspot_sbc_board_e {
    HS_SBC_NONE                = 0,
    HS_SBC_RASPBERRY_PI_1_ZERO = 1,
    HS_SBC_RASPBERRY_PI_2_3    = 2,
    HS_SBC_RASPBERRY_PI_4      = 3,
    HS_SBC_RASPBERRY_PI_5      = 4,
    HS_SBC_ORANGE_PI           = 5,
    HS_SBC_NVIDIA_JETSON       = 6,
    HS_SBC_LATTEPANDA_X86      = 7
} HS_hotspot_sbc_board_e;

typedef enum HS_hotspot_gpio_pin_mode_e {
    HS_GPIO_MODE_INPUT  = 0,
    HS_GPIO_MODE_OUTPUT = 1,
    HS_GPIO_MODE_ALT0   = 2
} HS_hotspot_gpio_pin_mode_e;

typedef struct HS_hotspot_gpio_desc_t {
    HS_hotspot_sbc_board_e board_type;
    uint32_t                   total_pins;
    uint64_t                   mmio_phys_base;
    void*                      mmio_virtual_base;
    uint8_t                    is_fast_mmio_active;
    uint8_t                    is_coprocessor_bridge;
    uint8_t                    _pad[6];
    char                       board_name[96];
} HS_hotspot_gpio_desc_t;

typedef struct HS_hotspot_ring0_status_t {
    uint8_t  is_driver_loaded;
    uint8_t  _pad[3];
    uint32_t driver_version;
} HS_hotspot_ring0_status_t;

typedef struct HS_hotspot_ring0_hwp_policy_t {
    uint8_t min_perf_pct;
    uint8_t max_perf_pct;
    uint8_t energy_perf_pref;
    uint8_t anti_downclock_boost;
} HS_hotspot_ring0_hwp_policy_t;

/* ========================================================================= */
/* 2. QUOTA-ENFORCED HIERARCHICAL VFS FOLDER TREE (sacculus_folder_tree_t)   */
/* ========================================================================= */

struct hs_folder_node_t {
    char                name[64]{ 0 };
    char                full_path[192]{ 0 };
    size_t              quota_bytes{ 0 };
    std::atomic<size_t> used_bytes{ 0 };

    hs_folder_node_t*                          parent{ nullptr };
    std::vector<hs_folder_node_t*>             children;
    std::vector<HS_hotspot_live_header_t*> objects;

    ~hs_folder_node_t();
};

struct sacculus_folder_tree_t {
    hs_folder_node_t* root{ nullptr };
    std::mutex        tree_lock;

    sacculus_folder_tree_t() = default;
    ~sacculus_folder_tree_t() { destroy(); }

    void init();
    void destroy();

    static void split_path(const char* path, std::vector<std::string>& parts);

    hs_folder_node_t* _mkdir_p_unlocked(const char* path, size_t quota_bytes = 0);
    hs_folder_node_t* mkdir_p(const char* path, size_t quota_bytes = 0);

    hs_folder_node_t* _find_node_unlocked(const char* path);
    hs_folder_node_t* find_node(const char* path);

    bool   bind_tensor(const char* folder_path, HS_hotspot_live_header_t* obj);
    size_t evict_folder(const char* folder_path, sacculus_runway_t* runway);
    size_t get_used_bytes(const char* folder_path);
    bool   unlink_file(const char* file_path, sacculus_runway_t* runway);
    bool   stat_path(const char* path, HS_hotspot_vfs_stat_t* out_stat);
    size_t snapshot_path(const char* path, void* out_buf, size_t cap);
    void   list_dir(const char* path, void (*cb)(const char*, int, size_t, void*), void* user);
};

using hsFolderTree        = sacculus_folder_tree_t;
using hsVfsHeap           = sacculus_folder_tree_t;
using sacculus_vfs_heap_t = sacculus_folder_tree_t;

extern sacculus_folder_tree_t g_hs_vfs_tree;

void               hs_tensor_views_bind_runway(sacculus_runway_t* runway);
sacculus_runway_t* hs_tensor_views_get_runway();

HS_hotspot_tensor_t hs_tensor_make(void* data, const int32_t* shape, int ndim, hsDType dtype) noexcept;

/* ========================================================================= */
/* 3. C ABI EXPORTS                                                          */
/* ========================================================================= */

#ifdef __cplusplus
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4190)
#endif
extern "C" {
#endif

// DType & Tensor Allocation Lifecycle
RJ_API size_t                  hs_dtype_size(hsDType d);
RJ_API hsDType                 hs_dtype_detect(size_t byte_size, bool is_float, bool is_signed);
RJ_API HS_hotspot_tensor_t HS_hotspot_tensor_alloc(const int32_t* shape, int ndim, int dtype);
RJ_API void                    HS_hotspot_tensor_free(HS_hotspot_tensor_t* tensor);

// Zero-Copy Strided Tensor Views, Slicing, Transpose, Permute, Broadcast & Pinning
RJ_API HS_hotspot_tensor_t HS_hotspot_tensor_slice(const HS_hotspot_tensor_t* parent,
                                                              size_t offset_bytes,
                                                              const int32_t* shape, int ndim, int dtype);
RJ_API int                     HS_hotspot_tensor_get_strides(const HS_hotspot_tensor_t* tensor,
                                                                    int64_t* out_strides_bytes);
RJ_API HS_hotspot_tensor_t HS_hotspot_tensor_transpose(const HS_hotspot_tensor_t* parent,
                                                                  int dim0, int dim1);
RJ_API HS_hotspot_tensor_t HS_hotspot_tensor_permute(const HS_hotspot_tensor_t* parent,
                                                                const int32_t* perm, int ndim);
RJ_API HS_hotspot_tensor_t HS_hotspot_tensor_broadcast(const HS_hotspot_tensor_t* parent,
                                                                  const int32_t* target_shape, int target_ndim);
RJ_API int                     HS_hotspot_tensor_pin(HS_hotspot_tensor_t* tensor);
RJ_API int                     HS_hotspot_tensor_unpin(HS_hotspot_tensor_t* tensor);
RJ_API int                     HS_hotspot_tensor_is_view(const HS_hotspot_tensor_t* tensor);
RJ_API void*                   HS_hotspot_tensor_resolve_ptr(const HS_hotspot_tensor_t* tensor);

// VFS Hierarchy, Quotas & Strict Branch Validation
RJ_API int                     HS_hotspot_folder_mkdir(const char* path, size_t quota_bytes);
RJ_API int                     HS_hotspot_folder_bind(const char* path, HS_hotspot_tensor_t* tensor);
RJ_API size_t                  HS_hotspot_folder_evict(const char* path);
RJ_API size_t                  HS_hotspot_folder_used_bytes(const char* path);
RJ_API int                     HS_hotspot_vfs_mkdir(const char* path, size_t quota_bytes);
RJ_API HS_hotspot_tensor_t HS_hotspot_vfs_alloc_tensor(const char* path,
                                                                  const int32_t* shape, int ndim, int dtype);
RJ_API int                     HS_hotspot_vfs_unlink(const char* path);
RJ_API size_t                  HS_hotspot_vfs_rmdir_rf(const char* path);
RJ_API int                     HS_hotspot_vfs_stat(const char* path, HS_hotspot_vfs_stat_t* out_stat);
RJ_API size_t                  HS_hotspot_vfs_snapshot(const char* path, void* out_buf, size_t buf_cap);
RJ_API void                    HS_hotspot_vfs_ls(const char* path,
                                                        void (*cb)(const char* name, int is_dir, size_t bytes, void* user),
                                                        void* user);

RJ_API void                    HS_hotspot_set_strict_vfs_branch_validation(int enable);
RJ_API int                     HS_hotspot_get_strict_vfs_branch_validation(void);
RJ_API int                     HS_hotspot_is_strict_vfs_branch_validation(void);
RJ_API void                    HS_hotspot_vfs_set_panic_dialog_enabled(int enable);
RJ_API int                     HS_hotspot_vfs_get_panic_dialog_enabled(void);
RJ_API void                    HS_hotspot_vfs_set_panic_behavior(int suppress_abort, int show_dialog);
RJ_API void                    HS_hotspot_vfs_set_panic_hook(hs_vfs_panic_hook_t hook, void* user_data);
RJ_API void                    HS_hotspot_vfs_trigger_panic(const char* missing_path, const char* operation_context);
RJ_API uint64_t                HS_hotspot_vfs_get_panic_count(void);
RJ_API const char*             HS_hotspot_vfs_get_last_panic_path(void);
RJ_API const char*             HS_hotspot_vfs_get_last_panic_op(void);

// SBC / GPIO Fast MMIO & Ring-0 HWP Bridges
RJ_API int                     HS_hotspot_sbc_detect_board(HS_hotspot_gpio_desc_t* out_desc);
RJ_API int                     HS_hotspot_gpio_init(HS_hotspot_gpio_desc_t* desc);
RJ_API int                     HS_hotspot_gpio_set_mode(uint32_t pin, HS_hotspot_gpio_pin_mode_e mode);
RJ_API void                    HS_hotspot_gpio_fast_write(uint32_t pin, uint8_t level);
RJ_API uint8_t                 HS_hotspot_gpio_fast_read(uint32_t pin);

RJ_API int                     HS_hotspot_ring0_connect(HS_hotspot_ring0_status_t* out_status);
RJ_API int                     HS_hotspot_ring0_apply_hwp_policy(const HS_hotspot_ring0_hwp_policy_t* policy);
RJ_API void                    HS_hotspot_ring0_disconnect(void);

#ifdef __cplusplus
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

#endif // SHARE_GC_REDJET_REDJETTENSORVIEWS_HPP
