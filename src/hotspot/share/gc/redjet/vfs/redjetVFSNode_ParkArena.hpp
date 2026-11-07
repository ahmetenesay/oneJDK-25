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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_PARKARENA_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_PARKARENA_HPP

#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include <atomic>
#include <cstdint>
#include <cstddef>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// Forward declarations
class RedjetVFSNode_Hotfield;
class RedjetRunway;

/* ========================================================================= */
/* 1. PARK PROVENANCE DATA STRUCTURES                                        */
/* ========================================================================= */

enum class RedjetParkOriginBranch : uint32_t {
    ORIGIN_BRANCH_UNKNOWN    = 0,
    ORIGIN_BRANCH_RUNWAY     = 1,
    ORIGIN_BRANCH_HOTFIELD   = 2,
    ORIGIN_BRANCH_MOTHERLINE = 3,
    ORIGIN_BRANCH_TBB_ARENA  = 4,
    ORIGIN_BRANCH_PITSTOP    = 5
};

struct alignas(64) RedjetParkProvenanceHeader {
    uint32_t                magic{ 0x5041524BU }; // "PARK" (0x5041524B)
    RedjetParkOriginBranch  origin_branch{ RedjetParkOriginBranch::ORIGIN_BRANCH_HOTFIELD };
    uint32_t                origin_room_tier{ 0 }; // Room or OOH height tier (0..11)
    uint32_t                survival_epoch{ 0 };   // Epoch when object was parked
    uint64_t                entry_tsc{ 0 };        // TSC timestamp upon entering park_arena
    void*                   original_slab_ptr{ nullptr }; // Original slab pointer
    uint32_t                native_thread_id{ 0 }; // Parking native thread id
    uint32_t                flags{ 0 };

    void*                   original_slot_addr{ nullptr }; // Original slot in hotfield/runway
    RedjetEmptyBox*         emptybox_tombstone{ nullptr }; // Tombstone reservation in origin slot
    void*                   parked_object_ptr{ nullptr };  // Pointer to object residing in park_arena
    size_t                  object_size{ 0 };              // Size of the object in bytes
    char                    origin_path[128]{ 0 };         // Origin VFS branch full path
    RedjetEmptyBox          embedded_emptybox{};          // Embedded descriptor for small slots (< 64 bytes)

    bool isValid() const noexcept {
        return (magic == 0x5041524BU && parked_object_ptr != nullptr);
    }

    // Places and initializes EmptyBox tombstone in the original slot
    RJ_API bool place_tombstone_guard(uint64_t timeout_cycles = 0) noexcept;

    // Return logic:
    // 1. Returns to original slot via EmptyBox reservation
    RJ_API bool return_to_original_slot() noexcept;

    // 2. Returns to Hotfield Order-Of-Height (OOH) tier
    RJ_API bool return_to_ooh_tier(RedjetVFSNode_Hotfield* hotfield) noexcept;

    // 3. Fallback return to Runway allocator
    RJ_API bool return_to_runway(RedjetRunway* runway) noexcept;

    // 4. Universal autonomous return dispatch
    RJ_API bool dispatch_return(RedjetVFSNode_Hotfield* hotfield = nullptr, RedjetRunway* runway = nullptr) noexcept;
};

/* ========================================================================= */
/* 2. PINNED NATIVE RECORD WITH PROVENANCE                                   */
/* ========================================================================= */

struct alignas(64) RedjetPinnedNativeRecord {
    void*                      java_oop{ nullptr };
    void*                      native_pinned_ptr{ nullptr };
    size_t                     buffer_size{ 0 };
    uint32_t                   pinning_thread_id{ 0 };
    uint64_t                   pin_timestamp_ns{ 0 };
    uint64_t                   lease_timeout_ns{ 0 }; // 0 = permanent until explicit unpin
    std::atomic<uint32_t>      pin_ref_count{ 0 };
    std::atomic<bool>          is_pinned{ false };
    RedjetParkProvenanceHeader provenance{};
    RedjetPinnedNativeRecord*  next{ nullptr };

    bool is_expired(uint64_t now_ns) const noexcept {
        return (lease_timeout_ns > 0 && now_ns >= lease_timeout_ns);
    }
};

