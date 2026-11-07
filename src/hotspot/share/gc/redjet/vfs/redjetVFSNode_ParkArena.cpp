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

#include "gc/redjet/vfs/redjetVFSNode_ParkArena.hpp"
#include "gc/redjet/vfs/redjetVFSNode_Hotfield.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetGC.hpp"
#include <new>
#include <cstring>

/* ========================================================================= */
/* 1. REDJET PARK PROVENANCE HEADER METHODS                                  */
/* ========================================================================= */

bool RedjetParkProvenanceHeader::place_tombstone_guard(uint64_t timeout_cycles) noexcept {
    if (!original_slot_addr || object_size == 0) return false;

    RedjetEmptyBox* eb = RedjetEmptyBox::place_tombstone(
        original_slot_addr, object_size, parked_object_ptr,
        native_thread_id, timeout_cycles, origin_room_tier, &embedded_emptybox);

    if (eb != nullptr) {
        emptybox_tombstone = eb;
        RedjetEmptyBoxManager::record_reservation(object_size);
        return true;
    }
    return false;
}

bool RedjetParkProvenanceHeader::return_to_original_slot() noexcept {
    if (!isValid() || !emptybox_tombstone || !original_slot_addr) {
        return false;
    }

    // Verify tombstone is valid and has not expired
    if (!emptybox_tombstone->isValid() || emptybox_tombstone->is_timeout()) {
        RedjetEmptyBoxManager::record_timeout();
        return false;
    }

    void* parked = nullptr;
    if (emptybox_tombstone->reclaim_slot(native_thread_id, &parked)) {
        if (parked_object_ptr != original_slot_addr && object_size > 0) {
            std::memcpy(original_slot_addr, parked_object_ptr, object_size);
        }
        // Do NOT call release_reservation() here: the slot is now occupied by the restored live object!
        RedjetEmptyBoxManager::record_claim(object_size);
        return true;
    }
    return false;
}

bool RedjetParkProvenanceHeader::return_to_ooh_tier(RedjetVFSNode_Hotfield* hotfield) noexcept {
    if (!isValid() || !hotfield || !parked_object_ptr || object_size == 0) {
        return false;
    }

    // If tombstone exists, cancel reservation and clear magic word since object moves to OOH tier
    if (emptybox_tombstone && emptybox_tombstone->isValid()) {
        emptybox_tombstone->release_reservation();
    }

    return hotfield->insert_object(parked_object_ptr, object_size, 100);
}

bool RedjetParkProvenanceHeader::return_to_runway(RedjetRunway* runway) noexcept {
    if (!isValid() || !runway || !parked_object_ptr || object_size == 0) {
        return false;
    }

    if (emptybox_tombstone && emptybox_tombstone->isValid()) {
        emptybox_tombstone->release_reservation();
    }

    void* new_cell = runway->bump_alloc(object_size);
    if (!new_cell) {
        new_cell = runway->allocate_cell(object_size);
    }
    if (new_cell) {
        std::memcpy(new_cell, parked_object_ptr, object_size);
        return true;
    }
    return false;
}

bool RedjetParkProvenanceHeader::dispatch_return(RedjetVFSNode_Hotfield* hotfield, RedjetRunway* runway) noexcept {
    // 1. Try returning to the original reserved slot
    if (return_to_original_slot()) {
        return true;
    }

    // 2. Try returning to OOH height tier in Hotfield
    if (hotfield != nullptr && return_to_ooh_tier(hotfield)) {
        return true;
    }

    // 3. Fallback to runway allocator
    if (runway != nullptr && return_to_runway(runway)) {
        return true;
    }

    // 4. Autonomous fallback to default GC runway if pointers were omitted
    RedjetGC* gc = hs_get_default_gc();
    if (gc != nullptr && gc->runway() != nullptr) {
        if (return_to_runway(gc->runway())) {
            return true;
        }
    }

    return false;
}

/* ========================================================================= */
/* 2. REDJET PARK ARENA VFS NODE IMPLEMENTATION                              */
/* ========================================================================= */

RedjetVFSNode_ParkArena::RedjetVFSNode_ParkArena(RedjetVFSNode* parent, size_t quota_bytes) :
    RedjetVFSNode("park_arena", "/heap/on_heap/park_arena", parent, quota_bytes) {

    for (size_t i = 0; i < PINNED_BUCKET_COUNT; ++i) {
        _hash_buckets[i] = nullptr;
    }
}

RedjetVFSNode_ParkArena::~RedjetVFSNode_ParkArena() {
    for (size_t i = 0; i < PINNED_BUCKET_COUNT; ++i) {
        hs_ticket_guard_t guard(_bucket_locks[i]);
        RedjetPinnedNativeRecord* cur = _hash_buckets[i];
        while (cur != nullptr) {
            RedjetPinnedNativeRecord* next = cur->next;
            delete cur;
            cur = next;
        }
        _hash_buckets[i] = nullptr;
    }
}

