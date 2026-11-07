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

#include "gc/redjet/api/redjetNativeSDK.hpp"
#include <new>

RedjetNativeRefRegistry g_redjet_native_registry;

RedjetNativeRefRegistry::RedjetNativeRefRegistry() {
    for (size_t i = 0; i < BUCKET_COUNT; ++i) {
        _buckets[i] = nullptr;
    }
}

RedjetNativeRefRegistry::~RedjetNativeRefRegistry() {
    for (size_t i = 0; i < BUCKET_COUNT; ++i) {
        hs_ticket_guard_t guard(_locks[i]);
        RedjetNativeRefEntry* cur = _buckets[i];
        while (cur != nullptr) {
            RedjetNativeRefEntry* next = cur->next;
            delete cur;
            cur = next;
        }
        _buckets[i] = nullptr;
    }
}

uint64_t RedjetNativeRefRegistry::register_ref(void* ptr, uint32_t thread_id) {
    if (!ptr) return 0;

    size_t b = hash_ptr(ptr);
    hs_ticket_guard_t guard(_locks[b]);

    // Check if pointer already has an active entry
    RedjetNativeRefEntry* cur = _buckets[b];
    while (cur != nullptr) {
        if (cur->native_or_java_ptr == ptr && cur->is_active.load(std::memory_order_relaxed)) {
            cur->strong_count.fetch_add(1, std::memory_order_relaxed);
            return cur->ref_id;
        }
        cur = cur->next;
    }

    uint64_t new_id = _next_ref_id.fetch_add(1, std::memory_order_relaxed);
    auto* entry = new (std::nothrow) RedjetNativeRefEntry();
    if (!entry) return 0;

    entry->native_or_java_ptr = ptr;
    entry->ref_id = new_id;
    entry->owner_thread_id = thread_id;
    entry->created_timestamp_ns = sacculus_now_ns();
    entry->strong_count.store(1, std::memory_order_relaxed);
    entry->is_active.store(true, std::memory_order_release);

    entry->next = _buckets[b];
    _buckets[b] = entry;

    _total_active_refs.fetch_add(1, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_TRACE, thread_id, 0,
                   0, 0, 0, 0, 0, "native_ref_registered");
    return new_id;
}

