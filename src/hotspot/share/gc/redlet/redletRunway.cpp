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

#include "gc/redlet/redletRunway.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include <cstdlib>
#include <cstring>
#include <new>

thread_local sf_tlab_buffer_t tl_sf_tlab{};
std::atomic<uint32_t> g_sf_runway_epoch{ 1 };

const size_t sf_slab_allocator_t::CLASS_SIZES[NUM_CLASSES] = {
    64, 128, 192, 256, 320, 384, 448, 512,
    768, 1024, 1280, 1536, 1792, 2048, 2304, 2560,
    4096, 6144, 8192, 10240, 12288, 14336, 16384, 18432,
    32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304
};

/* ========================================================================= */
/* 1. SLAB CLASS ALLOCATION & DEALLOCATION                                   */
/* ========================================================================= */

void* sf_slab_class_t::allocate_from_freelist() {
    std::lock_guard<std::mutex> g(lock);
    if (!free_list_head) return nullptr;

    sf_slab_node_t* node = free_list_head;
    free_list_head = free_list_head->next;
    if (!free_list_head) {
        free_list_tail = nullptr;
    }
    free_count.fetch_sub(1, std::memory_order_relaxed);
    allocated_count.fetch_add(1, std::memory_order_relaxed);
    return static_cast<void*>(node);
}

void sf_slab_class_t::deallocate(void* ptr, uint32_t policy) {
    if (!ptr) return;
    std::lock_guard<std::mutex> g(lock);
    auto* node = static_cast<sf_slab_node_t*>(ptr);
    node->next = nullptr;

    if (policy == 1) { // FIFO
        if (!free_list_head) {
            free_list_head = node;
            free_list_tail = node;
        } else {
            free_list_tail->next = node;
            free_list_tail = node;
        }
    } else { // LIFO (Default)
        node->next = free_list_head;
        free_list_head = node;
        if (!free_list_tail) {
            free_list_tail = node;
        }
    }
    free_count.fetch_add(1, std::memory_order_relaxed);
    allocated_count.fetch_sub(1, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. SLAB ALLOCATOR (32 SIZE CLASSES & CHAINED CHUNKS)                      */
/* ========================================================================= */

sf_slab_allocator_t::sf_slab_allocator_t() {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        class_sizes[i] = CLASS_SIZES[i];
        classes[i].init(CLASS_SIZES[i]);
    }
}

bool sf_slab_allocator_t::init(size_t handsel_bytes, bool advance_payment,
                               bool forbid_expansion, uint32_t freelist_policy,
                               size_t slab_chunk_sz) {
    runway_capacity = handsel_bytes;
    Advance_Payment = advance_payment;
    forbid_dynamic_expansion = forbid_expansion;
    freelist_recycle_policy = freelist_policy;
    slab_chunk_size_bytes = (slab_chunk_sz >= 64 * 1024) ? slab_chunk_sz : (512 * 1024);

    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        classes[i].init(CLASS_SIZES[i]);
    }

    while (runway_capacity >= 64ULL * 1024 * 1024) {
        runway_heap_base = (uint8_t*)sf_os_reserve_memory(runway_capacity);
        if (runway_heap_base) {
            if (sf_os_commit_memory(runway_heap_base, runway_capacity)) {
                break;
            }
            sf_os_release_memory(runway_heap_base, runway_capacity);
            runway_heap_base = nullptr;
        }
        runway_capacity /= 2;
    }
    if (!runway_heap_base) return false;

    superblock = reinterpret_cast<sf_heap_superblock_t*>(runway_heap_base);
    new (superblock) sf_heap_superblock_t();
    superblock->magic = SF_HEAP_MAGIC;
    superblock->version = 1;
    superblock->total_reserved_bytes = runway_capacity;
    superblock->total_committed_bytes = runway_capacity;
    superblock->page_size = 4096;
    superblock->alignment = SACFOR_CACHE_LINE;

    size_t header_sz = sizeof(sf_heap_superblock_t);
    header_sz = (header_sz + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);

    superblock->slabs_offset = header_sz;
    superblock->slabs_capacity = runway_capacity / 4;
    superblock->bump_arena_offset = header_sz;
    superblock->bump_arena_capacity = runway_capacity * 3 / 4;
    superblock->los_offset = runway_capacity * 3 / 4;
    superblock->los_capacity = runway_capacity / 4;

    runway_bump_offset.store(header_sz, std::memory_order_relaxed);
    g_sf_runway_epoch.fetch_add(1, std::memory_order_release);
    tl_sf_tlab.reset();

    if (Advance_Payment) {
        sf_pretouch_memory(runway_heap_base, runway_capacity);
    }
    return true;
}

