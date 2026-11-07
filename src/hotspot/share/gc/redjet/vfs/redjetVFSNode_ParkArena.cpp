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
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <new>

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
    rec->native_pinned_ptr = obj_ptr; // Direct pinned buffer
    rec->buffer_size = size_bytes;
    rec->pinning_thread_id = thread_id;
    rec->pin_timestamp_ns = now;
    rec->lease_timeout_ns = (lease_ns > 0) ? (now + lease_ns) : 0;
    rec->pin_ref_count.store(1, std::memory_order_relaxed);
    rec->is_pinned.store(true, std::memory_order_release);

    rec->next = _hash_buckets[b_idx];
    _hash_buckets[b_idx] = rec;

    _total_pinned_objects.fetch_add(1, std::memory_order_relaxed);
    _total_pinned_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    _total_pin_operations.fetch_add(1, std::memory_order_relaxed);
    add_used(size_bytes);

    hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0,
                   static_cast<uint16_t>(size_bytes / 1024), 0, 0, 0, 0,
                   "park_arena_pinned");
    return true;
}

bool RedjetVFSNode_ParkArena::unpin_java_object(void* obj_ptr, uint32_t thread_id) {
    if (!obj_ptr) return false;

    size_t b_idx = compute_bucket(obj_ptr);
    hs_ticket_guard_t guard(_bucket_locks[b_idx]);

    RedjetPinnedNativeRecord* prev = nullptr;
    RedjetPinnedNativeRecord* cur = _hash_buckets[b_idx];

    while (cur != nullptr) {
        if (cur->java_oop == obj_ptr) {
            uint32_t refs = cur->pin_ref_count.fetch_sub(1, std::memory_order_acq_rel);
            if (refs <= 1) {
                // Final unpin: remove from bucket
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
                delete cur;

                _total_unpin_operations.fetch_add(1, std::memory_order_relaxed);
                hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, thread_id, 0,
                               0, 0, 0, 0, 0, "park_arena_unpinned");
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

size_t RedjetVFSNode_ParkArena::sweep_expired_leases(uint64_t now_ns) {
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
