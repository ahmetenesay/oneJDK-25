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