void RedjetNativeRefRegistry::retain_ref(uint64_t ref_id) {
    if (ref_id == 0) return;

    for (size_t b = 0; b < BUCKET_COUNT; ++b) {
        hs_ticket_guard_t guard(_locks[b]);
        RedjetNativeRefEntry* cur = _buckets[b];
        while (cur != nullptr) {
            if (cur->ref_id == ref_id) {
                cur->strong_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            cur = cur->next;
        }
    }
}

void RedjetNativeRefRegistry::release_ref(uint64_t ref_id) {
    if (ref_id == 0) return;

    for (size_t b = 0; b < BUCKET_COUNT; ++b) {
        hs_ticket_guard_t guard(_locks[b]);
        RedjetNativeRefEntry* prev = nullptr;
        RedjetNativeRefEntry* cur = _buckets[b];

        while (cur != nullptr) {
            if (cur->ref_id == ref_id) {
                uint32_t count = cur->strong_count.fetch_sub(1, std::memory_order_acq_rel);
                if (count <= 1) {
                    cur->is_active.store(false, std::memory_order_release);
                    if (prev != nullptr) {
                        prev->next = cur->next;
                    } else {
                        _buckets[b] = cur->next;
                    }
                    delete cur;
                    if (_total_active_refs.load(std::memory_order_relaxed) > 0) {
                        _total_active_refs.fetch_sub(1, std::memory_order_relaxed);
                    }
                    return;
                }
                return;
            }
            prev = cur;
            cur = cur->next;
        }
    }
}

bool RedjetNativeRefRegistry::is_referenced(const void* ptr) const noexcept {
    if (!ptr) return false;
    size_t b = hash_ptr(ptr);
    hs_ticket_guard_t guard(const_cast<hs_ticket_lock_t&>(_locks[b]));

    RedjetNativeRefEntry* cur = _buckets[b];
    while (cur != nullptr) {
        if (cur->native_or_java_ptr == ptr && cur->is_active.load(std::memory_order_acquire)) {
            return (cur->strong_count.load(std::memory_order_relaxed) > 0);
        }
        cur = cur->next;
    }
    return false;
}

void RedjetNativeRefRegistry::iterate_roots(void (*closure)(void** slot, void* arg), void* arg) {
    if (!closure) return;

    for (size_t b = 0; b < BUCKET_COUNT; ++b) {
        hs_ticket_guard_t guard(_locks[b]);
        RedjetNativeRefEntry* cur = _buckets[b];
        while (cur != nullptr) {
            if (cur->is_active.load(std::memory_order_acquire) && cur->strong_count.load(std::memory_order_relaxed) > 0) {
                closure(&(cur->native_or_java_ptr), arg);
            }
            cur = cur->next;
        }
    }
}

/* ========================================================================= */
/* 2. REDJET NATIVE TRACKER IMPLEMENTATION                                   */
/* ========================================================================= */

alignas(64) std::atomic<RedjetNativeTracker*> RedjetNativeTracker::_head_tracker{ nullptr };
hs_ticket_lock_t                              RedjetNativeTracker::_registry_lock;
std::atomic<uint64_t>                         RedjetNativeTracker::_total_relocations_updated{ 0 };
std::atomic<uint64_t>                         RedjetNativeTracker::_total_reverse_syncs{ 0 };

RedjetNativeTracker::RedjetNativeTracker(uint32_t tid)
    : _thread_id(tid),
      _next(nullptr) {
    _alloc_cursor.store(0, std::memory_order_relaxed);
    _active_slots.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < TRACKER_CAPACITY; ++i) {
        _slots[i].target_address.store(nullptr, std::memory_order_relaxed);
        _slots[i].holder_slot = nullptr;
        _slots[i].ref_id = 0;
        _slots[i].thread_id = tid;
        _slots[i].is_occupied.store(false, std::memory_order_relaxed);
        _slots[i].generation.store(0, std::memory_order_relaxed);
    }

    // Register into global tracker list
    hs_ticket_guard_t guard(_registry_lock);
    _next = _head_tracker.load(std::memory_order_relaxed);
    _head_tracker.store(this, std::memory_order_release);
}

RedjetNativeTracker::~RedjetNativeTracker() {
    // Unlink from global tracker list
    hs_ticket_guard_t guard(_registry_lock);
    RedjetNativeTracker* prev = nullptr;
    RedjetNativeTracker* cur = _head_tracker.load(std::memory_order_relaxed);
    while (cur != nullptr) {
        if (cur == this) {
            if (prev != nullptr) {
                prev->_next = cur->_next;
            } else {
                _head_tracker.store(cur->_next, std::memory_order_release);
            }
            break;
        }
        prev = cur;
        cur = cur->_next;
    }
}

RedjetNativeTracker& RedjetNativeTracker::thread_tracker() {
    thread_local RedjetNativeTracker t_tracker(sacculus_get_thread_id());
    return t_tracker;
}

uint64_t RedjetNativeTracker::register_live_slot(void** holder_ptr, void* initial_target) {
    if (!holder_ptr || !initial_target) return 0;

    for (size_t attempt = 0; attempt < TRACKER_CAPACITY; ++attempt) {
        uint64_t idx = _alloc_cursor.fetch_add(1, std::memory_order_relaxed) & TRACKER_MASK;
        bool expected_occupied = false;
        if (_slots[idx].is_occupied.compare_exchange_strong(expected_occupied, true, std::memory_order_acq_rel)) {
            _slots[idx].holder_slot = holder_ptr;
            _slots[idx].target_address.store(initial_target, std::memory_order_release);
            _slots[idx].ref_id = idx + 1;
            _slots[idx].thread_id = _thread_id;
            _slots[idx].generation.fetch_add(1, std::memory_order_relaxed);
            _active_slots.fetch_add(1, std::memory_order_relaxed);
            return idx + 1;
        }
    }
    return 0; // Ring saturated
}

