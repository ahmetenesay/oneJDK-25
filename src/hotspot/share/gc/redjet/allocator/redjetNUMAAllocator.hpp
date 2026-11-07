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
// Mega-Server Topology & Total Silicon Exploitation Architecture
// 1. AMD Threadripper PRO 9995WX: 12 CCDs, 96 Cores/192 Threads, 8-Channel DDR5
// 2. Intel Xeon 6980P: 128 Redwood Cove P-Cores, Sub-NUMA SNC-3/SNC-4, 12-Channel MRDIMM
// 3. Ampere Altra Max: 128 Neoverse N1 Cores on 2D Mesh Interconnect
// ============================================================================

enum class RedjetMegaServerClass : uint8_t {
    GENERIC_NUMA           = 0,
    AMD_THREADRIPPER_PRO   = 1, // 12 CCDs, Infinity Fabric inter-die interconnect
    INTEL_XEON_6980P_SNC3  = 2, // 128 P-Cores, SNC-3 sub-NUMA clustering, 12-ch MRDIMM
    AMPERE_ALTRA_MAX_MESH  = 3  // 128 Cores, 2D Mesh topology
};

// ============================================================================
// 1. CCD / CCX LOCAL RUNWAY (Per-CCD L3 Cache & Infinity Fabric Isolation)
// ============================================================================

struct alignas(64) RedjetCCDRunway {
    static constexpr size_t NUM_CLASSES = 16;

    uint32_t            ccd_id{ 0 };
    uint32_t            parent_socket_id{ 0 };
    uint64_t            core_mask{ 0 };
    uint8_t*            ccd_heap_base{ nullptr };
    size_t              ccd_capacity{ 0 };
    std::atomic<size_t> ccd_bump_offset{ 0 };
    hs_ticket_lock_t    ccd_lock;

    // Per-CCD slab classes for local L3 cache reuse
    hs_slab_class_t     local_slabs[NUM_CLASSES];

    // CCD-level Telemetry
    std::atomic<uint64_t> local_allocations{ 0 };
    std::atomic<uint64_t> cross_ccd_steals{ 0 };
    std::atomic<size_t>   allocated_bytes{ 0 };

    RedjetCCDRunway() = default;
    ~RedjetCCDRunway() = default;

    bool init(uint32_t ccd, uint32_t socket, uint64_t mask, size_t capacity_bytes, uint8_t* base_ptr);
    void* bump_alloc(size_t bytes, size_t align = 64);
    void* slab_alloc(size_t bytes);
    void  slab_free(void* ptr, size_t bytes);
    size_t get_used_bytes() const noexcept;
};

// ============================================================================
// 2. MULTI-CHANNEL MEMORY STRIPING CONTROLLER (8-ch DDR5 / 12-ch MRDIMM)
// ============================================================================

struct RedjetMemoryStriping {
    static constexpr size_t STRIPE_BLOCK_SIZE = 64 * 1024; // 64 KB per memory channel
    static constexpr size_t MAX_CHANNELS      = 16;        // Up to 16 physical channels

    uint32_t active_channels{ 8 }; // 8 for Threadripper, 12 for Xeon 6980P
    std::atomic<uint64_t> total_striped_bytes{ 0 };
    std::atomic<uint64_t> total_striped_allocations{ 0 };

    void* allocate_striped(size_t total_bytes, uint32_t socket_id);
    void  free_striped(void* ptr, size_t total_bytes);
};

// ============================================================================
// 3. MESH TOPOLOGICAL DISTANCE CALCULATOR
// ============================================================================

struct RedjetMeshDistance {
    // Distance: 0=SMT, 1=Same CCD/L3, 2=Same SNC/Socket, 3=Remote CCD, 4=Remote Socket
    static uint32_t compute_distance(uint32_t src_core, uint32_t dst_core, RedjetMegaServerClass server_class);
};

// ============================================================================
// 4. REDJET SOCKET RUNWAY (with CCX/CCD Local Runways)
// ============================================================================

struct alignas(64) RedjetSocketRunway {
    static constexpr size_t NUM_CLASSES          = 32;
    static constexpr size_t MAX_CCDS_PER_SOCKET  = 16;

    uint32_t            socket_id{ 0 };
    uint8_t*            socket_heap_base{ nullptr };
    size_t              socket_capacity{ 0 };
    std::atomic<size_t> socket_bump_offset{ 0 };
    hs_ticket_lock_t    socket_lock;

    // Per-socket slab classes (eliminates cross-node UPI/QPI/Infinity Fabric traffic)
    hs_slab_class_t     slab_classes[NUM_CLASSES];

    // CCD/CCX local runways (12 CCDs on Threadripper PRO 9995WX, 3 SNCs on Xeon 6980P)
    RedjetCCDRunway     ccd_runways[MAX_CCDS_PER_SOCKET];
    uint32_t            ccd_count{ 0 };

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

    bool init(uint32_t id, size_t capacity_bytes, uint8_t* base_ptr = nullptr, uint32_t num_ccds = 0);
    void destroy();

    void* bump_alloc(size_t bytes, size_t align = 64);
    void* slab_alloc(size_t bytes);
    void* allocate_from_chained_chunks(size_t bytes, size_t align = 64);
    void  slab_free(void* ptr, size_t bytes);
    size_t get_used_bytes() const noexcept;

