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
#include <thread>

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
/* 1. REDJET CCD RUNWAY IMPLEMENTATION                                       */
/* ========================================================================= */

bool RedjetCCDRunway::init(uint32_t ccd, uint32_t socket, uint64_t mask, size_t capacity_bytes, uint8_t* base_ptr) {
    ccd_id = ccd;
    parent_socket_id = socket;
    core_mask = mask;
    ccd_capacity = capacity_bytes;
    ccd_heap_base = base_ptr;
    ccd_bump_offset.store(0, std::memory_order_relaxed);
    local_allocations.store(0, std::memory_order_relaxed);
    cross_ccd_steals.store(0, std::memory_order_relaxed);
    allocated_bytes.store(0, std::memory_order_relaxed);

    if (ccd_heap_base != nullptr) {
        for (size_t c = 0; c < NUM_CLASSES; ++c) {
            local_slabs[c].init(hs_slab_allocator_t::CLASS_SIZES[c]);
        }
        return true;
    }
    return false;
}

void* RedjetCCDRunway::bump_alloc(size_t bytes, size_t align) {
    if (!ccd_heap_base || bytes == 0) return nullptr;

    hs_ticket_guard_t guard(ccd_lock);
    size_t current = ccd_bump_offset.load(std::memory_order_relaxed);
    uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(ccd_heap_base + current);
    uintptr_t aligned_ptr = (raw_ptr + align - 1) & ~(align - 1);
    size_t padding = aligned_ptr - raw_ptr;
    size_t total_needed = bytes + padding;

    if (current + total_needed > ccd_capacity) {
        return nullptr; // CCD runway exhausted
    }

    ccd_bump_offset.store(current + total_needed, std::memory_order_relaxed);
    local_allocations.fetch_add(1, std::memory_order_relaxed);
    allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return reinterpret_cast<void*>(aligned_ptr);
}

void* RedjetCCDRunway::slab_alloc(size_t bytes) {
    for (size_t c = 0; c < NUM_CLASSES; ++c) {
        if (bytes <= hs_slab_allocator_t::CLASS_SIZES[c]) {
            void* p = local_slabs[c].allocate_from_freelist();
            if (p != nullptr) {
                local_allocations.fetch_add(1, std::memory_order_relaxed);
                allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
                return p;
            }
            break;
        }
    }
    return bump_alloc(bytes, 64);
}

void RedjetCCDRunway::slab_free(void* ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
    for (size_t c = 0; c < NUM_CLASSES; ++c) {
        if (bytes <= hs_slab_allocator_t::CLASS_SIZES[c]) {
            local_slabs[c].release_to_freelist(ptr);
            return;
        }
    }
}

size_t RedjetCCDRunway::get_used_bytes() const noexcept {
    return ccd_bump_offset.load(std::memory_order_relaxed);
}

/* ========================================================================= */
/* 2. MULTI-CHANNEL MEMORY STRIPING IMPLEMENTATION                           */
/* ========================================================================= */