void sf_slab_allocator_t::destroy() {
    g_sf_runway_epoch.fetch_add(1, std::memory_order_release);
    tl_sf_tlab.reset();

    {
        std::lock_guard<std::mutex> g(chain_lock);
        sf_chained_chunk_t* ch = chained_head;
        while (ch) {
            sf_chained_chunk_t* nxt = ch->next;
            if (ch->base) sf_os_release_memory(ch->base, ch->capacity);
            delete ch;
            ch = nxt;
        }
        chained_head = nullptr;
        chained_chunk_count.store(0, std::memory_order_relaxed);
    }

    if (runway_heap_base) {
        sf_os_release_memory(runway_heap_base, runway_capacity);
        runway_heap_base = nullptr;
        superblock = nullptr;
    }
    runway_capacity = 0;
    runway_bump_offset.store(0, std::memory_order_relaxed);
}

int sf_slab_allocator_t::find_size_class(size_t bytes) const {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        if (bytes <= CLASS_SIZES[i]) return static_cast<int>(i);
    }
    return -1;
}

void* sf_slab_allocator_t::allocate_from_chained_chunks(size_t alloc_sz, size_t align) {
    if (forbid_dynamic_expansion) {
        return nullptr;
    }

    std::lock_guard<std::mutex> g(chain_lock);
    sf_chained_chunk_t* cur = chained_head;
    while (cur) {
        size_t off = cur->bump_offset.load(std::memory_order_relaxed);
        size_t aligned_off = (off + align - 1) & ~(align - 1);
        if (aligned_off + alloc_sz <= cur->capacity) {
            cur->bump_offset.store(aligned_off + alloc_sz, std::memory_order_relaxed);
            if (superblock) superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
            return cur->base + aligned_off;
        }
        cur = cur->next;
    }

    size_t chunk_cap = alloc_sz * 2;
    size_t min_cap = (slab_chunk_size_bytes >= 64 * 1024) ? slab_chunk_size_bytes : (64ULL * 1024 * 1024);
    if (chunk_cap < min_cap) chunk_cap = min_cap;
    chunk_cap = (chunk_cap + 4095ULL) & ~4095ULL;

    uint8_t* new_base = static_cast<uint8_t*>(sf_os_reserve_memory(chunk_cap));
    if (!new_base) return nullptr;
    if (!sf_os_commit_memory(new_base, chunk_cap)) {
        sf_os_release_memory(new_base, chunk_cap);
        return nullptr;
    }
    if (Advance_Payment) {
        sf_pretouch_memory(new_base, chunk_cap);
    }

    auto* chunk = new (std::nothrow) sf_chained_chunk_t();
    if (!chunk) {
        sf_os_release_memory(new_base, chunk_cap);
        return nullptr;
    }
    chunk->base = new_base;
    chunk->capacity = chunk_cap;
    chunk->bump_offset.store(alloc_sz, std::memory_order_relaxed);
    chunk->next = chained_head;
    chained_head = chunk;
    chained_chunk_count.fetch_add(1, std::memory_order_relaxed);

    if (superblock) {
        superblock->total_reserved_bytes += chunk_cap;
        superblock->total_committed_bytes += chunk_cap;
        superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
        superblock->next_chained_block = new_base;
    }

    sf_hotlog_emit(SF_Ebin0_ALLOC_CHUNK_CHAIN, SF_Ebin1_INFO, 0,
                   static_cast<uint16_t>(chained_chunk_count.load(std::memory_order_relaxed)),
                   0, 0, 0, 0, static_cast<uint32_t>(chunk_cap / 1024), "os_chunk_chained");
    return new_base;
}

