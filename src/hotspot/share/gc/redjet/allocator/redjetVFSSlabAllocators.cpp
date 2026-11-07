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

#include "gc/redjet/allocator/redjetVFSSlabAllocators.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <new>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

static void* vfs_os_reserve_commit(size_t bytes) {
    if (bytes == 0) return nullptr;
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

static void vfs_os_release(void* ptr, size_t bytes) {
    if (!ptr) return;
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, bytes);
#endif
}

} // namespace

/* ========================================================================= */
/* 1. HOTFIELD SLAB ALLOCATOR                                                */
/* ========================================================================= */

RedjetHotfieldSlabAllocator::RedjetHotfieldSlabAllocator() {
    for (size_t i = 0; i < NUM_HEIGHT_CLASSES; ++i) {
        _height_classes[i].init(HEIGHT_CLASS_SIZES[i]);
    }
}

RedjetHotfieldSlabAllocator::~RedjetHotfieldSlabAllocator() {
    destroy();
}

bool RedjetHotfieldSlabAllocator::init(size_t capacity_bytes, RedjetVFSNode* node) {
    _arena_capacity = capacity_bytes;
    _vfs_node = node;
    _arena_base = static_cast<uint8_t*>(vfs_os_reserve_commit(_arena_capacity));
    if (!_arena_base) return false;

    _bump_offset.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < NUM_HEIGHT_CLASSES; ++i) {
        _height_classes[i].init(HEIGHT_CLASS_SIZES[i]);
    }
    return true;
}

void RedjetHotfieldSlabAllocator::destroy() {
    if (_arena_base) {
        vfs_os_release(_arena_base, _arena_capacity);
        _arena_base = nullptr;
    }
    _arena_capacity = 0;
    _bump_offset.store(0, std::memory_order_relaxed);
}

