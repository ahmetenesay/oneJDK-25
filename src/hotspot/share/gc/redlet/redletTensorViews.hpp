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

#ifndef SHARE_GC_REDLET_REDLETTENSORVIEWS_HPP
#define SHARE_GC_REDLET_REDLETTENSORVIEWS_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletALU.hpp"
#include "gc/redlet/redletEntangle.hpp"
#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletRunway.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include "gc/redlet/redletIntentCLI.hpp"

/* ========================================================================= */
/* 1. VFS STAT, PANIC HOOK, SBC/GPIO & RING-0 BRIDGE STRUCTURES              */
/* ========================================================================= */

typedef struct SF_custominiVm_vfs_stat_t {
    uint64_t inode_no;
    size_t   size_bytes;
    size_t   used_bytes;
    size_t   quota_bytes;
    uint32_t child_count;
    uint8_t  is_dir;
    uint8_t  _pad[3];
} SF_custominiVm_vfs_stat_t;

typedef void (*sf_vfs_panic_hook_t)(const char* missing_path, const char* operation_context, void* user_data);

typedef enum SF_custominiVm_sbc_board_e {
    SF_SBC_NONE                = 0,
    SF_SBC_RASPBERRY_PI_1_ZERO = 1,
    SF_SBC_RASPBERRY_PI_2_3    = 2,
    SF_SBC_RASPBERRY_PI_4      = 3,
    SF_SBC_RASPBERRY_PI_5      = 4,
    SF_SBC_ORANGE_PI           = 5,
    SF_SBC_NVIDIA_JETSON       = 6,
    SF_SBC_LATTEPANDA_X86      = 7
} SF_custominiVm_sbc_board_e;

typedef enum SF_custominiVm_gpio_pin_mode_e {
    SF_GPIO_MODE_INPUT  = 0,
    SF_GPIO_MODE_OUTPUT = 1,
    SF_GPIO_MODE_ALT0   = 2
} SF_custominiVm_gpio_pin_mode_e;

typedef struct SF_custominiVm_gpio_desc_t {
    SF_custominiVm_sbc_board_e board_type;
    uint32_t                   total_pins;
    uint64_t                   mmio_phys_base;
    void*                      mmio_virtual_base;
    uint8_t                    is_fast_mmio_active;
    uint8_t                    is_coprocessor_bridge;
    uint8_t                    _pad[6];
    char                       board_name[96];
} SF_custominiVm_gpio_desc_t;

typedef struct SF_custominiVm_ring0_status_t {
    uint8_t  is_driver_loaded;
    uint8_t  _pad[3];
    uint32_t driver_version;
} SF_custominiVm_ring0_status_t;

typedef struct SF_custominiVm_ring0_hwp_policy_t {
    uint8_t min_perf_pct;
    uint8_t max_perf_pct;
    uint8_t energy_perf_pref;
    uint8_t anti_downclock_boost;
} SF_custominiVm_ring0_hwp_policy_t;

/* ========================================================================= */
/* 2. QUOTA-ENFORCED HIERARCHICAL VFS FOLDER TREE (sacculus_folder_tree_t)   */
/* ========================================================================= */

struct sf_folder_node_t {
    char                name[64]{ 0 };
    char                full_path[192]{ 0 };
    size_t              quota_bytes{ 0 };
    std::atomic<size_t> used_bytes{ 0 };

    sf_folder_node_t*                          parent{ nullptr };
    std::vector<sf_folder_node_t*>             children;
    std::vector<SF_custominiVm_live_header_t*> objects;

    ~sf_folder_node_t();
};

struct sacculus_folder_tree_t {
    sf_folder_node_t* root{ nullptr };
    std::mutex        tree_lock;

    sacculus_folder_tree_t() = default;
    ~sacculus_folder_tree_t() { destroy(); }

    void init();
    void destroy();

    static void split_path(const char* path, std::vector<std::string>& parts);

    sf_folder_node_t* _mkdir_p_unlocked(const char* path, size_t quota_bytes = 0);
    sf_folder_node_t* mkdir_p(const char* path, size_t quota_bytes = 0);

    sf_folder_node_t* _find_node_unlocked(const char* path);
    sf_folder_node_t* find_node(const char* path);

    bool   bind_tensor(const char* folder_path, SF_custominiVm_live_header_t* obj);
    size_t evict_folder(const char* folder_path, sacculus_runway_t* runway);
    size_t get_used_bytes(const char* folder_path);
    bool   unlink_file(const char* file_path, sacculus_runway_t* runway);
    bool   stat_path(const char* path, SF_custominiVm_vfs_stat_t* out_stat);
    size_t snapshot_path(const char* path, void* out_buf, size_t cap);
    void   list_dir(const char* path, void (*cb)(const char*, int, size_t, void*), void* user);
};

using sfFolderTree        = sacculus_folder_tree_t;
using sfVfsHeap           = sacculus_folder_tree_t;
using sacculus_vfs_heap_t = sacculus_folder_tree_t;

extern sacculus_folder_tree_t g_sf_vfs_tree;

void               sf_tensor_views_bind_runway(sacculus_runway_t* runway);
sacculus_runway_t* sf_tensor_views_get_runway();