bool RedjetVFSNode_ParkArena::pin_java_object(void* obj_ptr, size_t size_bytes, uint32_t thread_id, uint64_t lease_ns) {
    RedjetParkProvenanceHeader default_prov;
    default_prov.magic = 0x5041524BU;
    default_prov.origin_branch = RedjetParkOriginBranch::ORIGIN_BRANCH_RUNWAY;
    default_prov.origin_room_tier = 0;
    default_prov.survival_epoch = 0;
    default_prov.entry_tsc = sacculus_rdtsc();
    default_prov.original_slab_ptr = nullptr;
    default_prov.native_thread_id = thread_id;
    default_prov.flags = 0;
    default_prov.original_slot_addr = obj_ptr;
    default_prov.emptybox_tombstone = nullptr;
    default_prov.parked_object_ptr = obj_ptr;
    default_prov.object_size = size_bytes;
    strncpy(default_prov.origin_path, "/heap/on_heap/runway", sizeof(default_prov.origin_path) - 1);

    return park_object_with_provenance(obj_ptr, size_bytes, thread_id, default_prov, lease_ns);
}

bool RedjetVFSNode_ParkArena::park_object_with_provenance(
    void* obj_ptr, size_t size_bytes, uint32_t thread_id,
    const RedjetParkProvenanceHeader& prov, uint64_t lease_ns) {

    if (!obj_ptr) return false;

    size_t b_idx = compute_bucket(obj_ptr);
    hs_ticket_guard_t guard(_bucket_locks[b_idx]);

    // Check if already registered
    RedjetPinnedNativeRecord* cur = _hash_buckets[b_idx];
    while (cur != nullptr) {
        if (cur->java_oop == obj_ptr) {
            cur->pin_ref_count.fetch_add(1, std::memory_order_relaxed);
            cur->is_pinned.store(true, std::memory_order_release);
            if (lease_ns > 0) {
                cur->lease_timeout_ns = sacculus_now_ns() + lease_ns;
            }
            _total_pin_operations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        cur = cur->next;
    }

    // Allocate new pinned record
    auto* rec = new (std::nothrow) RedjetPinnedNativeRecord();
    if (!rec) return false;

    uint64_t now = sacculus_now_ns();
    rec->java_oop = obj_ptr;
    rec->native_pinned_ptr = obj_ptr;
    rec->buffer_size = size_bytes;
    rec->pinning_thread_id = thread_id;
    rec->pin_timestamp_ns = now;
    rec->lease_timeout_ns = (lease_ns > 0) ? (now + lease_ns) : 0;
    rec->pin_ref_count.store(1, std::memory_order_relaxed);
    rec->is_pinned.store(true, std::memory_order_release);
    rec->provenance = prov;
    rec->provenance.parked_object_ptr = obj_ptr;
    rec->provenance.object_size = size_bytes;
    if (rec->provenance.emptybox_tombstone == nullptr && rec->provenance.original_slot_addr != nullptr) {
        rec->provenance.place_tombstone_guard(lease_ns > 0 ? (lease_ns * 3) : 0);
    }

    rec->next = _hash_buckets[b_idx];
    _hash_buckets[b_idx] = rec;

    _total_pinned_objects.fetch_add(1, std::memory_order_relaxed);
    _total_pinned_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    _total_pin_operations.fetch_add(1, std::memory_order_relaxed);
    add_used(size_bytes);

    hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0,
                   static_cast<uint16_t>(size_bytes / 1024), 0, 0, 0, 0,
                   "park_arena_pinned_provenance");
    return true;
}

bool RedjetVFSNode_ParkArena::unpin_java_object(void* obj_ptr, uint32_t thread_id) {
    return unpin_and_return_object(obj_ptr, thread_id, nullptr, nullptr);
}