/* ========================================================================= */
/* 3. PARK ARENA VFS NODE                                                    */
/* ========================================================================= */

class RedjetVFSNode_ParkArena : public RedjetVFSNode {
public:
    static constexpr size_t PINNED_BUCKET_COUNT = 256;

private:
    RedjetPinnedNativeRecord* _hash_buckets[PINNED_BUCKET_COUNT];
    hs_ticket_lock_t          _bucket_locks[PINNED_BUCKET_COUNT];

    std::atomic<uint32_t>     _total_pinned_objects{ 0 };
    std::atomic<size_t>       _total_pinned_bytes{ 0 };
    std::atomic<uint64_t>     _total_pin_operations{ 0 };
    std::atomic<uint64_t>     _total_unpin_operations{ 0 };
    std::atomic<uint64_t>     _total_expired_unpins{ 0 };

    std::atomic<uint64_t>     _returns_to_slot{ 0 };
    std::atomic<uint64_t>     _returns_to_ooh{ 0 };
    std::atomic<uint64_t>     _returns_to_runway{ 0 };

    inline size_t compute_bucket(const void* ptr) const noexcept {
        uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
        return ((v >> 4) ^ (v >> 12) ^ (v >> 20)) % PINNED_BUCKET_COUNT;
    }

public:
    RedjetVFSNode_ParkArena(RedjetVFSNode* parent = nullptr, size_t quota_bytes = 0);
    virtual ~RedjetVFSNode_ParkArena();

    // Standard Pin
    RJ_API bool pin_java_object(void* obj_ptr, size_t size_bytes, uint32_t thread_id, uint64_t lease_ns = 0);

    // Provenance-Aware Pin (creates EmptyBox tombstone in original slot and records origin)
    RJ_API bool park_object_with_provenance(void* obj_ptr, size_t size_bytes, uint32_t thread_id,
                                           const RedjetParkProvenanceHeader& prov, uint64_t lease_ns = 0);

    // Unpins an object and decrements ref_count
    RJ_API bool unpin_java_object(void* obj_ptr, uint32_t thread_id);

    // Unpins and executes autonomous return logic back to original slot/EmptyBox or OOH tier
    RJ_API bool unpin_and_return_object(void* obj_ptr, uint32_t thread_id,
                                        RedjetVFSNode_Hotfield* hotfield = nullptr,
                                        RedjetRunway* runway = nullptr);

    // Checks if object is currently pinned in Park Arena
    RJ_API bool is_pinned(const void* obj_ptr) const noexcept;

    // Retrieves pinned native pointer for a Java oop
    RJ_API void* get_native_pinned_ptr(const void* obj_ptr) const noexcept;

    // Retrieves provenance header pointer for a parked oop
    RJ_API RedjetParkProvenanceHeader* get_provenance(const void* obj_ptr) const noexcept;

    // Sweeps and unpins any expired leases (e.g. abandoned JNI leases)
    RJ_API size_t sweep_expired_leases(uint64_t now_ns = 0,
                                      RedjetVFSNode_Hotfield* hotfield = nullptr,
                                      RedjetRunway* runway = nullptr);

    // Statistics
    uint32_t pinned_object_count() const noexcept { return _total_pinned_objects.load(std::memory_order_relaxed); }
    size_t   pinned_bytes() const noexcept { return _total_pinned_bytes.load(std::memory_order_relaxed); }
    uint64_t total_pins() const noexcept { return _total_pin_operations.load(std::memory_order_relaxed); }
    uint64_t total_unpins() const noexcept { return _total_unpin_operations.load(std::memory_order_relaxed); }
    uint64_t expired_unpins() const noexcept { return _total_expired_unpins.load(std::memory_order_relaxed); }
    uint64_t returns_to_slot() const noexcept { return _returns_to_slot.load(std::memory_order_relaxed); }
    uint64_t returns_to_ooh() const noexcept { return _returns_to_ooh.load(std::memory_order_relaxed); }
    uint64_t returns_to_runway() const noexcept { return _returns_to_runway.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_PARKARENA_HPP
