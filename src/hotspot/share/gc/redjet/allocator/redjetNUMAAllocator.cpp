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

#include "gc/redjet/allocator/redjetNUMAAllocator.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetOSAffinity.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include <new>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

RedjetNUMAAllocator g_redjet_numa_allocator;

/* ========================================================================= */
/* 1. REDJET SOCKET RUNWAY IMPLEMENTATION                                    */
/* ========================================================================= */

RedjetSocketRunway::RedjetSocketRunway() = default;

RedjetSocketRunway::~RedjetSocketRunway() {
    destroy();
}

bool RedjetSocketRunway::init(uint32_t id, size_t capacity_bytes, uint8_t* base_ptr) {
    socket_id = id;
    socket_capacity = capacity_bytes;
    socket_bump_offset.store(0, std::memory_order_relaxed);

    if (base_ptr != nullptr) {
        socket_heap_base = base_ptr;
    } else {
#if defined(_WIN32)
        socket_heap_base = static_cast<uint8_t*>(VirtualAllocExNuma(
            GetCurrentProcess(), nullptr, capacity_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, id));
        if (!socket_heap_base) {
            socket_heap_base = static_cast<uint8_t*>(VirtualAlloc(
                nullptr, capacity_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        }
#else
        socket_heap_base = static_cast<uint8_t*>(mmap(
            nullptr, capacity_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (socket_heap_base == MAP_FAILED) socket_heap_base = nullptr;
#endif
    }

    if (!socket_heap_base) {
        return false;
    }

    // Initialize 32 size classes for this socket
    for (size_t c = 0; c < NUM_CLASSES; ++c) {
        slab_classes[c].init(hs_slab_allocator_t::CLASS_SIZES[c]);
    }

    return true;
}

void RedjetSocketRunway::destroy() {
    if (socket_heap_base) {
#if defined(_WIN32)
        VirtualFree(socket_heap_base, 0, MEM_RELEASE);
#else
        munmap(socket_heap_base, socket_capacity);
#endif
        socket_heap_base = nullptr;
    }
    socket_capacity = 0;
    socket_bump_offset.store(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(chain_lock);
    hs_chained_chunk_t* cur = chained_head;
    while (cur != nullptr) {
        hs_chained_chunk_t* next = cur->next;
        if (cur->base) {
#if defined(_WIN32)
            VirtualFree(cur->base, 0, MEM_RELEASE);
#else
            munmap(cur->base, cur->capacity);
#endif
        }
        delete cur;
        cur = next;
    }
    chained_head = nullptr;
    chained_chunk_count.store(0, std::memory_order_relaxed);
}

void* RedjetSocketRunway::bump_alloc(size_t bytes, size_t align) {
    if (!socket_heap_base || bytes == 0) return nullptr;

    hs_ticket_guard_t guard(socket_lock);
    size_t current = socket_bump_offset.load(std::memory_order_relaxed);
    uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(socket_heap_base + current);
    uintptr_t aligned_ptr = (raw_ptr + align - 1) & ~(align - 1);
    size_t padding = aligned_ptr - raw_ptr;
    size_t total_needed = bytes + padding;

    if (current + total_needed > socket_capacity) {
        return nullptr; // Socket runway capacity reached
    }

    socket_bump_offset.store(current + total_needed, std::memory_order_relaxed);
    local_allocations.fetch_add(1, std::memory_order_relaxed);
    allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);

    return reinterpret_cast<void*>(aligned_ptr);
}

void* RedjetSocketRunway::slab_alloc(size_t bytes) {
    for (size_t c = 0; c < NUM_CLASSES; ++c) {
        if (bytes <= hs_slab_allocator_t::CLASS_SIZES[c]) {
            void* p = slab_classes[c].allocate_from_freelist();
            if (p != nullptr) {
                local_allocations.fetch_add(1, std::memory_order_relaxed);
                allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
                return p;
            }
            break;
        }
    }
    // If freelist was empty, allocate from socket bump pointer
    return bump_alloc(bytes, 64);
}

void* RedjetSocketRunway::allocate_from_chained_chunks(size_t bytes, size_t align) {
    if (bytes == 0) return nullptr;

    std::lock_guard<std::mutex> lock(chain_lock);

    // 1. Try existing chained chunks
    hs_chained_chunk_t* cur = chained_head;
    while (cur != nullptr) {
        size_t current = cur->bump_offset.load(std::memory_order_relaxed);
        uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(cur->base + current);
        uintptr_t aligned_ptr = (raw_ptr + align - 1) & ~(align - 1);
        size_t padding = aligned_ptr - raw_ptr;
        size_t total_needed = bytes + padding;

        if (current + total_needed <= cur->capacity) {
            cur->bump_offset.store(current + total_needed, std::memory_order_relaxed);
            local_allocations.fetch_add(1, std::memory_order_relaxed);
            allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
            return reinterpret_cast<void*>(aligned_ptr);
        }
        cur = cur->next;
    }

    // 2. Allocate fresh OS chunk bound to this socket
    size_t chunk_sz = std::max(bytes + 4096, 16ULL * 1024 * 1024); // 16 MB chunk minimum
    uint8_t* chunk_mem = nullptr;
#if defined(_WIN32)
    chunk_mem = static_cast<uint8_t*>(VirtualAllocExNuma(
        GetCurrentProcess(), nullptr, chunk_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, socket_id));
    if (!chunk_mem) {
        chunk_mem = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, chunk_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
#else
    chunk_mem = static_cast<uint8_t*>(mmap(
        nullptr, chunk_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (chunk_mem == MAP_FAILED) chunk_mem = nullptr;
#endif

    if (!chunk_mem) return nullptr;

    auto* new_chunk = new (std::nothrow) hs_chained_chunk_t();
    if (!new_chunk) {
#if defined(_WIN32)
        VirtualFree(chunk_mem, 0, MEM_RELEASE);
#else
        munmap(chunk_mem, chunk_sz);
#endif
        return nullptr;
    }

    new_chunk->base = chunk_mem;
    new_chunk->capacity = chunk_sz;

    uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(chunk_mem);
    uintptr_t aligned_ptr = (raw_ptr + align - 1) & ~(align - 1);
    size_t padding = aligned_ptr - raw_ptr;
    new_chunk->bump_offset.store(bytes + padding, std::memory_order_relaxed);

    new_chunk->next = chained_head;
    chained_head = new_chunk;
    chained_chunk_count.fetch_add(1, std::memory_order_relaxed);
    os_expansions.fetch_add(1, std::memory_order_relaxed);
    local_allocations.fetch_add(1, std::memory_order_relaxed);
    allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);

    return reinterpret_cast<void*>(aligned_ptr);
}

void RedjetSocketRunway::slab_free(void* ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
    for (size_t c = 0; c < NUM_CLASSES; ++c) {
        if (bytes <= hs_slab_allocator_t::CLASS_SIZES[c]) {
            slab_classes[c].deallocate(ptr);
            return;
        }
    }
}

size_t RedjetSocketRunway::get_used_bytes() const noexcept {
    return socket_bump_offset.load(std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. REDJET NUMA ALLOCATOR IMPLEMENTATION                                   */
/* ========================================================================= */

RedjetNUMAAllocator::RedjetNUMAAllocator() {
    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        _thread_profiles[i].thread_id = 0;
        _thread_profiles[i].current_socket = 0;
        _thread_profiles[i].current_tlab_size = RedjetElasticTLABConfig::DEFAULT_TLAB_BYTES;
        _thread_profiles[i].previous_waste_bytes = 0;
        _thread_profiles[i].last_refill_tsc = 0;
        _thread_profiles[i].refill_count = 0;
        _thread_profiles[i].estimated_velocity_mbps = 0.0f;
    }
}

RedjetNUMAAllocator::~RedjetNUMAAllocator() {
    destroy();
}

bool RedjetNUMAAllocator::init(size_t socket_count, size_t per_socket_capacity) {
    if (_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    hs_ticket_guard_t guard(_global_lock);

    // Detect hardware NUMA socket count if not explicitly provided
    if (socket_count == 0) {
        socket_count = RedjetHardware::numa_node_count();
        if (socket_count == 0) socket_count = 1;
        if (socket_count > MAX_NUMA_SOCKETS) socket_count = MAX_NUMA_SOCKETS;
    }

    _socket_count = socket_count;
    _per_socket_capacity = per_socket_capacity;

    for (size_t s = 0; s < _socket_count; ++s) {
        if (!_sockets[s].init(static_cast<uint32_t>(s), per_socket_capacity)) {
            // Cleanup on failure
            for (size_t k = 0; k < s; ++k) {
                _sockets[k].destroy();
            }
            return false;
        }
    }

    _initialized.store(true, std::memory_order_release);
    hs_unified_log_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, 0, "numa_alloc",
                        "RedjetNUMAAllocator: Initialized with %zu socket runway branches (%zu MB per socket)",
                        _socket_count, per_socket_capacity / (1024 * 1024));
    return true;
}

void RedjetNUMAAllocator::destroy() {
    hs_ticket_guard_t guard(_global_lock);
    for (size_t s = 0; s < _socket_count; ++s) {
        _sockets[s].destroy();
    }
    _socket_count = 1;
    _initialized.store(false, std::memory_order_release);
}

uint32_t RedjetNUMAAllocator::current_numa_node() const noexcept {
    uint32_t node = RedjetOSAffinity::get_thread_numa_node();
    if (node >= _socket_count) {
        node = 0;
    }
    return node;
}

RedjetThreadTLABProfile* RedjetNUMAAllocator::get_or_create_thread_profile(uint32_t thread_id, uint32_t socket_id) {
    if (thread_id == 0) return nullptr;

    hs_ticket_guard_t guard(_profile_lock);
    size_t empty_idx = MAX_TRACKED_THREADS;

    for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
        if (_thread_profiles[i].thread_id == thread_id) {
            _thread_profiles[i].current_socket = socket_id;
            return &_thread_profiles[i];
        }
        if (empty_idx == MAX_TRACKED_THREADS && _thread_profiles[i].thread_id == 0) {
            empty_idx = i;
        }
    }

    if (empty_idx < MAX_TRACKED_THREADS) {
        _thread_profiles[empty_idx].thread_id = thread_id;
        _thread_profiles[empty_idx].current_socket = socket_id;
        _thread_profiles[empty_idx].current_tlab_size = RedjetElasticTLABConfig::DEFAULT_TLAB_BYTES;
        _thread_profiles[empty_idx].previous_waste_bytes = 0;
        _thread_profiles[empty_idx].last_refill_tsc = sacculus_rdtsc();
        _thread_profiles[empty_idx].refill_count = 0;
        _thread_profiles[empty_idx].estimated_velocity_mbps = 0.0f;
        return &_thread_profiles[empty_idx];
    }

    return nullptr; // Table saturated, caller falls back to default
}

void* RedjetNUMAAllocator::allocate(size_t bytes, uint32_t socket_id, size_t align) {
    if (bytes == 0) return nullptr;

    if (socket_id == UINT32_MAX || socket_id >= _socket_count) {
        socket_id = current_numa_node();
    }

    _total_numa_allocations.fetch_add(1, std::memory_order_relaxed);

    // Tier 1 & 2: Local NUMA socket runway allocation
    void* ptr = nullptr;
    if (bytes <= hs_slab_allocator_t::TLAB_MAX_OBJ_SIZE) {
        ptr = _sockets[socket_id].slab_alloc(bytes);
    } else {
        ptr = _sockets[socket_id].bump_alloc(bytes, align);
    }

    if (ptr != nullptr) {
        return ptr;
    }

    // Tier 3: Cross-socket stealing fallback from sibling nodes
    for (size_t s = 0; s < _socket_count; ++s) {
        if (s == socket_id) continue;

        if (bytes <= hs_slab_allocator_t::TLAB_MAX_OBJ_SIZE) {
            ptr = _sockets[s].slab_alloc(bytes);
        } else {
            ptr = _sockets[s].bump_alloc(bytes, align);
        }

        if (ptr != nullptr) {
            _sockets[s].remote_steals.fetch_add(1, std::memory_order_relaxed);
            _total_cross_socket_steals.fetch_add(1, std::memory_order_relaxed);
            return ptr;
        }
    }

    // Tier 4: Autonomous OS chained chunk expansion on originating socket
    ptr = _sockets[socket_id].allocate_from_chained_chunks(bytes, align);
    if (ptr != nullptr) {
        _total_os_chunk_expansions.fetch_add(1, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_ALLOC_CHUNK_GROW, HS_Ebin1_INFO, socket_id, "numa_tier4",
                            "RedjetNUMAAllocator: Tier 4 OS chunk expansion succeeded on Socket %u (%zu bytes)",
                            socket_id, bytes);
        return ptr;
    }

    return nullptr;
}

void RedjetNUMAAllocator::deallocate(void* ptr, size_t bytes, uint32_t socket_id) {
    if (!ptr || bytes == 0) return;
    if (socket_id >= _socket_count) socket_id = 0;
    _sockets[socket_id].slab_free(ptr, bytes);
}

size_t RedjetNUMAAllocator::compute_elastic_tlab_size(
    uint32_t thread_id, uint32_t socket_id, size_t requested_bytes, size_t waste_bytes) {

    RedjetThreadTLABProfile* prof = get_or_create_thread_profile(thread_id, socket_id);
    size_t current_sz = prof ? prof->current_tlab_size : RedjetElasticTLABConfig::DEFAULT_TLAB_BYTES;

    uint64_t now_tsc = sacculus_rdtsc();
    uint64_t elapsed_cycles = (prof && prof->last_refill_tsc > 0) ? (now_tsc - prof->last_refill_tsc) : 1000000;

    // Check MLTC bus congestion level: throttle expansion if bus saturated
    RedjetBusCongestionLevel bus_level = g_hs_mltc_solver.current_congestion_level();

    // Elastic heuristic:
    // If refill occurred very quickly (< 500,000 cycles ~ 0.2ms) and waste is low (< 10%): expand!
    // If waste is high (> 25% of TLAB) or bus is under severe contention: shrink!
    size_t target_sz = current_sz;

    if (waste_bytes > (current_sz / 4) || bus_level >= RedjetBusCongestionLevel::BUS_SATURATION) {
        target_sz = static_cast<size_t>(static_cast<float>(current_sz) * RedjetElasticTLABConfig::SHRINK_FACTOR);
        if (target_sz < RedjetElasticTLABConfig::MIN_TLAB_BYTES) {
            target_sz = RedjetElasticTLABConfig::MIN_TLAB_BYTES;
        }
        _total_elastic_shrinks.fetch_add(1, std::memory_order_relaxed);
    } else if (elapsed_cycles < 2000000 && bus_level <= RedjetBusCongestionLevel::MILD_CONTENTION) {
        target_sz = static_cast<size_t>(static_cast<float>(current_sz) * RedjetElasticTLABConfig::EXPANSION_FACTOR);
        if (target_sz > RedjetElasticTLABConfig::MAX_TLAB_BYTES) {
            target_sz = RedjetElasticTLABConfig::MAX_TLAB_BYTES;
        }
        _total_elastic_expansions.fetch_add(1, std::memory_order_relaxed);
    }

    // Ensure requested_bytes fits
    if (target_sz < requested_bytes) {
        target_sz = (requested_bytes + 4095) & ~4095;
    }

    if (prof) {
        prof->current_tlab_size = target_sz;
        prof->previous_waste_bytes = waste_bytes;
        prof->last_refill_tsc = now_tsc;
        prof->refill_count++;
    }

    return target_sz;
}

bool RedjetNUMAAllocator::refill_elastic_tlab(
    hs_tlab_buffer_t* tlab, uint32_t thread_id, size_t requested_bytes, uint32_t socket_id) {

    if (!tlab) return false;

    if (socket_id == UINT32_MAX || socket_id >= _socket_count) {
        socket_id = current_numa_node();
    }

    // Calculate remaining unused bytes in old TLAB as waste
    size_t waste_bytes = (tlab->capacity > tlab->offset) ? (tlab->capacity - tlab->offset) : 0;

    // Compute dynamic elastic TLAB size
    size_t elastic_sz = compute_elastic_tlab_size(thread_id, socket_id, requested_bytes, waste_bytes);

    // Allocate fresh buffer from socket runway branch
    void* buf = allocate(elastic_sz, socket_id, 64);
    if (!buf) {
        // Fallback to smaller min size if large chunk failed
        elastic_sz = std::max(requested_bytes, RedjetElasticTLABConfig::MIN_TLAB_BYTES);
        buf = allocate(elastic_sz, socket_id, 64);
        if (!buf) return false;
    }

    tlab->owner_runway_base = _sockets[socket_id].socket_heap_base;
    tlab->owner_epoch = g_hs_runway_epoch.load(std::memory_order_relaxed);
    tlab->base = static_cast<uint8_t*>(buf);
    tlab->offset = 0;
    tlab->capacity = elastic_sz;

    _sockets[socket_id].tlab_refills_served.fetch_add(1, std::memory_order_relaxed);
    return true;
}

size_t RedjetNUMAAllocator::get_socket_used_bytes(uint32_t socket_id) const noexcept {
    if (socket_id >= _socket_count) return 0;
    return _sockets[socket_id].get_used_bytes();
}

size_t RedjetNUMAAllocator::get_total_used_bytes() const noexcept {
    size_t total = 0;
    for (size_t s = 0; s < _socket_count; ++s) {
        total += _sockets[s].get_used_bytes();
    }
    return total;
}