    RedjetCCDRunway* get_ccd_runway_for_core(uint32_t core_id);
};

/* ========================================================================= */
/* 5. ELASTIC TLAB TUNER STATE                                               */
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
/* 6. REDJET NUMA ALLOCATOR NEXUS                                            */
/* ========================================================================= */

class RedjetNUMAAllocator {
public:
    static constexpr size_t MAX_NUMA_SOCKETS = 64; // Supports 12 CCDs and SNC clusters

private:
    RedjetSocketRunway      _sockets[MAX_NUMA_SOCKETS];
    size_t                  _socket_count{ 1 };
    size_t                  _per_socket_capacity{ 0 };
    std::atomic<bool>       _initialized{ false };
    hs_ticket_lock_t        _global_lock;

    RedjetMegaServerClass   _server_class{ RedjetMegaServerClass::GENERIC_NUMA };
    RedjetMemoryStriping    _striping;

    // Elastic TLAB tracking table
    static constexpr size_t MAX_TRACKED_THREADS = 1024;
    RedjetThreadTLABProfile _thread_profiles[MAX_TRACKED_THREADS];
    hs_ticket_lock_t        _profile_lock;

    std::atomic<uint64_t>   _total_numa_allocations{ 0 };
    std::atomic<uint64_t>   _total_cross_socket_steals{ 0 };
    std::atomic<uint64_t>   _total_cross_ccd_steals{ 0 };
    std::atomic<uint64_t>   _total_os_chunk_expansions{ 0 };
    std::atomic<uint64_t>   _total_elastic_expansions{ 0 };
    std::atomic<uint64_t>   _total_elastic_shrinks{ 0 };

    RedjetThreadTLABProfile* get_or_create_thread_profile(uint32_t thread_id, uint32_t socket_id);
    void detect_mega_server_topology();

public:
    RedjetNUMAAllocator();
    ~RedjetNUMAAllocator();

    // Initializes NUMA multi-tier runway with detected host sockets and CCD topology
    RJ_API bool init(size_t socket_count = 0, size_t per_socket_capacity = 256ULL * 1024 * 1024);
    RJ_API void destroy();

    // Resolves current thread's NUMA node affinity
    RJ_API uint32_t current_numa_node() const noexcept;

    // Multi-Tier Allocation Pipeline:
    // Tier 0: CCX/CCD local runway (L3 cache local, 0-hop)
    // Tier 1: Per-socket runway bump allocator
    // Tier 2: Per-socket runway slab class freelist
    // Tier 3: Cross-CCD / nearest neighbor stealing
    // Tier 4: OS chained chunk expansion
    RJ_API void* allocate(size_t bytes, uint32_t socket_id = UINT32_MAX, size_t align = 64);

    // CCD-Local Runway Allocation (Threadripper PRO 9995WX / Xeon 6980P SNC)
    RJ_API void* allocate_from_ccd(size_t bytes, uint32_t core_id, size_t align = 64);

    // Multi-Channel Memory Striping Allocation (interleaved across 8-ch / 12-ch controllers)
    RJ_API void* allocate_striped(size_t bytes, uint32_t socket_id = UINT32_MAX);
    RJ_API void  free_striped(void* ptr, size_t bytes);

    // Deallocates memory to appropriate socket freelist
    RJ_API void deallocate(void* ptr, size_t bytes, uint32_t socket_id);

    // Elastic TLAB Refill: computes dynamic size based on allocation rate and waste ratio
    RJ_API bool refill_elastic_tlab(hs_tlab_buffer_t* tlab, uint32_t thread_id,
                                    size_t requested_bytes, uint32_t socket_id = UINT32_MAX);

    // Dynamically computes elastic TLAB size for thread
    RJ_API size_t compute_elastic_tlab_size(uint32_t thread_id, uint32_t socket_id,
                                            size_t requested_bytes, size_t waste_bytes);

    // Mega-server inspection
    RedjetMegaServerClass mega_server_class() const noexcept { return _server_class; }
    RedjetMemoryStriping& memory_striping() noexcept { return _striping; }

    // Socket inspection & statistics
    size_t   socket_count() const noexcept { return _socket_count; }
    size_t   get_socket_used_bytes(uint32_t socket_id) const noexcept;
    size_t   get_total_used_bytes() const noexcept;
    uint64_t total_allocations() const noexcept { return _total_numa_allocations.load(std::memory_order_relaxed); }
    uint64_t cross_socket_steals() const noexcept { return _total_cross_socket_steals.load(std::memory_order_relaxed); }
    uint64_t cross_ccd_steals() const noexcept { return _total_cross_ccd_steals.load(std::memory_order_relaxed); }
    uint64_t os_chunk_expansions() const noexcept { return _total_os_chunk_expansions.load(std::memory_order_relaxed); }
    uint64_t elastic_expansions() const noexcept { return _total_elastic_expansions.load(std::memory_order_relaxed); }
    uint64_t elastic_shrinks() const noexcept { return _total_elastic_shrinks.load(std::memory_order_relaxed); }
};

extern RedjetNUMAAllocator g_redjet_numa_allocator;

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETNUMAALLOCATOR_HPP
