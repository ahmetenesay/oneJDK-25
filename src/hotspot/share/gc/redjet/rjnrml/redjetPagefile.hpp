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

#ifndef SHARE_GC_REDJET_REDJETPAGEFILE_HPP
#define SHARE_GC_REDJET_REDJETPAGEFILE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <vector>
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/redjetMotherline.hpp"
#include "gc/redjet/rjspcf/redjetEntangle.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/* ========================================================================= */
/* 1. OS VIRTUAL MEMORY ALLOCATION REGISTRY & HUGE-PAGE PRIMITIVES           */
/* ========================================================================= */

enum hs_os_alloc_tag_e : uint32_t {
    HS_OS_TAG_VIRTUAL = 0, // VirtualAlloc / mmap anonymous
    HS_OS_TAG_MMIO    = 1, // Memory-mapped I/O (/dev/gpiomem, /dev/mem)
    HS_OS_TAG_FILEMAP = 2  // MapViewOfFile / file-backed mmap
};

struct hs_os_allocation_entry_t {
    void*    addr{ nullptr };
    size_t   bytes{ 0 };
    uint32_t tag{ HS_OS_TAG_VIRTUAL };
};

struct hs_os_allocation_registry_t {
    static constexpr size_t MAX_TRACKED_OS_BLOCKS = 4096;

    std::recursive_mutex                   lock;
    hs_os_allocation_entry_t               entries[MAX_TRACKED_OS_BLOCKS]{};
    std::vector<hs_os_allocation_entry_t>  overflow_entries;
    std::atomic<size_t>                    active_count{ 0 };

    void   register_block(void* addr, size_t bytes, uint32_t tag = HS_OS_TAG_VIRTUAL);
    void   unregister_block(void* addr);
    size_t sweep_all();
};

extern hs_os_allocation_registry_t g_hs_os_registry;
extern std::atomic<bool>           g_hs_enable_huge_pages;

void* hs_os_reserve_memory(size_t bytes);
bool  hs_os_commit_memory(void* addr, size_t bytes);
void  hs_os_decommit_memory(void* addr, size_t bytes);
void  hs_os_release_memory(void* addr, size_t bytes = 0);

/* ========================================================================= */
/* 2. ANONYMOUS PAGEFILE MAPPING & HEAP SUPERBLOCK (HS_HEAP_MAGIC)           */
/* ========================================================================= */

struct hs_anonymous_pagefile_t {
    void*  mapped_base{ nullptr };
    size_t mapped_capacity{ 0 };
#ifdef _WIN32
    HANDLE hMap{ NULL };
#endif

    bool init(size_t capacity_bytes);
    void destroy();
};

#define HS_HEAP_MAGIC 0x53464850U

struct SACFOR_ALIGN_CL hs_heap_superblock_t {
    uint32_t magic{ HS_HEAP_MAGIC };
    uint32_t version{ 1 };
    size_t   total_reserved_bytes{ 0 };
    size_t   total_committed_bytes{ 0 };
    size_t   page_size{ 4096 };
    size_t   alignment{ 64 };

    size_t   slabs_offset{ 0 };
    size_t   slabs_capacity{ 0 };
    size_t   bump_arena_offset{ 0 };
    size_t   bump_arena_capacity{ 0 };
    size_t   los_offset{ 0 };
    size_t   los_capacity{ 0 };

    size_t   puzzle_arena_offset{ 0 };
    size_t   puzzle_arena_capacity{ 0 };

    std::atomic<size_t>   allocated_bytes{ 0 };
    std::atomic<size_t>   rebated_bytes{ 0 };
    std::atomic<uint32_t> rebate_cycle_count{ 0 };

    void*    next_chained_block{ nullptr };
    uint8_t  _pad[4]{ 0 };
};

/* ========================================================================= */
/* 3. COLD OBJECT METADATA SIDE-TABLE (NON-INTRUSIVE TO hsLiveHeader)        */
/* ========================================================================= */

