/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:5400-5850.
 */

#ifndef SHARE_GC_REDLET_REDLETRUNWAY_HPP
#define SHARE_GC_REDLET_REDLETRUNWAY_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletEmptyBox.hpp"

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

/* ========================================================================= */
/* 1. SLAB NODE & FREELIST SIZE CLASSES                                      */
/* ========================================================================= */

struct sf_slab_node_t {
    sf_slab_node_t* next = nullptr;
};

struct sf_slab_class_t {
    size_t              slot_size = 0;
    sf_slab_node_t*     free_list_head = nullptr;
    sf_slab_node_t*     free_list_tail = nullptr;
    std::atomic<size_t> free_count{ 0 };
    std::atomic<size_t> allocated_count{ 0 };
    std::mutex          lock;

    void* allocate();
    void  deallocate(void* ptr, uint32_t policy = 0);
};

/* ========================================================================= */
/* 2. TLAB BUFFER & RUNWAY ALLOCATION EPOCH                                  */
/* ========================================================================= */

struct sf_tlab_buffer_t {
    uint8_t* owner_runway_base = nullptr;
    uint32_t owner_epoch = 0;
    uint8_t* base = nullptr;
    size_t   offset = 0;
    size_t   capacity = 0;

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
/* 3. SLAB & BUMP ALLOCATOR WITH 32 SIZE CLASSES                             */
/* ========================================================================= */

struct sf_slab_allocator_t {
    static constexpr size_t NUM_CLASSES = 32;
    static constexpr size_t TLAB_REFILL_BYTES = 512 * 1024;
    static constexpr size_t TLAB_MAX_OBJ_SIZE = 65536;

    size_t class_sizes[NUM_CLASSES];
    sf_slab_class_t classes[NUM_CLASSES];

    size_t              slab_chunk_size_bytes{ 512 * 1024 };
    uint32_t            freelist_recycle_policy{ 0 };
    uint8_t*            runway_heap_base{ nullptr };
    size_t              runway_capacity{ 0 };
    std::atomic<size_t> runway_bump_offset{ 0 };
    std::mutex          bump_lock;
    bool                forbid_dynamic_expansion{ false };

    sf_slab_allocator_t();

    bool init(size_t handsel_bytes, bool advance_payment, bool forbid_expansion);
    void destroy();

    int find_size_class(size_t bytes) const;
    void* allocate(size_t cell_sz, size_t alignment = SACFOR_CACHE_LINE);
    void  deallocate(void* ptr, size_t cell_sz);
    size_t rebate_pages(uint32_t rebate_mode, size_t page_bytes);
};

/* ========================================================================= */
/* 4. REDLET RUNWAY ENGINE (sacculus_runway_t)                               */
/* ========================================================================= */

class RedletRunway {
private:
    sf_slab_allocator_t _allocator;
    std::atomic<size_t> _pacing_stalls{ 0 };
    std::atomic<size_t> _total_allocated_bytes{ 0 };

public:
    RedletRunway();
    ~RedletRunway();

    bool init(size_t initial_bytes, bool forbid_expansion = false);
    void destroy();

    void pace_allocation();
    void* bump_alloc(size_t cell_sz);
    void* allocate_cell(size_t body_bytes, uint32_t fn_id = 0, const char* name = nullptr);
    void  free_cell(void* body_ptr, size_t body_bytes);

    size_t used_bytes() const { return _allocator.runway_bump_offset.load(std::memory_order_relaxed); }
    size_t pacing_stalls() const { return _pacing_stalls.load(std::memory_order_relaxed); }
    sf_slab_allocator_t* allocator() { return &_allocator; }
};

using sacculus_runway_t = RedletRunway;

#endif // SHARE_GC_REDLET_REDLETRUNWAY_HPP