void* RedjetMemoryStriping::allocate_striped(size_t total_bytes, uint32_t socket_id) {
    if (total_bytes == 0) return nullptr;

    size_t aligned_bytes = (total_bytes + 4095) & ~4095;
    void* mem = nullptr;

#if defined(_WIN32)
    mem = VirtualAllocExNuma(
        GetCurrentProcess(), nullptr, aligned_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, socket_id);
    if (!mem) {
        mem = VirtualAlloc(nullptr, aligned_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
#else
    mem = mmap(nullptr, aligned_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) mem = nullptr;
#endif

    if (mem != nullptr) {
        total_striped_bytes.fetch_add(aligned_bytes, std::memory_order_relaxed);
        total_striped_allocations.fetch_add(1, std::memory_order_relaxed);
    }

    return mem;
}

void RedjetMemoryStriping::free_striped(void* ptr, size_t total_bytes) {
    if (!ptr || total_bytes == 0) return;
    size_t aligned_bytes = (total_bytes + 4095) & ~4095;
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, aligned_bytes);
#endif
}

/* ========================================================================= */
/* 3. MESH TOPOLOGICAL DISTANCE IMPLEMENTATION                               */
/* ========================================================================= */

uint32_t RedjetMeshDistance::compute_distance(uint32_t src_core, uint32_t dst_core, RedjetMegaServerClass server_class) {
    if (src_core == dst_core) return 0;
    if ((src_core ^ 1) == dst_core) return 0; // SMT sibling

    switch (server_class) {
        case RedjetMegaServerClass::AMD_THREADRIPPER_PRO: {
            uint32_t src_ccd = src_core / 8;
            uint32_t dst_ccd = dst_core / 8;
            if (src_ccd == dst_ccd) return 1; // Same CCD (L3 cache shared)
            return 3;                         // Cross-CCD via Infinity Fabric
        }
        case RedjetMegaServerClass::INTEL_XEON_6980P_SNC3: {
            uint32_t src_snc = src_core / 42;
            uint32_t dst_snc = dst_core / 42;
            if (src_snc == dst_snc) return 2; // Same SNC sub-NUMA cluster
            return 3;                         // Cross-SNC on-die mesh
        }
        case RedjetMegaServerClass::AMPERE_ALTRA_MAX_MESH: {
            // 2D Mesh Manhattan distance
            uint32_t src_x = src_core % 16, src_y = src_core / 16;
            uint32_t dst_x = dst_core % 16, dst_y = dst_core / 16;
            uint32_t dist = (src_x > dst_x ? src_x - dst_x : dst_x - src_x) +
                            (src_y > dst_y ? src_y - dst_y : dst_y - src_y);
            return (dist <= 2) ? 1 : (dist <= 6 ? 2 : 3);
        }
        default: {
            uint32_t src_sock = src_core / 32;
            uint32_t dst_sock = dst_core / 32;
            return (src_sock == dst_sock) ? 2 : 4;
        }
    }
}

/* ========================================================================= */
/* 4. REDJET SOCKET RUNWAY IMPLEMENTATION                                    */
/* ========================================================================= */

RedjetSocketRunway::RedjetSocketRunway() = default;

RedjetSocketRunway::~RedjetSocketRunway() {
    destroy();
}

bool RedjetSocketRunway::init(uint32_t id, size_t capacity_bytes, uint8_t* base_ptr, uint32_t num_ccds) {
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

    // Partition socket heap into CCX/CCD local runways
    ccd_count = (num_ccds > 0 && num_ccds <= MAX_CCDS_PER_SOCKET) ? num_ccds : 4;
    size_t per_ccd_cap = capacity_bytes / (ccd_count + 1); // Save 1 partition for shared socket overflow

    for (uint32_t i = 0; i < ccd_count; ++i) {
        uint8_t* ccd_base = socket_heap_base + (i * per_ccd_cap);
        uint64_t mask = 0xFFULL << (i * 8);
        ccd_runways[i].init(i, id, mask, per_ccd_cap, ccd_base);
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
        return nullptr;
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
    return bump_alloc(bytes, 64);
}

void* RedjetSocketRunway::allocate_from_chained_chunks(size_t bytes, size_t align) {
    if (bytes == 0) return nullptr;

    std::lock_guard<std::mutex> lock(chain_lock);

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

    size_t chunk_sz = std::max(bytes + 4096, 16ULL * 1024 * 1024);
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
    new_chunk->socket_id = socket_id;
    new_chunk->next = chained_head;

    uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(chunk_mem);
    uintptr_t aligned_ptr = (raw_ptr + align - 1) & ~(align - 1);
    size_t total_needed = bytes + (aligned_ptr - raw_ptr);

    new_chunk->bump_offset.store(total_needed, std::memory_order_relaxed);
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
            slab_classes[c].release_to_freelist(ptr);
            return;
        }
    }
}

size_t RedjetSocketRunway::get_used_bytes() const noexcept {
    size_t used = socket_bump_offset.load(std::memory_order_relaxed);
    hs_chained_chunk_t* cur = chained_head;
    while (cur != nullptr) {
        used += cur->bump_offset.load(std::memory_order_relaxed);
        cur = cur->next;
    }
    return used;
}