SF_custominiVm_tensor_t sf_tensor_make(void* data, const int32_t* shape, int ndim, sfDType dtype) noexcept;

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
MSIMD_API size_t                  sf_dtype_size(sfDType d);
MSIMD_API sfDType                 sf_dtype_detect(size_t byte_size, bool is_float, bool is_signed);
MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_alloc(const int32_t* shape, int ndim, int dtype);
MSIMD_API void                    SF_custominiVm_tensor_free(SF_custominiVm_tensor_t* tensor);

// Zero-Copy Strided Tensor Views, Slicing, Transpose, Permute, Broadcast & Pinning
MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_slice(const SF_custominiVm_tensor_t* parent,
                                                              size_t offset_bytes,
                                                              const int32_t* shape, int ndim, int dtype);
MSIMD_API int                     SF_custominiVm_tensor_get_strides(const SF_custominiVm_tensor_t* tensor,
                                                                    int64_t* out_strides_bytes);
MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_transpose(const SF_custominiVm_tensor_t* parent,
                                                                  int dim0, int dim1);
MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_permute(const SF_custominiVm_tensor_t* parent,
                                                                const int32_t* perm, int ndim);
MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_broadcast(const SF_custominiVm_tensor_t* parent,
                                                                  const int32_t* target_shape, int target_ndim);
MSIMD_API int                     SF_custominiVm_tensor_pin(SF_custominiVm_tensor_t* tensor);
MSIMD_API int                     SF_custominiVm_tensor_unpin(SF_custominiVm_tensor_t* tensor);
MSIMD_API int                     SF_custominiVm_tensor_is_view(const SF_custominiVm_tensor_t* tensor);
MSIMD_API void*                   SF_custominiVm_tensor_resolve_ptr(const SF_custominiVm_tensor_t* tensor);

// VFS Hierarchy, Quotas & Strict Branch Validation
MSIMD_API int                     SF_custominiVm_folder_mkdir(const char* path, size_t quota_bytes);
MSIMD_API int                     SF_custominiVm_folder_bind(const char* path, SF_custominiVm_tensor_t* tensor);
MSIMD_API size_t                  SF_custominiVm_folder_evict(const char* path);
MSIMD_API size_t                  SF_custominiVm_folder_used_bytes(const char* path);
MSIMD_API int                     SF_custominiVm_vfs_mkdir(const char* path, size_t quota_bytes);
MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_vfs_alloc_tensor(const char* path,
                                                                  const int32_t* shape, int ndim, int dtype);
MSIMD_API int                     SF_custominiVm_vfs_unlink(const char* path);
MSIMD_API size_t                  SF_custominiVm_vfs_rmdir_rf(const char* path);
MSIMD_API int                     SF_custominiVm_vfs_stat(const char* path, SF_custominiVm_vfs_stat_t* out_stat);
MSIMD_API size_t                  SF_custominiVm_vfs_snapshot(const char* path, void* out_buf, size_t buf_cap);
MSIMD_API void                    SF_custominiVm_vfs_ls(const char* path,
                                                        void (*cb)(const char* name, int is_dir, size_t bytes, void* user),
                                                        void* user);

MSIMD_API void                    SF_custominiVm_set_strict_vfs_branch_validation(int enable);
MSIMD_API int                     SF_custominiVm_get_strict_vfs_branch_validation(void);
MSIMD_API int                     SF_custominiVm_is_strict_vfs_branch_validation(void);
MSIMD_API void                    SF_custominiVm_vfs_set_panic_dialog_enabled(int enable);
MSIMD_API int                     SF_custominiVm_vfs_get_panic_dialog_enabled(void);
MSIMD_API void                    SF_custominiVm_vfs_set_panic_behavior(int suppress_abort, int show_dialog);
MSIMD_API void                    SF_custominiVm_vfs_set_panic_hook(sf_vfs_panic_hook_t hook, void* user_data);
MSIMD_API void                    SF_custominiVm_vfs_trigger_panic(const char* missing_path, const char* operation_context);
MSIMD_API uint64_t                SF_custominiVm_vfs_get_panic_count(void);
MSIMD_API const char*             SF_custominiVm_vfs_get_last_panic_path(void);
MSIMD_API const char*             SF_custominiVm_vfs_get_last_panic_op(void);

// SBC / GPIO Fast MMIO & Ring-0 HWP Bridges
MSIMD_API int                     SF_custominiVm_sbc_detect_board(SF_custominiVm_gpio_desc_t* out_desc);
MSIMD_API int                     SF_custominiVm_gpio_init(SF_custominiVm_gpio_desc_t* desc);
MSIMD_API int                     SF_custominiVm_gpio_set_mode(uint32_t pin, SF_custominiVm_gpio_pin_mode_e mode);
MSIMD_API void                    SF_custominiVm_gpio_fast_write(uint32_t pin, uint8_t level);
MSIMD_API uint8_t                 SF_custominiVm_gpio_fast_read(uint32_t pin);

MSIMD_API int                     SF_custominiVm_ring0_connect(SF_custominiVm_ring0_status_t* out_status);
MSIMD_API int                     SF_custominiVm_ring0_apply_hwp_policy(const SF_custominiVm_ring0_hwp_policy_t* policy);
MSIMD_API void                    SF_custominiVm_ring0_disconnect(void);

#ifdef __cplusplus
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

#endif // SHARE_GC_REDLET_REDLETTENSORVIEWS_HPP
