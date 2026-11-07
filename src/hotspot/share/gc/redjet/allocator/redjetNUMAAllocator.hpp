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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETNUMAALLOCATOR_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETNUMAALLOCATOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// RedjetNUMAAllocator: NUMA-Aware Multi-Tier Runway Allocator Subsystem
// Per-Socket Runway Branches, Elastic TLAB Sizing, and Cross-Socket Stealing
// ============================================================================

struct alignas(64) RedjetSocketRunway {
    static constexpr size_t NUM_CLASSES = 32;

    uint32_t            socket_id{ 0 };
    uint8_t*            socket_heap_base{ nullptr };
    size_t              socket_capacity{ 0 };
    std::atomic<size_t> socket_bump_offset{ 0 };
    hs_ticket_lock_t    socket_lock;

    // Per-socket slab classes (eliminates cross-node UPI/QPI/Infinity Fabric traffic)
    hs_slab_class_t     slab_classes[NUM_CLASSES];

    // Socket-level Telemetry
    std::atomic<uint64_t> local_allocations{ 0 };
    std::atomic<uint64_t> remote_steals{ 0 };
    std::atomic<size_t>   allocated_bytes{ 0 };
    std::atomic<uint64_t> tlab_refills_served{ 0 };

    // Tier 4 OS Chained Expansion Chunks
    std::mutex          chain_lock;
    hs_chained_chunk_t* chained_head{ nullptr };
    std::atomic<size_t> chained_chunk_count{ 0 };
    std::atomic<uint64_t> os_expansions{ 0 };

    RedjetSocketRunway();
    ~RedjetSocketRunway();

    bool init(uint32_t id, size_t capacity_bytes, uint8_t* base_ptr = nullptr);
    void destroy();

    void* bump_alloc(size_t bytes, size_t align = 64);
    void* slab_alloc(size_t bytes);
    void* allocate_from_chained_chunks(size_t bytes, size_t align = 64);
    void  slab_free(void* ptr, size_t bytes);
    size_t get_used_bytes() const noexcept;
};

/* ========================================================================= */
/* 2. ELASTIC TLAB TUNER STATE                                               */
/* ========================================================================= */

struct RedjetElasticTLABConfig {
    static constexpr size_t MIN_TLAB_BYTES     = 64 * 1024;        // 64 KB minimum
    static constexpr size_t DEFAULT_TLAB_BYTES = 512 * 1024;       // 512 KB default
    static constexpr size_t MAX_TLAB_BYTES     = 4 * 1024 * 1024;  // 4 MB maximum
    static constexpr float  EXPANSION_FACTOR   = 1.5f;             // Grow by 50% on frequent refills
    static constexpr float  SHRINK_FACTOR      = 0.75f;            // Shrink by 25% on high waste
};

struct alignas(64) RedjetThreadTLABProfile {
    uint32_t            thread_id{ 0 };
    uint32_t            current_socket{ 0 };
    size_t              current_tlab_size{ RedjetElasticTLABConfig::DEFAULT_TLAB_BYTES };
    size_t              previous_waste_bytes{ 0 };
    uint64_t            last_refill_tsc{ 0 };
    uint64_t            refill_count{ 0 };
    float               estimated_velocity_mbps{ 0.0f };
};

/* ========================================================================= */
/* 3. REDJET NUMA ALLOCATOR NEXUS                                            */
/* ========================================================================= */

class RedjetNUMAAllocator {
public:
    static constexpr size_t MAX_NUMA_SOCKETS = 32;

private:
    RedjetSocketRunway   _sockets[MAX_NUMA_SOCKETS];
    size_t               _socket_count{ 1 };
    size_t               _per_socket_capacity{ 0 };
    std::atomic<bool>    _initialized{ false };
    hs_ticket_lock_t     _global_lock;

    // Elastic TLAB tracking table
    static constexpr size_t MAX_TRACKED_THREADS = 1024;
    RedjetThreadTLABProfile _thread_profiles[MAX_TRACKED_THREADS];
    hs_ticket_lock_t        _profile_lock;

    std::atomic<uint64_t> _total_numa_allocations{ 0 };
    std::atomic<uint64_t> _total_cross_socket_steals{ 0 };
    std::atomic<uint64_t> _total_os_chunk_expansions{ 0 };
    std::atomic<uint64_t> _total_elastic_expansions{ 0 };
    std::atomic<uint64_t> _total_elastic_shrinks{ 0 };

    // Locates or registers thread profile
    RedjetThreadTLABProfile* get_or_create_thread_profile(uint32_t thread_id, uint32_t socket_id);

public:
    RedjetNUMAAllocator();
    ~RedjetNUMAAllocator();

    // Initializes NUMA multi-tier runway with detected host sockets
    RJ_API bool init(size_t socket_count = 0, size_t per_socket_capacity = 256ULL * 1024 * 1024);
    RJ_API void destroy();

    // Resolves current thread's NUMA node affinity
    RJ_API uint32_t current_numa_node() const noexcept;

    // Multi-Tier Allocation Pipeline:
    // Tier 1: Per-socket runway bump allocator
    // Tier 2: Per-socket runway slab class freelist
    // Tier 3: Cross-socket stealing from nearest neighbor
    // Tier 4: OS chained chunk expansion
    RJ_API void* allocate(size_t bytes, uint32_t socket_id = UINT32_MAX, size_t align = 64);

    // Deallocates memory to appropriate socket freelist
    RJ_API void deallocate(void* ptr, size_t bytes, uint32_t socket_id);

    // Elastic TLAB Refill: computes dynamic size based on allocation rate and waste ratio
    RJ_API bool refill_elastic_tlab(hs_tlab_buffer_t* tlab, uint32_t thread_id,
                                    size_t requested_bytes, uint32_t socket_id = UINT32_MAX);

    // Dynamically computes elastic TLAB size for thread
    RJ_API size_t compute_elastic_tlab_size(uint32_t thread_id, uint32_t socket_id,
                                            size_t requested_bytes, size_t waste_bytes);

    // Socket inspection & statistics
    size_t   socket_count() const noexcept { return _socket_count; }
    size_t   get_socket_used_bytes(uint32_t socket_id) const noexcept;
    size_t   get_total_used_bytes() const noexcept;
    uint64_t total_allocations() const noexcept { return _total_numa_allocations.load(std::memory_order_relaxed); }
    uint64_t cross_socket_steals() const noexcept { return _total_cross_socket_steals.load(std::memory_order_relaxed); }
    uint64_t os_chunk_expansions() const noexcept { return _total_os_chunk_expansions.load(std::memory_order_relaxed); }
    uint64_t elastic_expansions() const noexcept { return _total_elastic_expansions.load(std::memory_order_relaxed); }
    uint64_t elastic_shrinks() const noexcept { return _total_elastic_shrinks.load(std::memory_order_relaxed); }
};

extern RedjetNUMAAllocator g_redjet_numa_allocator;

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETNUMAALLOCATOR_HPP