RedjetCCDRunway* RedjetSocketRunway::get_ccd_runway_for_core(uint32_t core_id) {
    if (ccd_count == 0) return nullptr;
    uint32_t ccd_idx = (core_id / 8) % ccd_count;
    return &ccd_runways[ccd_idx];
}

/* ========================================================================= */
/* 5. REDJET NUMA ALLOCATOR NEXUS IMPLEMENTATION                             */
/* ========================================================================= */

RedjetNUMAAllocator::RedjetNUMAAllocator() :
    _socket_count(1),
    _per_socket_capacity(256ULL * 1024 * 1024),
    _initialized(false),
    _server_class(RedjetMegaServerClass::GENERIC_NUMA) {
}

RedjetNUMAAllocator::~RedjetNUMAAllocator() {
    destroy();
}

void RedjetNUMAAllocator::detect_mega_server_topology() {
    unsigned int hw_threads = std::thread::hardware_concurrency();

    if (hw_threads >= 96) {
        _server_class = RedjetMegaServerClass::AMD_THREADRIPPER_PRO;
        _striping.active_channels = 8;
    } else if (hw_threads >= 64) {
        _server_class = RedjetMegaServerClass::INTEL_XEON_6980P_SNC3;
        _striping.active_channels = 12;
    } else {
        _server_class = RedjetMegaServerClass::GENERIC_NUMA;
        _striping.active_channels = 4;
    }
}

bool RedjetNUMAAllocator::init(size_t socket_count, size_t per_socket_capacity) {
    hs_ticket_guard_t guard(_global_lock);
    if (_initialized.load(std::memory_order_relaxed)) {
        return true;
    }

    detect_mega_server_topology();

    if (socket_count == 0) {
        socket_count = RedjetHardware::numa_nodes_count();
        if (socket_count == 0) socket_count = 1;
    }

    _socket_count = std::min(socket_count, MAX_NUMA_SOCKETS);
    _per_socket_capacity = per_socket_capacity;

    uint32_t num_ccds = (_server_class == RedjetMegaServerClass::AMD_THREADRIPPER_PRO) ? 12 : 4;

    for (size_t s = 0; s < _socket_count; ++s) {
        if (!_sockets[s].init(static_cast<uint32_t>(s), per_socket_capacity, nullptr, num_ccds)) {
            destroy();
            return false;
        }
    }

    _initialized.store(true, std::memory_order_release);
    return true;
}

void RedjetNUMAAllocator::destroy() {
    hs_ticket_guard_t guard(_global_lock);
    for (size_t s = 0; s < _socket_count; ++s) {
        _sockets[s].destroy();
    }
    _initialized.store(false, std::memory_order_release);
}

uint32_t RedjetNUMAAllocator::current_numa_node() const noexcept {
    uint32_t cpu = RedjetOSAffinity::current_logical_cpu();
    return (_socket_count > 0) ? (cpu % _socket_count) : 0;
}

void* RedjetNUMAAllocator::allocate_from_ccd(size_t bytes, uint32_t core_id, size_t align) {
    if (!_initialized.load(std::memory_order_acquire) || bytes == 0) {
        return nullptr;
    }

    uint32_t sock = (_socket_count > 0) ? (core_id % _socket_count) : 0;
    RedjetCCDRunway* ccd = _sockets[sock].get_ccd_runway_for_core(core_id);
    if (ccd != nullptr) {
        void* p = (bytes <= 32768) ? ccd->slab_alloc(bytes) : ccd->bump_alloc(bytes, align);
        if (p != nullptr) {
            _total_numa_allocations.fetch_add(1, std::memory_order_relaxed);
            return p;
        }
    }

    return allocate(bytes, sock, align);
}

void* RedjetNUMAAllocator::allocate_striped(size_t bytes, uint32_t socket_id) {
    if (socket_id == UINT32_MAX || socket_id >= _socket_count) {
        socket_id = current_numa_node();
    }
    return _striping.allocate_striped(bytes, socket_id);
}

