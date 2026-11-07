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

#include "gc/redjet/vfs/redjetVFSNode_Hotfield.hpp"
#include <new>

namespace {
static const size_t TIER_BOUNDS[RedjetVFSNode_Hotfield::NUM_HEIGHT_TIERS][2] = {
    { 0,      64 },
    { 65,     128 },
    { 129,    256 },
    { 257,    512 },
    { 513,    1024 },
    { 1025,   2048 },
    { 2049,   4096 },
    { 4097,   8192 },
    { 8193,   16384 },
    { 16385,  32768 },
    { 32769,  65536 },
    { 65537,  262144 }
};
} // namespace

RedjetVFSNode_Hotfield::RedjetVFSNode_Hotfield(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes) :
    RedjetVFSNode(name, full_path, parent, quota_bytes) {

    for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
        _tiers[i].min_bytes = TIER_BOUNDS[i][0];
        _tiers[i].max_bytes = TIER_BOUNDS[i][1];
        _tiers[i].head = nullptr;
        _tiers[i].slot_count.store(0, std::memory_order_relaxed);
        _tiers[i].total_bytes.store(0, std::memory_order_relaxed);
    }
}

RedjetVFSNode_Hotfield::~RedjetVFSNode_Hotfield() {
    for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
        hs_ticket_guard_t guard(_tiers[i].tier_lock);
        RedjetHotfieldSlot* cur = _tiers[i].head;
        while (cur != nullptr) {
            RedjetHotfieldSlot* next = cur->next;
            delete cur;
            cur = next;
        }
        _tiers[i].head = nullptr;
        _tiers[i].slot_count.store(0, std::memory_order_relaxed);
        _tiers[i].total_bytes.store(0, std::memory_order_relaxed);
    }
}

size_t RedjetVFSNode_Hotfield::find_tier_for_size(size_t bytes) const noexcept {
    for (size_t i = 0; i < NUM_HEIGHT_TIERS; ++i) {
        if (bytes <= _tiers[i].max_bytes) {
            return i;
        }
    }
    return NUM_HEIGHT_TIERS - 1;
}

bool RedjetVFSNode_Hotfield::insert_object(void* obj_ptr, size_t footprint, uint32_t initial_heat) {
    if (!obj_ptr || footprint == 0) return false;

    size_t tier_idx = find_tier_for_size(footprint);
    RedjetHotfieldHeightTier& tier = _tiers[tier_idx];

    auto* slot = new (std::nothrow) RedjetHotfieldSlot();
    if (!slot) return false;

    slot->obj_ptr = obj_ptr;
    slot->footprint_bytes = footprint;
    slot->height_tier = static_cast<uint32_t>(tier_idx);
    slot->access_heat = initial_heat;
    slot->last_access_tsc = sacculus_rdtsc();

    hs_ticket_guard_t guard(tier.tier_lock);
    // Insert sorted by access_heat descending
    RedjetHotfieldSlot* prev = nullptr;
    RedjetHotfieldSlot* cur = tier.head;
    while (cur != nullptr && cur->access_heat >= initial_heat) {
        prev = cur;
        cur = cur->next;
    }

    if (prev != nullptr) {
        slot->next = prev->next;
        prev->next = slot;
    } else {
        slot->next = tier.head;
        tier.head = slot;
    }

    tier.slot_count.fetch_add(1, std::memory_order_relaxed);
    tier.total_bytes.fetch_add(footprint, std::memory_order_relaxed);
    _total_inserts.fetch_add(1, std::memory_order_relaxed);
    add_used(footprint);

    return true;
}