struct hs_cold_object_metadata_t {
    const HS_custominiVm_live_header_t* owner{ nullptr };
    std::atomic<uint8_t>                is_paged_out{ 0 };
    uint64_t                            pagefile_slot_offset{ 0 };
    std::atomic<uint32_t>               child_view_count{ 0 };
    int64_t                             strides[8]{ 0 };
    uint8_t                             is_strided_view{ 0 };
    uint8_t                             priority{ 5 };
    uint8_t                             group_priority{ 5 };
    uint8_t                             _pad0{ 0 };
    uint64_t                            type_sig_hash{ 0 };
    char                                debug_name[64]{ 0 };
};

hs_cold_object_metadata_t* hs_get_cold_metadata(const HS_custominiVm_live_header_t* obj, bool create_if_missing = true);
void                       hs_remove_cold_metadata(const HS_custominiVm_live_header_t* obj);
const char*                hs_get_object_debug_name(const HS_custominiVm_live_header_t* obj);
void                       hs_set_object_debug_name(const HS_custominiVm_live_header_t* obj, const char* name);

/* ========================================================================= */
/* 4. VIRTUAL PAGEFILE SWAP ENGINE (64KB BITMAP SLOTS + >4GB CHUNKED I/O)    */
/* ========================================================================= */

typedef enum HS_custominiVm_paging_policy_t {
    HS_PAGING_FIFO     = 0,
    HS_PAGING_LRU_COLD = 1,
    HS_PAGING_SIZE_MAX = 2
} HS_custominiVm_paging_policy_t;

struct hs_pagefile_t {
    static constexpr size_t PAGEFILE_BLOCK_BYTES = 65536; // 64 KB slot granularity

    HS_custominiVm_paging_policy_t policy{ HS_PAGING_FIFO };
    std::atomic<size_t>            paged_out_bytes{ 0 };
    std::atomic<size_t>            paged_out_count{ 0 };
    size_t                         capacity_bytes{ 256ULL * 1024 * 1024 };
    std::atomic<uint64_t>          write_offset{ 0 };
    std::vector<uint64_t>          free_bitmap; // 1 = occupied 64KB block, 0 = free 64KB block
    size_t                         total_blocks{ 0 };
    std::mutex                     lock;

#ifdef _WIN32
    HANDLE file_handle{ INVALID_HANDLE_VALUE };
#else
    int    file_fd{ -1 };
#endif
    void*  memory_fallback{ nullptr };

    hs_pagefile_t() = default;
    ~hs_pagefile_t() { destroy(); }

    bool allocate_slot_locked(size_t fp, uint64_t& out_offset);
    void release_slot_locked(uint64_t slot_off, size_t fp);

    bool init(size_t max_bytes, HS_custominiVm_paging_policy_t pol = HS_PAGING_FIFO);
    void destroy();

    bool page_out(HS_custominiVm_live_header_t* obj);
    bool page_in(HS_custominiVm_live_header_t* obj);
    bool prefetch_async(HS_custominiVm_live_header_t* obj);
};

extern hs_pagefile_t g_hs_pagefile;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API int    HS_custominiVm_pagefile_init(size_t max_bytes, HS_custominiVm_paging_policy_t policy);
MSIMD_API int    HS_custominiVm_page_out(HS_custominiVm_tensor_t* tensor);
MSIMD_API int    HS_custominiVm_page_in(HS_custominiVm_tensor_t* tensor);
MSIMD_API int    HS_custominiVm_page_prefetch(HS_custominiVm_tensor_t* tensor);
MSIMD_API size_t HS_custominiVm_pagefile_used_bytes(void);
MSIMD_API size_t HS_custominiVm_emergency_os_sweep(void);
MSIMD_API size_t HS_custominiVm_os_active_allocations(void);

#ifdef __cplusplus
}
#endif

using RedjetPagefile = hs_pagefile_t;

#endif // SHARE_GC_REDJET_REDJETPAGEFILE_HPP
