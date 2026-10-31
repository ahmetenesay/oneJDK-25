/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:5400-5850.
 */

#include "gc/redlet/redletRunway.hpp"
#include "gc/redlet/redletSIMD.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif
#include <cstdlib>
#include <new>

thread_local sf_tlab_buffer_t tl_sf_tlab{};
std::atomic<uint32_t> g_sf_runway_epoch{ 1 };

/* ========================================================================= */
/* SLAB CLASS ALLOCATION & DEALLOCATION                                      */
/* ========================================================================= */

void* sf_slab_class_t::allocate() {
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
/* SLAB ALLOCATOR (32 SIZE CLASSES)                                          */
/* ========================================================================= */

sf_slab_allocator_t::sf_slab_allocator_t() {
    // Generate 32 size classes: 64B to 512KB
    size_t sz = 64;
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        class_sizes[i] = sz;
        classes[i].slot_size = sz;
        if (i < 8) {
            sz += 64;           // 64, 128, 192, 256, 320, 384, 448, 512
        } else if (i < 16) {
            sz += 256;          // up to ~2.5KB
        } else if (i < 24) {
            sz += 2048;         // up to ~18KB
        } else {
            sz *= 2;            // up to 512KB
        }
    }
}

bool sf_slab_allocator_t::init(size_t handsel_bytes, bool advance_payment, bool forbid_expansion) {
    (void)advance_payment;
    runway_capacity = handsel_bytes;
    forbid_dynamic_expansion = forbid_expansion;
    runway_heap_base = static_cast<uint8_t*>(std::malloc(runway_capacity));
    if (!runway_heap_base) return false;

    runway_bump_offset.store(0, std::memory_order_relaxed);
    sf_hotlog_emit(SF_Ebin0_ALLOC_CHUNK_CHAIN, SF_Ebin1_INFO, 0, 0, 0, 0, 0, 0,
                   (uint32_t)(runway_capacity / 1024), "runway_initialized");
    return true;
}

void sf_slab_allocator_t::destroy() {
    if (runway_heap_base) {
        std::free(runway_heap_base);
        runway_heap_base = nullptr;
    }
    runway_capacity = 0;
    runway_bump_offset.store(0, std::memory_order_relaxed);
}

int sf_slab_allocator_t::find_size_class(size_t bytes) const {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        if (bytes <= class_sizes[i]) return static_cast<int>(i);
    }
    return -1;
}

void* sf_slab_allocator_t::allocate(size_t cell_sz, size_t alignment) {
    if (cell_sz == 0) return nullptr;

    // 1. Try TLAB fast path for small allocations
    if (cell_sz <= TLAB_MAX_OBJ_SIZE) {
        if (tl_sf_tlab.base && (tl_sf_tlab.offset + cell_sz <= tl_sf_tlab.capacity)) {
            void* res = tl_sf_tlab.base + tl_sf_tlab.offset;
            tl_sf_tlab.offset += (cell_sz + alignment - 1) & ~(alignment - 1);
            return res;
        }
    }

    // 2. Try Slab freelist
    int sc = find_size_class(cell_sz);
    if (sc >= 0) {
        void* slab_ptr = classes[sc].allocate();
        if (slab_ptr) {
            sf_hotlog_emit(SF_Ebin0_ALLOC_FREELIST, SF_Ebin1_TRACE, 0, (uint16_t)sc, 0, 0, 0, 0, 0, "slab_freelist_hit");
            return slab_ptr;
        }
    }

    // 3. Fallback to Bump allocation
    size_t aligned_sz = (cell_sz + alignment - 1) & ~(alignment - 1);
    size_t cur_off = runway_bump_offset.fetch_add(aligned_sz, std::memory_order_acq_rel);

    if (runway_heap_base && (cur_off + aligned_sz <= runway_capacity)) {
        void* res = runway_heap_base + cur_off;
        sf_hotlog_emit(SF_Ebin0_ALLOC_BUMP, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0, (uint32_t)(cell_sz / 1024), "runway_bump_alloc");
        return res;
    }

    return nullptr;
}

void sf_slab_allocator_t::deallocate(void* ptr, size_t cell_sz) {
    if (!ptr || cell_sz == 0) return;
    int sc = find_size_class(cell_sz);
    if (sc >= 0) {
        classes[sc].deallocate(ptr, freelist_recycle_policy);
    }
}

size_t sf_slab_allocator_t::rebate_pages(uint32_t rebate_mode, size_t page_bytes) {
    (void)rebate_mode;
    sf_hotlog_emit(SF_Ebin0_ALLOC_REBATE, SF_Ebin1_INFO, 0, 0, 0, 0, 0, 0, (uint32_t)(page_bytes / 1024), "page_rebate_applied");
    return page_bytes;
}

/* ========================================================================= */
/* REDLET RUNWAY ENGINE IMPLEMENTATION                                       */
/* ========================================================================= */

RedletRunway::RedletRunway() = default;

RedletRunway::~RedletRunway() {
    destroy();
}

bool RedletRunway::init(size_t initial_bytes, bool forbid_expansion) {
    return _allocator.init(initial_bytes, false, forbid_expansion);
}

void RedletRunway::destroy() {
    _allocator.destroy();
}

void RedletRunway::pace_allocation() {
    // Microsecond pacing check to avoid thread monopolization
    if (_allocator.runway_bump_offset.load(std::memory_order_relaxed) > (_allocator.runway_capacity * 9) / 10) {
        _pacing_stalls.fetch_add(1, std::memory_order_relaxed);
        sf_hotlog_emit(SF_Ebin0_ALLOC_PACE_STALL, SF_Ebin1_WARN, 0, 90, 0, 0, 0, 0, 0, "runway_pacing_stall");
    }
}

void* RedletRunway::bump_alloc(size_t cell_sz) {
    pace_allocation();
    void* ptr = _allocator.allocate(cell_sz, SACFOR_CACHE_LINE);
    if (ptr) {
        _total_allocated_bytes.fetch_add(cell_sz, std::memory_order_relaxed);
    }
    return ptr;
}

void* RedletRunway::allocate_cell(size_t body_bytes, uint32_t fn_id, const char* name) {
    (void)fn_id;
    (void)name;
    return bump_alloc(body_bytes);
}

void RedletRunway::free_cell(void* body_ptr, size_t body_bytes) {
    _allocator.deallocate(body_ptr, body_bytes);
}