bool RedjetVFSNode_ParkArena::unpin_and_return_object(
    void* obj_ptr, uint32_t thread_id,
    RedjetVFSNode_Hotfield* hotfield, RedjetRunway* runway) {

    if (!obj_ptr) return false;

    size_t b_idx = compute_bucket(obj_ptr);
    hs_ticket_guard_t guard(_bucket_locks[b_idx]);

    RedjetPinnedNativeRecord* prev = nullptr;
    RedjetPinnedNativeRecord* cur = _hash_buckets[b_idx];

    while (cur != nullptr) {
        if (cur->java_oop == obj_ptr) {
            uint32_t refs = cur->pin_ref_count.fetch_sub(1, std::memory_order_acq_rel);
            if (refs <= 1) {
                cur->is_pinned.store(false, std::memory_order_release);
                if (prev != nullptr) {
                    prev->next = cur->next;
                } else {
                    _hash_buckets[b_idx] = cur->next;
                }

                size_t sz = cur->buffer_size;
                if (_total_pinned_objects.load(std::memory_order_relaxed) > 0) {
                    _total_pinned_objects.fetch_sub(1, std::memory_order_relaxed);
                }
                if (_total_pinned_bytes.load(std::memory_order_relaxed) >= sz) {
                    _total_pinned_bytes.fetch_sub(sz, std::memory_order_relaxed);
                }
                sub_used(sz);

                // Execute autonomous return logic via Provenance
                RedjetParkProvenanceHeader prov = cur->provenance;
                delete cur;

                _total_unpin_operations.fetch_add(1, std::memory_order_relaxed);

                if (prov.return_to_original_slot()) {
                    _returns_to_slot.fetch_add(1, std::memory_order_relaxed);
                } else if (hotfield != nullptr && prov.return_to_ooh_tier(hotfield)) {
                    _returns_to_ooh.fetch_add(1, std::memory_order_relaxed);
                } else if (runway != nullptr && prov.return_to_runway(runway)) {
                    _returns_to_runway.fetch_add(1, std::memory_order_relaxed);
                } else if (prov.dispatch_return(hotfield, runway)) {
                    _returns_to_runway.fetch_add(1, std::memory_order_relaxed);
                }

                hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0,
                               0, 0, 0, 0, 0, "park_arena_unpinned_returned");
                return true;
            }
            _total_unpin_operations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

bool RedjetVFSNode_ParkArena::is_pinned(const void* obj_ptr) const noexcept {
    if (!obj_ptr) return false;
    size_t b_idx = compute_bucket(obj_ptr);
    hs_ticket_guard_t guard(const_cast<hs_ticket_lock_t&>(_bucket_locks[b_idx]));

    RedjetPinnedNativeRecord* cur = _hash_buckets[b_idx];
    while (cur != nullptr) {
        if (cur->java_oop == obj_ptr) {
            return cur->is_pinned.load(std::memory_order_acquire);
        }
        cur = cur->next;
    }
    return false;
}

void* RedjetVFSNode_ParkArena::get_native_pinned_ptr(const void* obj_ptr) const noexcept {
    if (!obj_ptr) return nullptr;
    size_t b_idx = compute_bucket(obj_ptr);
    hs_ticket_guard_t guard(const_cast<hs_ticket_lock_t&>(_bucket_locks[b_idx]));

    RedjetPinnedNativeRecord* cur = _hash_buckets[b_idx];
    while (cur != nullptr) {
        if (cur->java_oop == obj_ptr) {
            return cur->native_pinned_ptr;
        }
        cur = cur->next;
    }
    return nullptr;
}

RedjetParkProvenanceHeader* RedjetVFSNode_ParkArena::get_provenance(const void* obj_ptr) const noexcept {
    if (!obj_ptr) return nullptr;
    size_t b_idx = compute_bucket(obj_ptr);
    hs_ticket_guard_t guard(const_cast<hs_ticket_lock_t&>(_bucket_locks[b_idx]));

    RedjetPinnedNativeRecord* cur = _hash_buckets[b_idx];
    while (cur != nullptr) {
        if (cur->java_oop == obj_ptr) {
            return &cur->provenance;
        }
        cur = cur->next;
    }
    return nullptr;
}

size_t RedjetVFSNode_ParkArena::sweep_expired_leases(
    uint64_t now_ns, RedjetVFSNode_Hotfield* hotfield, RedjetRunway* runway) {

    if (now_ns == 0) now_ns = sacculus_now_ns();
    size_t expired_count = 0;

    for (size_t i = 0; i < PINNED_BUCKET_COUNT; ++i) {
        hs_ticket_guard_t guard(_bucket_locks[i]);
        RedjetPinnedNativeRecord* prev = nullptr;
        RedjetPinnedNativeRecord* cur = _hash_buckets[i];

        while (cur != nullptr) {
            if (cur->is_expired(now_ns)) {
                cur->is_pinned.store(false, std::memory_order_release);
                RedjetPinnedNativeRecord* to_delete = cur;

                if (prev != nullptr) {
                    prev->next = cur->next;
                } else {
                    _hash_buckets[i] = cur->next;
                }
                cur = cur->next;

                size_t sz = to_delete->buffer_size;
                if (_total_pinned_objects.load(std::memory_order_relaxed) > 0) {
                    _total_pinned_objects.fetch_sub(1, std::memory_order_relaxed);
                }
                if (_total_pinned_bytes.load(std::memory_order_relaxed) >= sz) {
                    _total_pinned_bytes.fetch_sub(sz, std::memory_order_relaxed);
                }
                sub_used(sz);

                // Return or dispatch
                to_delete->provenance.dispatch_return(hotfield, runway);
                delete to_delete;

                _total_expired_unpins.fetch_add(1, std::memory_order_relaxed);
                expired_count++;
            } else {
                prev = cur;
                cur = cur->next;
            }
        }
    }
    return expired_count;
}