void* sf_slab_allocator_t::allocate(size_t bytes, size_t align) {
    if (bytes == 0) return nullptr;

    int sc = find_size_class(bytes);
    if (sc >= 0) {
        void* recycled = classes[sc].allocate_from_freelist();
        if (recycled) {
            if (superblock) superblock->allocated_bytes.fetch_add(CLASS_SIZES[sc], std::memory_order_relaxed);
            sf_hotlog_emit(SF_Ebin0_ALLOC_FREELIST, SF_Ebin1_TRACE, 0, static_cast<uint16_t>(sc),
                           0, 0, 0, 0, static_cast<uint32_t>(CLASS_SIZES[sc] / 1024), "slab_freelist_hit");
            return recycled;
        }
        bytes = CLASS_SIZES[sc];
    }

    size_t alloc_sz = (bytes + align - 1) & ~(align - 1);

    uint32_t cur_epoch = g_sf_runway_epoch.load(std::memory_order_acquire);
    if (!forbid_dynamic_expansion && alloc_sz <= TLAB_MAX_OBJ_SIZE && runway_heap_base) {
        if (tl_sf_tlab.owner_runway_base == runway_heap_base &&
            tl_sf_tlab.owner_epoch == cur_epoch &&
            tl_sf_tlab.base != nullptr) {
            size_t t_off = (tl_sf_tlab.offset + align - 1) & ~(align - 1);
            if (t_off + alloc_sz <= tl_sf_tlab.capacity) {
                tl_sf_tlab.offset = t_off + alloc_sz;
                if (superblock) superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
                return tl_sf_tlab.base + t_off;
            }
        }

        sf_ticket_guard_t guard(bump_lock);
        size_t cur = runway_bump_offset.load(std::memory_order_relaxed);
        size_t aligned_cur = (cur + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        size_t refill_sz = (slab_chunk_size_bytes >= 64 * 1024) ? slab_chunk_size_bytes : TLAB_REFILL_BYTES;
        if (aligned_cur + refill_sz <= runway_capacity) {
            sf_os_commit_memory(runway_heap_base + aligned_cur, refill_sz);
            runway_bump_offset.store(aligned_cur + refill_sz, std::memory_order_relaxed);
            tl_sf_tlab.owner_runway_base = runway_heap_base;
            tl_sf_tlab.owner_epoch = cur_epoch;
            tl_sf_tlab.base = runway_heap_base + aligned_cur;
            tl_sf_tlab.capacity = refill_sz;
            tl_sf_tlab.offset = alloc_sz;
            if (superblock) superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
            return tl_sf_tlab.base;
        }
    }

    sf_ticket_guard_t guard(bump_lock);
    size_t color_pad = (cache_color_step.fetch_add(1, std::memory_order_relaxed) & 7) * SACFOR_CACHE_LINE;
    size_t cur = runway_bump_offset.load(std::memory_order_relaxed) + color_pad;
    size_t aligned_cur = (cur + align - 1) & ~(align - 1);

    if (aligned_cur + alloc_sz > runway_capacity) {
        if (forbid_dynamic_expansion) {
            return nullptr;
        }
        return allocate_from_chained_chunks(alloc_sz, align);
    }

    sf_os_commit_memory(runway_heap_base + aligned_cur, alloc_sz);
    runway_bump_offset.store(aligned_cur + alloc_sz, std::memory_order_relaxed);
    if (superblock) superblock->allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
    return runway_heap_base + aligned_cur;
}

void sf_slab_allocator_t::deallocate(void* ptr, size_t bytes) {
    if (!ptr || !runway_heap_base) return;
    int sc = find_size_class(bytes);
    if (sc >= 0) {
        classes[sc].deallocate(ptr, freelist_recycle_policy);
        if (superblock) superblock->allocated_bytes.fetch_sub(CLASS_SIZES[sc], std::memory_order_relaxed);
    } else {
        if (superblock) superblock->allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

size_t sf_slab_allocator_t::rebate_pages(sf_snatch_rebate_mode_e mode, size_t threshold_bytes) {
    if (forbid_dynamic_expansion || mode == sf_snatch_rebate_mode_e::LAZY || !runway_heap_base) {
        return 0;
    }

    sf_ticket_guard_t guard(bump_lock);
    size_t cur_used = runway_bump_offset.load(std::memory_order_relaxed);
    size_t page_start = (cur_used + 4095ULL) & ~4095ULL;
    if (runway_capacity > page_start) {
        size_t unused = runway_capacity - page_start;
        if (unused >= threshold_bytes) {
            size_t page_aligned = unused & ~4095ULL;
            if (page_aligned > 0) {
                uint8_t* target = runway_heap_base + page_start;
                sf_os_decommit_memory(target, page_aligned);
                if (superblock) {
                    superblock->rebated_bytes.fetch_add(page_aligned, std::memory_order_relaxed);
                    superblock->rebate_cycle_count.fetch_add(1, std::memory_order_relaxed);
                }
                sf_hotlog_emit(SF_Ebin0_ALLOC_REBATE, SF_Ebin1_INFO, 0,
                               static_cast<uint16_t>(mode), 0, 0, 0, 0,
                               static_cast<uint32_t>(page_aligned / 1024), "runway_pages_rebated");
                return page_aligned;
            }
        }
    }
    return 0;
}

size_t sf_slab_allocator_t::get_used_bytes() const {
    size_t off = runway_bump_offset.load(std::memory_order_relaxed);
    size_t hdr = sizeof(sf_heap_superblock_t);
    hdr = (hdr + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    return (off > hdr) ? (off - hdr) : 0;
}

/* ========================================================================= */
/* 3. REDLET RUNWAY ENGINE IMPLEMENTATION                                    */
/* ========================================================================= */

RedletRunway::RedletRunway() = default;

RedletRunway::~RedletRunway() {
    destroy();
}

bool RedletRunway::init(size_t handsel_bytes, bool advance_payment,
                        bool forbid_expansion, uint32_t freelist_policy,
                        size_t slab_chunk_sz) {
    motherline.init();
    return allocator.init(handsel_bytes, advance_payment, forbid_expansion, freelist_policy, slab_chunk_sz);
}

void RedletRunway::destroy() {
    pitstop.destroy();
    motherline.destroy();
    allocator.destroy();
}

void RedletRunway::pace_allocation() {
    if (allocator.runway_bump_offset.load(std::memory_order_relaxed) > (allocator.runway_capacity * 9) / 10) {
        pacing_stalls.fetch_add(1, std::memory_order_relaxed);
        sf_hotlog_emit(SF_Ebin0_ALLOC_PACE_STALL, SF_Ebin1_WARN, 0, 90, 0, 0, 0, 0, 0, "runway_pacing_stall");
    }
}

void* RedletRunway::bump_alloc(size_t cell_sz) {
    pace_allocation();
    void* ptr = allocator.allocate(cell_sz, SACFOR_CACHE_LINE);
    if (ptr) {
        total_allocated_bytes.fetch_add(cell_sz, std::memory_order_relaxed);
    }
    return ptr;
}

void* RedletRunway::allocate_cell(size_t body_bytes, uint32_t fn_id, const char* name) {
    const size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    const size_t aligned_body = (body_bytes + 7ULL) & ~7ULL;
    const size_t cell_sz = (hdr_sz + aligned_body + sizeof(uint64_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);

    void* raw = bump_alloc(cell_sz);
    if (!raw) return nullptr;

    std::memset(raw, 0, hdr_sz);
    auto* hdr = new (raw) SF_custominiVm_live_header_t();
    hdr->init(fn_id, 0, name);
    hdr->memory_footprint.store(body_bytes, std::memory_order_relaxed);
    hdr->total_cell_bytes = cell_sz;
    hdr->state.store(static_cast<uint8_t>(sf_live_state_e::LIVE), std::memory_order_release);
    if (body_bytes > 0) {
        hdr->writeTailCanary();
    }
    motherline.admit_egg_object(hdr);
    return hdr->body();
}

void RedletRunway::free_cell(void* body_ptr, size_t body_bytes) {
    if (!body_ptr) return;
    auto* hdr = SF_custominiVm_live_header_t::from_body(body_ptr);
    if (!hdr) return;

    size_t cell_sz = hdr->total_cell_bytes;
    if (cell_sz == 0) {
        const size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        const size_t aligned_body = (body_bytes + 7ULL) & ~7ULL;
        cell_sz = (hdr_sz + aligned_body + sizeof(uint64_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    }
    motherline.evict(hdr);
    hdr->sfMN = 0;
    hdr->state.store(static_cast<uint8_t>(sf_live_state_e::DEAD), std::memory_order_release);
    hdr->~SF_custominiVm_live_header_t();
    allocator.deallocate(static_cast<void*>(hdr), cell_sz);
}

sacculus_pitstop_entry_t* RedletRunway::move_to_pitstop(sfLiveHeader* obj) {
    if (!obj) return nullptr;
    uint8_t mode = g_sf_puzzler_morph_method.load(std::memory_order_relaxed);
    if (mode == static_cast<uint8_t>(sf_puzzler_morph_e::EXCISION_EXCLUSIVE)) {
        motherline.evict(obj);
        sf_hotlog_emit(SF_Ebin0_PITSTOP_EXCISE, SF_Ebin1_INFO, obj->function_id, 0,
                       obj->generation, obj->region, 0, 0, 0, "pitstop_excise");
        return pitstop.admit_excision(obj);
    } else {
        sf_hotlog_emit(SF_Ebin0_PITSTOP_SHADOW, SF_Ebin1_INFO, obj->function_id, 0,
                       obj->generation, obj->region, 0, 0, 0, "pitstop_shadow_rcu");
        return pitstop.admit_shadow(obj);
    }
}

sfLiveHeader* RedletRunway::return_from_pitstop(sacculus_pitstop_entry_t* entry) {
    if (!entry) return nullptr;
    sfLiveHeader* restored = nullptr;
    uint32_t fn_id = entry->saved_header.function_id;
    uint16_t active_idx = entry->saved_header.active_simd_idx;
    uint64_t cycles = entry->saved_header.cycles_gained.load(std::memory_order_relaxed);
    const char* isa_name = entry->saved_header.simd_table[active_idx].name;
    bool was_excision = (entry->mode == sf_puzzler_morph_e::EXCISION_EXCLUSIVE);

    if (pitstop.restore_return(entry, &restored) && restored) {
        if (was_excision) {
            motherline.admit(restored);
        } else {
            motherline.promote_to_fledgling(restored);
        }
        sf_hotlog_emit(SF_Ebin0_PITSTOP_RETURN, SF_Ebin1_INFO, fn_id, 0,
                       restored->generation, restored->region, 0, 0, static_cast<uint32_t>(cycles), isa_name);
        batch_optimize_egg_objects(fn_id, isa_name, active_idx, cycles);
        return restored;
    }
    return nullptr;
}

sfLiveHeader* RedletRunway::return_from_pitstop(sfLiveHeader* obj) {
    if (!obj) return nullptr;
    auto* entry = pitstop.find_by_slot(static_cast<void*>(obj));
    if (entry) return return_from_pitstop(entry);
    return obj;
}

size_t RedletRunway::batch_optimize_egg_objects(uint32_t fn_id, const char* new_isa_name,
                                                uint16_t new_simd_idx, uint64_t cycles_saved) {
    return motherline.batch_optimize_egg_objects(fn_id, new_isa_name, new_simd_idx, cycles_saved);
}