bool RedjetVFSNode_Hotfield::remove_object(void* obj_ptr, size_t footprint) {
    if (!obj_ptr) return false;

    size_t tier_idx = find_tier_for_size(footprint);
    RedjetHotfieldHeightTier& tier = _tiers[tier_idx];

    hs_ticket_guard_t guard(tier.tier_lock);
    RedjetHotfieldSlot* prev = nullptr;
    RedjetHotfieldSlot* cur = tier.head;

    while (cur != nullptr) {
        if (cur->obj_ptr == obj_ptr) {
            if (prev != nullptr) {
                prev->next = cur->next;
            } else {
                tier.head = cur->next;
            }

            size_t fp = cur->footprint_bytes;
            delete cur;

            if (tier.slot_count.load(std::memory_order_relaxed) > 0) {
                tier.slot_count.fetch_sub(1, std::memory_order_relaxed);
            }
            if (tier.total_bytes.load(std::memory_order_relaxed) >= fp) {
                tier.total_bytes.fetch_sub(fp, std::memory_order_relaxed);
            }
            sub_used(fp);
            _total_removals.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

void RedjetVFSNode_Hotfield::touch_object(void* obj_ptr, size_t footprint) noexcept {
    if (!obj_ptr) return;
    size_t tier_idx = find_tier_for_size(footprint);
    RedjetHotfieldHeightTier& tier = _tiers[tier_idx];

    hs_ticket_guard_t guard(tier.tier_lock);
    RedjetHotfieldSlot* cur = tier.head;
    while (cur != nullptr) {
        if (cur->obj_ptr == obj_ptr) {
            cur->access_heat++;
            cur->last_access_tsc = sacculus_rdtsc();
            _total_heat_touches.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        cur = cur->next;
    }
}

void RedjetVFSNode_Hotfield::rebalance_and_sort_tiers() {
    for (size_t t = 0; t < NUM_HEIGHT_TIERS; ++t) {
        RedjetHotfieldHeightTier& tier = _tiers[t];
        hs_ticket_guard_t guard(tier.tier_lock);

        if (!tier.head || !tier.head->next) continue;

        // Insertion sort linked list by access_heat descending
        RedjetHotfieldSlot* sorted = nullptr;
        RedjetHotfieldSlot* cur = tier.head;

        while (cur != nullptr) {
            RedjetHotfieldSlot* next = cur->next;

            if (!sorted || sorted->access_heat < cur->access_heat) {
                cur->next = sorted;
                sorted = cur;
            } else {
                RedjetHotfieldSlot* s_cur = sorted;
                while (s_cur->next != nullptr && s_cur->next->access_heat >= cur->access_heat) {
                    s_cur = s_cur->next;
                }
                cur->next = s_cur->next;
                s_cur->next = cur;
            }
            cur = next;
        }
        tier.head = sorted;
    }
}

void* RedjetVFSNode_Hotfield::find_best_fit_slot(size_t needed_bytes, size_t* out_actual_size) {
    size_t start_tier = find_tier_for_size(needed_bytes);

    for (size_t t = start_tier; t < NUM_HEIGHT_TIERS; ++t) {
        RedjetHotfieldHeightTier& tier = _tiers[t];
        hs_ticket_guard_t guard(tier.tier_lock);

        RedjetHotfieldSlot* cur = tier.head;
        while (cur != nullptr) {
            if (cur->footprint_bytes >= needed_bytes) {
                if (out_actual_size) *out_actual_size = cur->footprint_bytes;
                return cur->obj_ptr;
            }
            cur = cur->next;
        }
    }
    if (out_actual_size) *out_actual_size = 0;
    return nullptr;
}

void RedjetVFSNode_Hotfield::get_tier_stats(size_t tier_idx, size_t* out_slots, size_t* out_bytes) const noexcept {
    if (tier_idx >= NUM_HEIGHT_TIERS) {
        if (out_slots) *out_slots = 0;
        if (out_bytes) *out_bytes = 0;
        return;
    }
    if (out_slots) *out_slots = _tiers[tier_idx].slot_count.load(std::memory_order_relaxed);
    if (out_bytes) *out_bytes = _tiers[tier_idx].total_bytes.load(std::memory_order_relaxed);
}