void RedjetNUMAAllocator::free_striped(void* ptr, size_t bytes) {
    _striping.free_striped(ptr, bytes);
}

void* RedjetNUMAAllocator::allocate(size_t bytes, uint32_t socket_id, size_t align) {
    if (!_initialized.load(std::memory_order_acquire) || bytes == 0) {
        return nullptr;
    }

    if (socket_id == UINT32_MAX || socket_id >= _socket_count) {
        socket_id = current_numa_node();
    }

    // Tier 1: Per-socket runway bump allocator / local slab freelist
    void* ptr = (bytes <= 32768) ? _sockets[socket_id].slab_alloc(bytes)
                                : _sockets[socket_id].bump_alloc(bytes, align);
    if (ptr != nullptr) {
        _total_numa_allocations.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }

    // Tier 2: Nearest peer stealing
    if (_socket_count > 1) {
        for (size_t delta = 1; delta < _socket_count; ++delta) {
            uint32_t peer_socket = static_cast<uint32_t>((socket_id + delta) % _socket_count);
            ptr = (bytes <= 32768) ? _sockets[peer_socket].slab_alloc(bytes)
                                   : _sockets[peer_socket].bump_alloc(bytes, align);
            if (ptr != nullptr) {
                _sockets[peer_socket].remote_steals.fetch_add(1, std::memory_order_relaxed);
                _total_cross_socket_steals.fetch_add(1, std::memory_order_relaxed);
                _total_numa_allocations.fetch_add(1, std::memory_order_relaxed);
                return ptr;
            }
        }
    }

    // Tier 3: OS chained chunk expansion
    ptr = _sockets[socket_id].allocate_from_chained_chunks(bytes, align);
    if (ptr != nullptr) {
        _total_os_chunk_expansions.fetch_add(1, std::memory_order_relaxed);
        _total_numa_allocations.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }

    return nullptr;
}

void RedjetNUMAAllocator::deallocate(void* ptr, size_t bytes, uint32_t socket_id) {
    if (!ptr || bytes == 0 || socket_id >= _socket_count) return;
    _sockets[socket_id].slab_free(ptr, bytes);
}

RedjetThreadTLABProfile* RedjetNUMAAllocator::get_or_create_thread_profile(uint32_t thread_id, uint32_t socket_id) {
    hs_ticket_guard_t guard(_profile_lock);
    size_t slot = thread_id % MAX_TRACKED_THREADS;
    RedjetThreadTLABProfile& prof = _thread_profiles[slot];

    if (prof.thread_id != thread_id) {
        prof.thread_id = thread_id;
        prof.current_socket = socket_id;
        prof.current_tlab_size = RedjetElasticTLABConfig::DEFAULT_TLAB_BYTES;
        prof.previous_waste_bytes = 0;
        prof.last_refill_tsc = sacculus_rdtsc();
        prof.refill_count = 0;
        prof.estimated_velocity_mbps = 0.0f;
    }

    return &prof;
}

size_t RedjetNUMAAllocator::compute_elastic_tlab_size(
    uint32_t thread_id, uint32_t socket_id, size_t requested_bytes, size_t waste_bytes) {

    RedjetThreadTLABProfile* prof = get_or_create_thread_profile(thread_id, socket_id);
    size_t current_sz = prof ? prof->current_tlab_size : RedjetElasticTLABConfig::DEFAULT_TLAB_BYTES;

    uint64_t now_tsc = sacculus_rdtsc();
    uint64_t elapsed_cycles = (prof && prof->last_refill_tsc > 0) ? (now_tsc - prof->last_refill_tsc) : 1000000;

    RedjetBusCongestionLevel bus_level = g_hs_mltc_solver.current_congestion_level();
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

    size_t waste_bytes = (tlab->capacity > tlab->offset) ? (tlab->capacity - tlab->offset) : 0;
    size_t elastic_sz = compute_elastic_tlab_size(thread_id, socket_id, requested_bytes, waste_bytes);

    void* buf = allocate(elastic_sz, socket_id, 64);
    if (!buf) {
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
