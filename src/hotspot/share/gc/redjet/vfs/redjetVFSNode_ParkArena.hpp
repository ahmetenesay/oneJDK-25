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

/* ========================================================================= */
/* 1. PINNED NATIVE RECORD                                                   */
/* ========================================================================= */

struct alignas(64) RedjetPinnedNativeRecord {
    void*                 java_oop{ nullptr };
    void*                 native_pinned_ptr{ nullptr };
    size_t                buffer_size{ 0 };
    uint32_t              pinning_thread_id{ 0 };
    uint64_t              pin_timestamp_ns{ 0 };
    uint64_t              lease_timeout_ns{ 0 }; // 0 = permanent until explicit unpin
    std::atomic<uint32_t> pin_ref_count{ 0 };
    std::atomic<bool>     is_pinned{ false };
    RedjetPinnedNativeRecord* next{ nullptr };

    bool is_expired(uint64_t now_ns) const noexcept {
        return (lease_timeout_ns > 0 && now_ns >= lease_timeout_ns);
    }
};

/* ========================================================================= */
/* 2. PARK ARENA VFS NODE                                                    */
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

    inline size_t compute_bucket(const void* ptr) const noexcept {
        uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
        return ((v >> 4) ^ (v >> 12) ^ (v >> 20)) % PINNED_BUCKET_COUNT;
    }

public:
    RedjetVFSNode_ParkArena(RedjetVFSNode* parent = nullptr, size_t quota_bytes = 0);
    virtual ~RedjetVFSNode_ParkArena();

    // Pins a Java object transition buffer to native execution (is_pinned = 1)
    RJ_API bool pin_java_object(void* obj_ptr, size_t size_bytes, uint32_t thread_id, uint64_t lease_ns = 0);

    // Unpins a Java object, decrementing ref_count. When 0, is_pinned is set to 0.
    RJ_API bool unpin_java_object(void* obj_ptr, uint32_t thread_id);

    // Checks if the given object is currently pinned in Park Arena
    RJ_API bool is_pinned(const void* obj_ptr) const noexcept;

    // Retrieves pinned native pointer for a Java oop
    RJ_API void* get_native_pinned_ptr(const void* obj_ptr) const noexcept;

    // Sweeps and unpins any expired leases (e.g. abandoned JNI leases)
    RJ_API size_t sweep_expired_leases(uint64_t now_ns = 0);

    // Statistics
    uint32_t pinned_object_count() const noexcept { return _total_pinned_objects.load(std::memory_order_relaxed); }
    size_t   pinned_bytes() const noexcept { return _total_pinned_bytes.load(std::memory_order_relaxed); }
    uint64_t total_pins() const noexcept { return _total_pin_operations.load(std::memory_order_relaxed); }
    uint64_t total_unpins() const noexcept { return _total_unpin_operations.load(std::memory_order_relaxed); }
    uint64_t expired_unpins() const noexcept { return _total_expired_unpins.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_PARKARENA_HPP