int RedjetHotfieldSlabAllocator::find_height_class(size_t bytes) const noexcept {
    for (size_t i = 0; i < NUM_HEIGHT_CLASSES; ++i) {
        if (bytes <= HEIGHT_CLASS_SIZES[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void* RedjetHotfieldSlabAllocator::allocate(size_t bytes, size_t align) {
    int cls = find_height_class(bytes);
    void* result = nullptr;

    if (cls >= 0) {
        result = _height_classes[cls].allocate_from_freelist();
        if (result != nullptr) {
            _total_allocations.fetch_add(1, std::memory_order_relaxed);
            _total_allocated_bytes.fetch_add(HEIGHT_CLASS_SIZES[cls], std::memory_order_relaxed);
            if (_vfs_node) _vfs_node->add_used(HEIGHT_CLASS_SIZES[cls]);
            return result;
        }
    }

    // Bump allocation fallback from continuous hotfield arena
    size_t alloc_sz = (cls >= 0) ? HEIGHT_CLASS_SIZES[cls] : bytes;
    hs_ticket_guard_t guard(_bump_lock);
    size_t cur = _bump_offset.load(std::memory_order_relaxed);
    size_t aligned_cur = (cur + align - 1) & ~(align - 1);

    if (aligned_cur + alloc_sz <= _arena_capacity) {
        _bump_offset.store(aligned_cur + alloc_sz, std::memory_order_relaxed);
        result = _arena_base + aligned_cur;
        _total_allocations.fetch_add(1, std::memory_order_relaxed);
        _total_allocated_bytes.fetch_add(alloc_sz, std::memory_order_relaxed);
        if (_vfs_node) _vfs_node->add_used(alloc_sz);
        return result;
    }

    return nullptr;
}

void RedjetHotfieldSlabAllocator::deallocate(void* ptr, size_t bytes) {
    if (!ptr) return;
    int cls = find_height_class(bytes);
    if (cls >= 0) {
        _height_classes[cls].deallocate(ptr, RedjetSlabClass::LIFO);
        if (_vfs_node) _vfs_node->sub_used(HEIGHT_CLASS_SIZES[cls]);
    }
}

size_t RedjetHotfieldSlabAllocator::used_bytes() const noexcept {
    return _bump_offset.load(std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. PUZZLE ARENA SLAB ALLOCATOR                                            */
/* ========================================================================= */

RedjetPuzzleArenaSlabAllocator::RedjetPuzzleArenaSlabAllocator() = default;

RedjetPuzzleArenaSlabAllocator::~RedjetPuzzleArenaSlabAllocator() {
    destroy();
}

bool RedjetPuzzleArenaSlabAllocator::init(size_t capacity_bytes, RedjetVFSNode* node) {
    _arena_capacity = capacity_bytes;
    _vfs_node = node;
    _arena_base = static_cast<uint8_t*>(vfs_os_reserve_commit(_arena_capacity));
    if (!_arena_base) return false;

    for (size_t i = 0; i < NUM_SCRATCH_SLOTS; ++i) {
        _slots[i].base = _arena_base + (i * SCRATCH_SLOT_SIZE);
        _slots[i].in_use.store(false, std::memory_order_relaxed);
        _slots[i].owner_duel_id = 0;
    }
    return true;
}

void RedjetPuzzleArenaSlabAllocator::destroy() {
    if (_arena_base) {
        vfs_os_release(_arena_base, _arena_capacity);
        _arena_base = nullptr;
    }
    _arena_capacity = 0;
}

void* RedjetPuzzleArenaSlabAllocator::acquire_scratch_buffer(uint32_t duel_id, size_t min_bytes) {
    if (min_bytes > SCRATCH_SLOT_SIZE) return nullptr;

    hs_ticket_guard_t guard(_arena_lock);
    for (size_t i = 0; i < NUM_SCRATCH_SLOTS; ++i) {
        bool expected = false;
        if (_slots[i].in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            _slots[i].owner_duel_id = duel_id;
            _total_scratch_allocs.fetch_add(1, std::memory_order_relaxed);
            if (_vfs_node) _vfs_node->add_used(SCRATCH_SLOT_SIZE);
            return _slots[i].base;
        }
    }
    return nullptr;
}

void RedjetPuzzleArenaSlabAllocator::release_scratch_buffer(void* ptr) {
    if (!ptr || !_arena_base) return;

    uintptr_t base_addr = reinterpret_cast<uintptr_t>(_arena_base);
    uintptr_t target_addr = reinterpret_cast<uintptr_t>(ptr);
    if (target_addr < base_addr || target_addr >= (base_addr + _arena_capacity)) return;

    size_t idx = (target_addr - base_addr) / SCRATCH_SLOT_SIZE;
    if (idx < NUM_SCRATCH_SLOTS) {
        _slots[idx].owner_duel_id = 0;
        _slots[idx].in_use.store(false, std::memory_order_release);
        if (_vfs_node) _vfs_node->sub_used(SCRATCH_SLOT_SIZE);
    }
}

size_t RedjetPuzzleArenaSlabAllocator::active_slots() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < NUM_SCRATCH_SLOTS; ++i) {
        if (_slots[i].in_use.load(std::memory_order_relaxed)) {
            count++;
        }
    }
    return count;
}

/* ========================================================================= */
/* 3. PARK ARENA SLAB ALLOCATOR                                              */
/* ========================================================================= */

RedjetParkArenaSlabAllocator::RedjetParkArenaSlabAllocator() = default;

RedjetParkArenaSlabAllocator::~RedjetParkArenaSlabAllocator() {
    destroy();
}

bool RedjetParkArenaSlabAllocator::init(size_t capacity_bytes, RedjetVFSNode* node) {
    _arena_capacity = capacity_bytes;
    _vfs_node = node;
    _arena_base = static_cast<uint8_t*>(vfs_os_reserve_commit(_arena_capacity));
    if (!_arena_base) return false;

    _bump_offset.store(0, std::memory_order_relaxed);
    _freelist_head = nullptr;
    _pinned_object_count.store(0, std::memory_order_relaxed);
    _pinned_bytes.store(0, std::memory_order_relaxed);
    return true;
}

void RedjetParkArenaSlabAllocator::destroy() {
    if (_arena_base) {
        vfs_os_release(_arena_base, _arena_capacity);
        _arena_base = nullptr;
    }
    _freelist_head = nullptr;
    _arena_capacity = 0;
}

void* RedjetParkArenaSlabAllocator::allocate_pinned(size_t bytes, size_t align) {
    if (align < 4096) align = 4096;
    size_t aligned_bytes = (bytes + 4095) & ~4095;

    hs_ticket_guard_t guard(_park_lock);

    // Check freelist for recycled chunk first
    FreeChunk** prev = &_freelist_head;
    FreeChunk* curr = _freelist_head;
    while (curr != nullptr) {
        if (curr->size >= aligned_bytes) {
            *prev = curr->next;
            _pinned_object_count.fetch_add(1, std::memory_order_relaxed);
            _pinned_bytes.fetch_add(aligned_bytes, std::memory_order_relaxed);
            if (_vfs_node) _vfs_node->add_used(aligned_bytes);
            return reinterpret_cast<void*>(curr);
        }
        prev = &curr->next;
        curr = curr->next;
    }

    // Bump allocate if no suitable freelist chunk found
    size_t cur = _bump_offset.load(std::memory_order_relaxed);
    size_t aligned_cur = (cur + align - 1) & ~(align - 1);

    if (aligned_cur + aligned_bytes <= _arena_capacity) {
        _bump_offset.store(aligned_cur + aligned_bytes, std::memory_order_relaxed);
        _pinned_object_count.fetch_add(1, std::memory_order_relaxed);
        _pinned_bytes.fetch_add(aligned_bytes, std::memory_order_relaxed);
        if (_vfs_node) _vfs_node->add_used(aligned_bytes);
        return _arena_base + aligned_cur;
    }
    return nullptr;
}

void RedjetParkArenaSlabAllocator::deallocate_pinned(void* ptr, size_t bytes) {
    if (!ptr || !_arena_base) return;

    uintptr_t base_addr = reinterpret_cast<uintptr_t>(_arena_base);
    uintptr_t target_addr = reinterpret_cast<uintptr_t>(ptr);
    if (target_addr < base_addr || target_addr >= (base_addr + _arena_capacity)) return;

    size_t aligned_bytes = (bytes + 4095) & ~4095;

    hs_ticket_guard_t guard(_park_lock);
    FreeChunk* chunk = reinterpret_cast<FreeChunk*>(ptr);
    chunk->size = aligned_bytes;
    chunk->next = _freelist_head;
    _freelist_head = chunk;

    if (_pinned_bytes.load(std::memory_order_relaxed) >= aligned_bytes) {
        _pinned_bytes.fetch_sub(aligned_bytes, std::memory_order_relaxed);
    }
    if (_pinned_object_count.load(std::memory_order_relaxed) > 0) {
        _pinned_object_count.fetch_sub(1, std::memory_order_relaxed);
    }
    if (_vfs_node) _vfs_node->sub_used(aligned_bytes);
}

/* ========================================================================= */
/* 4. FALLINE SLAB ALLOCATOR                                                 */
/* ========================================================================= */

RedjetFallineSlabAllocator::RedjetFallineSlabAllocator() {
    _envelope_class.init(ENVELOPE_SIZE);
}

RedjetFallineSlabAllocator::~RedjetFallineSlabAllocator() {
    destroy();
}

bool RedjetFallineSlabAllocator::init(size_t capacity_bytes, RedjetVFSNode* node) {
    _arena_capacity = capacity_bytes;
    _vfs_node = node;
    _arena_base = static_cast<uint8_t*>(vfs_os_reserve_commit(_arena_capacity));
    if (!_arena_base) return false;

    _bump_offset.store(0, std::memory_order_relaxed);
    _envelope_class.init(ENVELOPE_SIZE);
    return true;
}

void RedjetFallineSlabAllocator::destroy() {
    if (_arena_base) {
        vfs_os_release(_arena_base, _arena_capacity);
        _arena_base = nullptr;
    }
    _arena_capacity = 0;
}

void* RedjetFallineSlabAllocator::allocate_envelope() {
    void* result = _envelope_class.allocate_from_freelist();
    if (result != nullptr) {
        _envelopes_allocated.fetch_add(1, std::memory_order_relaxed);
        if (_vfs_node) _vfs_node->add_used(ENVELOPE_SIZE);
        return result;
    }

    hs_ticket_guard_t guard(_bump_lock);
    size_t cur = _bump_offset.load(std::memory_order_relaxed);
    size_t aligned_cur = (cur + 63) & ~63;

    if (aligned_cur + ENVELOPE_SIZE <= _arena_capacity) {
        _bump_offset.store(aligned_cur + ENVELOPE_SIZE, std::memory_order_relaxed);
        result = _arena_base + aligned_cur;
        _envelopes_allocated.fetch_add(1, std::memory_order_relaxed);
        if (_vfs_node) _vfs_node->add_used(ENVELOPE_SIZE);
        return result;
    }
    return nullptr;
}

void RedjetFallineSlabAllocator::recycle_envelope(void* ptr) {
    if (!ptr) return;
    _envelope_class.deallocate(ptr, RedjetSlabClass::LIFO);
    if (_vfs_node) _vfs_node->sub_used(ENVELOPE_SIZE);
}

size_t RedjetFallineSlabAllocator::used_bytes() const noexcept {
    return _bump_offset.load(std::memory_order_relaxed);
}

/* ========================================================================= */
/* 5. HEADER PANE SLAB ALLOCATOR                                             */
/* ========================================================================= */

RedjetHeaderPaneSlabAllocator::RedjetHeaderPaneSlabAllocator() {
    _header_class.init(HEADER_ENTRY_SIZE);
}

RedjetHeaderPaneSlabAllocator::~RedjetHeaderPaneSlabAllocator() {
    destroy();
}

bool RedjetHeaderPaneSlabAllocator::init(size_t capacity_bytes, RedjetVFSNode* node) {
    _arena_capacity = capacity_bytes;
    _vfs_node = node;
    _arena_base = static_cast<uint8_t*>(vfs_os_reserve_commit(_arena_capacity));
    if (!_arena_base) return false;

    _bump_offset.store(0, std::memory_order_relaxed);
    _header_class.init(HEADER_ENTRY_SIZE);
    return true;
}

void RedjetHeaderPaneSlabAllocator::destroy() {
    if (_arena_base) {
        vfs_os_release(_arena_base, _arena_capacity);
        _arena_base = nullptr;
    }
    _arena_capacity = 0;
}

void* RedjetHeaderPaneSlabAllocator::allocate_header_slot() {
    void* result = _header_class.allocate_from_freelist();
    if (result != nullptr) {
        _headers_stored.fetch_add(1, std::memory_order_relaxed);
        if (_vfs_node) _vfs_node->add_used(HEADER_ENTRY_SIZE);
        return result;
    }

    hs_ticket_guard_t guard(_bump_lock);
    size_t cur = _bump_offset.load(std::memory_order_relaxed);
    size_t aligned_cur = (cur + 63) & ~63;

    if (aligned_cur + HEADER_ENTRY_SIZE <= _arena_capacity) {
        _bump_offset.store(aligned_cur + HEADER_ENTRY_SIZE, std::memory_order_relaxed);
        result = _arena_base + aligned_cur;
        _headers_stored.fetch_add(1, std::memory_order_relaxed);
        if (_vfs_node) _vfs_node->add_used(HEADER_ENTRY_SIZE);
        return result;
    }
    return nullptr;
}

void RedjetHeaderPaneSlabAllocator::recycle_header_slot(void* ptr) {
    if (!ptr) return;
    _header_class.deallocate(ptr, RedjetSlabClass::LIFO);
    if (_vfs_node) _vfs_node->sub_used(HEADER_ENTRY_SIZE);
}

size_t RedjetHeaderPaneSlabAllocator::used_bytes() const noexcept {
    return _bump_offset.load(std::memory_order_relaxed);
}