void RedjetNativeTracker::unregister_live_slot(uint64_t slot_id, void** holder_ptr) {
    if (slot_id == 0 || slot_id > TRACKER_CAPACITY) return;
    size_t idx = (slot_id - 1) & TRACKER_MASK;

    if (_slots[idx].holder_slot == holder_ptr) {
        _slots[idx].target_address.store(nullptr, std::memory_order_release);
        _slots[idx].holder_slot = nullptr;
        _slots[idx].is_occupied.store(false, std::memory_order_release);
        if (_active_slots.load(std::memory_order_relaxed) > 0) {
            _active_slots.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

size_t RedjetNativeTracker::update_relocated_object(const void* old_addr, void* new_addr) {
    if (!old_addr || !new_addr || old_addr == new_addr) return 0;
    size_t updated = 0;

    hs_ticket_guard_t guard(_registry_lock);
    RedjetNativeTracker* tracker = _head_tracker.load(std::memory_order_acquire);

    while (tracker != nullptr) {
        for (size_t i = 0; i < TRACKER_CAPACITY; ++i) {
            if (tracker->_slots[i].is_occupied.load(std::memory_order_relaxed)) {
                void* expected = const_cast<void*>(old_addr);
                if (tracker->_slots[i].target_address.compare_exchange_strong(expected, new_addr, std::memory_order_acq_rel)) {
                    if (tracker->_slots[i].holder_slot != nullptr) {
                        *(tracker->_slots[i].holder_slot) = new_addr;
                    }
                    updated++;
                }
            }
        }
        tracker = tracker->_next;
    }

    if (updated > 0) {
        _total_relocations_updated.fetch_add(updated, std::memory_order_relaxed);
        hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, 0, 0,
                       0, 0, 0, 0, 0, "live_ref_relocated");
    }
    return updated;
}

size_t RedjetNativeTracker::update_relocated_range(const void* old_base, void* new_base, size_t range_bytes) {
    if (!old_base || !new_base || range_bytes == 0) return 0;
    size_t updated = 0;

    uintptr_t old_start = reinterpret_cast<uintptr_t>(old_base);
    uintptr_t old_end   = old_start + range_bytes;
    uintptr_t new_start = reinterpret_cast<uintptr_t>(new_base);

    hs_ticket_guard_t guard(_registry_lock);
    RedjetNativeTracker* tracker = _head_tracker.load(std::memory_order_acquire);

    while (tracker != nullptr) {
        for (size_t i = 0; i < TRACKER_CAPACITY; ++i) {
            if (tracker->_slots[i].is_occupied.load(std::memory_order_relaxed)) {
                void* cur_target = tracker->_slots[i].target_address.load(std::memory_order_relaxed);
                uintptr_t cur_val = reinterpret_cast<uintptr_t>(cur_target);
                if (cur_val >= old_start && cur_val < old_end) {
                    uintptr_t offset = cur_val - old_start;
                    void* relocated_target = reinterpret_cast<void*>(new_start + offset);
                    void* expected = cur_target;
                    if (tracker->_slots[i].target_address.compare_exchange_strong(expected, relocated_target, std::memory_order_acq_rel)) {
                        if (tracker->_slots[i].holder_slot != nullptr) {
                            *(tracker->_slots[i].holder_slot) = relocated_target;
                        }
                        updated++;
                    }
                }
            }
        }
        tracker = tracker->_next;
    }

    if (updated > 0) {
        _total_relocations_updated.fetch_add(updated, std::memory_order_relaxed);
    }
    return updated;
}

bool RedjetNativeTracker::update_native_memory_segment(const void* old_native_ptr, void* new_native_ptr, size_t new_size) {
    if (!old_native_ptr || !new_native_ptr) return false;
    _total_reverse_syncs.fetch_add(1, std::memory_order_relaxed);

    size_t updated = update_relocated_object(old_native_ptr, new_native_ptr);
    hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, 0, 0,
                   0, 0, 0, 0, 0, "native_memory_segment_updated");
    return updated > 0;
}

extern "C" {
void onejdk_update_native_ref(void* old_ptr, void* new_ptr) {
    RedjetNativeTracker::update_native_memory_segment(old_ptr, new_ptr, 0);
}

void* onejdk_get_live_ref_target(uint64_t ref_id) {
    if (ref_id == 0) return nullptr;
    return nullptr;
}
}
