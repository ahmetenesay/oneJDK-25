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

#ifndef SHARE_GC_REDLET_REDLETRUNWAY_HPP
#define SHARE_GC_REDLET_REDLETRUNWAY_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletEmptyBox.hpp"
#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletPitstop.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletIntentCLI.hpp"

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

/* ========================================================================= */
/* 1. SLAB NODE & FREELIST SIZE CLASSES                                      */
/* ========================================================================= */

struct sf_slab_node_t {
    sf_slab_node_t* next{ nullptr };
};

struct sf_slab_class_t {
    size_t              slot_size{ 0 };
    sf_slab_node_t*     free_list_head{ nullptr };
    sf_slab_node_t*     free_list_tail{ nullptr };
    std::atomic<size_t> free_count{ 0 };
    std::atomic<size_t> allocated_count{ 0 };
    std::mutex          lock;

    void  init(size_t sz) noexcept { slot_size = sz; }
    void* allocate_from_freelist();
    void* allocate() { return allocate_from_freelist(); }
    void  deallocate(void* ptr, uint32_t policy = 0);
};

/* ========================================================================= */
/* 2. TLAB BUFFER & RUNWAY ALLOCATION EPOCH                                  */
/* ========================================================================= */

struct sf_tlab_buffer_t {
    uint8_t* owner_runway_base{ nullptr };
    uint32_t owner_epoch{ 0 };
    uint8_t* base{ nullptr };
    size_t   offset{ 0 };
    size_t   capacity{ 0 };

    void reset() noexcept {
        owner_runway_base = nullptr;
        owner_epoch = 0;
        base = nullptr;
        offset = 0;
        capacity = 0;
    }
};

extern thread_local sf_tlab_buffer_t tl_sf_tlab;
extern std::atomic<uint32_t> g_sf_runway_epoch;

/* ========================================================================= */
/* 3. CHAINED OS CHUNK RECORD                                                */
/* ========================================================================= */

struct sf_chained_chunk_t {
    uint8_t*            base{ nullptr };
    size_t              capacity{ 0 };
    std::atomic<size_t> bump_offset{ 0 };
    sf_chained_chunk_t* next{ nullptr };
};

/* ========================================================================= */
/* 4. SLAB & BUMP ALLOCATOR WITH 32 SIZE CLASSES                             */
/* ========================================================================= */

struct sf_slab_allocator_t {
    static constexpr size_t NUM_CLASSES = 32;
    static constexpr size_t TLAB_REFILL_BYTES = 512 * 1024;
    static constexpr size_t TLAB_MAX_OBJ_SIZE = 65536;

    static const size_t CLASS_SIZES[NUM_CLASSES];
    size_t class_sizes[NUM_CLASSES];
    sf_slab_class_t classes[NUM_CLASSES];

    size_t                slab_chunk_size_bytes{ 512 * 1024 };
    uint32_t              freelist_recycle_policy{ 0 };
    uint8_t*              runway_heap_base{ nullptr };
    size_t                runway_capacity{ 0 };
    std::atomic<size_t>   runway_bump_offset{ 0 };
    sf_ticket_lock_t      bump_lock;
    bool                  Advance_Payment{ false };
    bool                  forbid_dynamic_expansion{ false };
    std::atomic<uint32_t> cache_color_step{ 0 };
    sf_heap_superblock_t* superblock{ nullptr };

    std::mutex            chain_lock;
    sf_chained_chunk_t*   chained_head{ nullptr };
    std::atomic<size_t>   chained_chunk_count{ 0 };

    sf_slab_allocator_t();

    bool init(size_t handsel_bytes = 1024ULL * 1024 * 1024, bool advance_payment = false,
              bool forbid_expansion = false, uint32_t freelist_policy = 0,
              size_t slab_chunk_sz = 512 * 1024);
    void destroy();

    int   find_size_class(size_t bytes) const;
    void* allocate(size_t bytes, size_t align = SACFOR_CACHE_LINE);
    void* allocate_from_chained_chunks(size_t alloc_sz, size_t align);
    void  deallocate(void* ptr, size_t bytes);
    size_t rebate_pages(sf_snatch_rebate_mode_e mode, size_t threshold_bytes = 64 * 1024);
    size_t get_used_bytes() const;
};

/* ========================================================================= */
/* 5. REDLET RUNWAY ENGINE (sacculus_runway_t)                               */
/* ========================================================================= */

class RedletRunway {
public:
    sacculus_motherline_t motherline;
    sacculus_pitstop_t    pitstop;
    sf_slab_allocator_t   allocator;
    std::atomic<size_t>   pacing_stalls{ 0 };
    std::atomic<size_t>   total_allocated_bytes{ 0 };

    RedletRunway();
    ~RedletRunway();

    bool init(size_t handsel_bytes = 1024ULL * 1024 * 1024, bool advance_payment = false,
              bool forbid_expansion = false, uint32_t freelist_policy = 0,
              size_t slab_chunk_sz = 512 * 1024);
    void destroy();

    void pace_allocation();
    void* bump_alloc(size_t cell_sz);
    void* allocate_cell(size_t body_bytes, uint32_t fn_id = 0, const char* name = nullptr);
    void  free_cell(void* body_ptr, size_t body_bytes);

    sacculus_pitstop_entry_t* move_to_pitstop(sfLiveHeader* obj);
    sfLiveHeader* return_from_pitstop(sacculus_pitstop_entry_t* entry);
    sfLiveHeader* return_from_pitstop(sfLiveHeader* obj);

    size_t batch_optimize_egg_objects(uint32_t fn_id, const char* new_isa_name, uint16_t new_simd_idx, uint64_t cycles_saved);
    size_t batch_optimize_egg_objects(uint32_t fn_id, uint16_t new_simd_idx, uint64_t cycles_saved) {
        return batch_optimize_egg_objects(fn_id, nullptr, new_simd_idx, cycles_saved);
    }

    size_t used_bytes() const noexcept { return allocator.get_used_bytes(); }
    size_t get_pacing_stalls() const noexcept { return pacing_stalls.load(std::memory_order_relaxed); }
};

using sacculus_runway_t = RedletRunway;

#endif // SHARE_GC_REDLET_REDLETRUNWAY_HPP
