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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_HOTFIELD_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_HOTFIELD_HPP

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
/* 1. HOTFIELD ORDER-OF-HEIGHT (OOH) DATA STRUCTURES                         */
/* ========================================================================= */

struct alignas(64) RedjetHotfieldSlot {
    void*               obj_ptr{ nullptr };
    size_t              footprint_bytes{ 0 };
    uint32_t            height_tier{ 0 };
    uint32_t            access_heat{ 100 };
    uint64_t            last_access_tsc{ 0 };
    RedjetHotfieldSlot* next{ nullptr };
};

struct RedjetHotfieldHeightTier {
    size_t               min_bytes{ 0 };
    size_t               max_bytes{ 0 };
    RedjetHotfieldSlot*  head{ nullptr };
    std::atomic<size_t>  slot_count{ 0 };
    std::atomic<size_t>  total_bytes{ 0 };
    hs_ticket_lock_t     tier_lock;
};

/* ========================================================================= */
/* 2. HOTFIELD VFS NODE                                                      */
/* ========================================================================= */

class RedjetVFSNode_Hotfield : public RedjetVFSNode {
public:
    static constexpr size_t NUM_HEIGHT_TIERS = 12;

private:
    RedjetHotfieldHeightTier _tiers[NUM_HEIGHT_TIERS];
    std::atomic<uint64_t>    _total_inserts{ 0 };
    std::atomic<uint64_t>    _total_removals{ 0 };
    std::atomic<uint64_t>    _total_heat_touches{ 0 };

    size_t find_tier_for_size(size_t bytes) const noexcept;

public:
    RedjetVFSNode_Hotfield(const char* name = "hotfield",
                           const char* full_path = "/heap/on_heap/runway/motherline/egg/young/hotfield",
                           RedjetVFSNode* parent = nullptr,
                           size_t quota_bytes = 0);
    virtual ~RedjetVFSNode_Hotfield();

    // Inserts an object into its sorted height tier
    RJ_API bool insert_object(void* obj_ptr, size_t footprint, uint32_t initial_heat = 100);

    // Removes an object from its height tier
    RJ_API bool remove_object(void* obj_ptr, size_t footprint);

    // Increments access heat on a hotfield object
    RJ_API void touch_object(void* obj_ptr, size_t footprint) noexcept;

    // Re-sorts and compacts objects within all height tiers
    RJ_API void rebalance_and_sort_tiers();

    // Searches for a best-fitting slot in the closest height tier
    RJ_API void* find_best_fit_slot(size_t needed_bytes, size_t* out_actual_size = nullptr);

    // Inspection
    size_t tier_count() const noexcept { return NUM_HEIGHT_TIERS; }
    void   get_tier_stats(size_t tier_idx, size_t* out_slots, size_t* out_bytes) const noexcept;
    uint64_t total_inserts() const noexcept { return _total_inserts.load(std::memory_order_relaxed); }
    uint64_t total_removals() const noexcept { return _total_removals.load(std::memory_order_relaxed); }
    uint64_t total_touches() const noexcept { return _total_heat_touches.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_HOTFIELD_HPP
