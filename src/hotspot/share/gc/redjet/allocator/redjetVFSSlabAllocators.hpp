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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETVFSSLABALLOCATORS_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETVFSSLABALLOCATORS_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/allocator/redjetSlabAllocator.hpp"
#include "gc/redjet/vfs/redjetVFSNode.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. HOTFIELD SLAB ALLOCATOR (/heap/on_heap/runway/motherline/.../hotfield) */
/* ========================================================================= */

class RedjetHotfieldSlabAllocator {
public:
    static constexpr size_t NUM_HEIGHT_CLASSES = 8;
    static constexpr size_t HEIGHT_CLASS_SIZES[NUM_HEIGHT_CLASSES] = {
        32, 64, 128, 256, 512, 1024, 2048, 4096
    };

private:
    RedjetSlabClass       _height_classes[NUM_HEIGHT_CLASSES];
    uint8_t*              _arena_base{ nullptr };
    size_t                _arena_capacity{ 0 };
    std::atomic<size_t>   _bump_offset{ 0 };
    hs_ticket_lock_t      _bump_lock;
    RedjetVFSNode*        _vfs_node{ nullptr };
    std::atomic<uint64_t> _total_allocations{ 0 };
    std::atomic<uint64_t> _total_allocated_bytes{ 0 };

public:
    RedjetHotfieldSlabAllocator();
    ~RedjetHotfieldSlabAllocator();

    bool init(size_t capacity_bytes = 64ULL * 1024 * 1024, RedjetVFSNode* node = nullptr);
    void destroy();

    void* allocate(size_t bytes, size_t align = 64);
    void  deallocate(void* ptr, size_t bytes);

    int find_height_class(size_t bytes) const noexcept;
    size_t capacity() const noexcept { return _arena_capacity; }
    size_t used_bytes() const noexcept;
    uint64_t total_allocations() const noexcept { return _total_allocations.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 2. PUZZLE ARENA SLAB ALLOCATOR (/heap/on_heap/puzzle_arena)               */
/* ========================================================================= */

class RedjetPuzzleArenaSlabAllocator {
public:
    static constexpr size_t SCRATCH_SLOT_SIZE = 128 * 1024; // 128 KB standard scratch
    static constexpr size_t NUM_SCRATCH_SLOTS = 64;

private:
    struct ScratchSlot {
        uint8_t*            base{ nullptr };
        std::atomic<bool>   in_use{ false };
        uint32_t            owner_duel_id{ 0 };
    };

    uint8_t*              _arena_base{ nullptr };
    size_t                _arena_capacity{ 0 };
    ScratchSlot           _slots[NUM_SCRATCH_SLOTS];
    hs_ticket_lock_t      _arena_lock;
    RedjetVFSNode*        _vfs_node{ nullptr };
    std::atomic<uint64_t> _total_scratch_allocs{ 0 };

public:
    RedjetPuzzleArenaSlabAllocator();
    ~RedjetPuzzleArenaSlabAllocator();

    bool init(size_t capacity_bytes = 32ULL * 1024 * 1024, RedjetVFSNode* node = nullptr);
    void destroy();

    void* acquire_scratch_buffer(uint32_t duel_id, size_t min_bytes = SCRATCH_SLOT_SIZE);
    void  release_scratch_buffer(void* ptr);

    size_t capacity() const noexcept { return _arena_capacity; }
    size_t active_slots() const noexcept;
};

/* ========================================================================= */
/* 3. PARK ARENA SLAB ALLOCATOR (/heap/on_heap/park_arena)                   */
/* ========================================================================= */

class RedjetParkArenaSlabAllocator {
public:
    static constexpr size_t PAGE_CHUNK_SIZE = 4096;

private:
    struct FreeChunk {
        FreeChunk* next{ nullptr };
        size_t     size{ 0 };
    };

    uint8_t*              _arena_base{ nullptr };
    size_t                _arena_capacity{ 0 };
    std::atomic<size_t>   _bump_offset{ 0 };
    FreeChunk*            _freelist_head{ nullptr };
    hs_ticket_lock_t      _park_lock;
    RedjetVFSNode*        _vfs_node{ nullptr };
    std::atomic<uint32_t> _pinned_object_count{ 0 };
    std::atomic<size_t>   _pinned_bytes{ 0 };

public:
    RedjetParkArenaSlabAllocator();
    ~RedjetParkArenaSlabAllocator();

    bool init(size_t capacity_bytes = 128ULL * 1024 * 1024, RedjetVFSNode* node = nullptr);
    void destroy();

    void* allocate_pinned(size_t bytes, size_t align = 4096);
    void  deallocate_pinned(void* ptr, size_t bytes);

    size_t capacity() const noexcept { return _arena_capacity; }
    size_t used_bytes() const noexcept { return _pinned_bytes.load(std::memory_order_relaxed); }
    uint32_t pinned_count() const noexcept { return _pinned_object_count.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 4. FALLINE SLAB ALLOCATOR (/heap/on_heap/runway/falline)                  */
/* ========================================================================= */

class RedjetFallineSlabAllocator {
public:
    static constexpr size_t ENVELOPE_SIZE = 128; // Standard EmptyBox/Morgue envelope size

private:
    RedjetSlabClass       _envelope_class;
    uint8_t*              _arena_base{ nullptr };
    size_t                _arena_capacity{ 0 };
    std::atomic<size_t>   _bump_offset{ 0 };
    hs_ticket_lock_t      _bump_lock;
    RedjetVFSNode*        _vfs_node{ nullptr };
    std::atomic<uint64_t> _envelopes_allocated{ 0 };

public:
    RedjetFallineSlabAllocator();
    ~RedjetFallineSlabAllocator();

    bool init(size_t capacity_bytes = 16ULL * 1024 * 1024, RedjetVFSNode* node = nullptr);
    void destroy();

    void* allocate_envelope();
    void  recycle_envelope(void* ptr);

    size_t capacity() const noexcept { return _arena_capacity; }
    size_t used_bytes() const noexcept;
};

/* ========================================================================= */
/* 5. HEADER PANE SLAB ALLOCATOR (/heap/on_heap/landfill/header_pane)        */
/* ========================================================================= */

class RedjetHeaderPaneSlabAllocator {
public:
    static constexpr size_t HEADER_ENTRY_SIZE = 128; // Aligned live header size

private:
    RedjetSlabClass       _header_class;
    uint8_t*              _arena_base{ nullptr };
    size_t                _arena_capacity{ 0 };
    std::atomic<size_t>   _bump_offset{ 0 };
    hs_ticket_lock_t      _bump_lock;
    RedjetVFSNode*        _vfs_node{ nullptr };
    std::atomic<uint64_t> _headers_stored{ 0 };

public:
    RedjetHeaderPaneSlabAllocator();
    ~RedjetHeaderPaneSlabAllocator();

    bool init(size_t capacity_bytes = 32ULL * 1024 * 1024, RedjetVFSNode* node = nullptr);
    void destroy();

    void* allocate_header_slot();
    void  recycle_header_slot(void* ptr);

    size_t capacity() const noexcept { return _arena_capacity; }
    size_t used_bytes() const noexcept;
    uint64_t headers_stored() const noexcept { return _headers_stored.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETVFSSLABALLOCATORS_HPP
