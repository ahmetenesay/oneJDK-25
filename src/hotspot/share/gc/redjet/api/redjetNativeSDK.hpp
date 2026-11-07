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

#ifndef SHARE_GC_REDJET_API_REDJETNATIVESDK_HPP
#define SHARE_GC_REDJET_API_REDJETNATIVESDK_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <utility>
#include <type_traits>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. NATIVE REFERENCE TRACKER & GC ROOT REGISTRY                            */
/* ========================================================================= */

struct alignas(64) RedjetNativeRefEntry {
    void*                 native_or_java_ptr{ nullptr };
    uint64_t              ref_id{ 0 };
    uint32_t              owner_thread_id{ 0 };
    uint64_t              created_timestamp_ns{ 0 };
    std::atomic<uint32_t> strong_count{ 0 };
    std::atomic<bool>     is_active{ false };
    RedjetNativeRefEntry* next{ nullptr };
};

class RedjetNativeRefRegistry {
public:
    static constexpr size_t BUCKET_COUNT = 512;

private:
    RedjetNativeRefEntry* _buckets[BUCKET_COUNT];
    hs_ticket_lock_t      _locks[BUCKET_COUNT];
    std::atomic<uint64_t> _next_ref_id{ 1 };
    std::atomic<size_t>   _total_active_refs{ 0 };

    inline size_t hash_ptr(const void* ptr) const noexcept {
        uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
        return ((v >> 3) ^ (v >> 11) ^ (v >> 19)) % BUCKET_COUNT;
    }

public:
    RedjetNativeRefRegistry();
    ~RedjetNativeRefRegistry();

    // Registers a native reference with Redjet GC root tracking
    RJ_API uint64_t register_ref(void* ptr, uint32_t thread_id);

    // Retains or releases an existing native reference
    RJ_API void retain_ref(uint64_t ref_id);
    RJ_API void release_ref(uint64_t ref_id);

    // GC Query: checks if an object address is currently referenced by native code
    RJ_API bool is_referenced(const void* ptr) const noexcept;

    // GC Root Scanner Callback: iterates all active native roots
    RJ_API void iterate_roots(void (*closure)(void** slot, void* arg), void* arg);

    size_t active_reference_count() const noexcept {
        return _total_active_refs.load(std::memory_order_relaxed);
    }
};

extern RedjetNativeRefRegistry g_redjet_native_registry;

/* ========================================================================= */
/* 2. ATOMIC SYNCHRONIZATION RING BUFFER (Native <-> Java IPC)               */
/* ========================================================================= */

template <typename T, size_t Capacity = 4096>
class RedjetNativeSyncRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

private:
    struct alignas(64) Slot {
        T                   data{};
        std::atomic<bool>   ready{ false };
    };

    Slot                              _slots[Capacity];
    alignas(64) std::atomic<uint64_t> _head{ 0 }; // Write cursor
    alignas(64) std::atomic<uint64_t> _tail{ 0 }; // Read cursor
    hs_ticket_lock_t                  _push_lock;
    hs_ticket_lock_t                  _pop_lock;
    std::atomic<uint64_t>             _total_pushed{ 0 };
    std::atomic<uint64_t>             _total_popped{ 0 };

public:
    RedjetNativeSyncRing() = default;
    RedjetNativeSyncRing(const RedjetNativeSyncRing&) = delete;
    RedjetNativeSyncRing& operator=(const RedjetNativeSyncRing&) = delete;

    bool try_push(const T& item) noexcept {
        hs_ticket_guard_t guard(_push_lock);
        uint64_t head = _head.load(std::memory_order_relaxed);
        uint64_t tail = _tail.load(std::memory_order_acquire);

        if (head - tail >= Capacity) {
            return false; // Ring buffer full
        }

        size_t idx = head & (Capacity - 1);
        _slots[idx].data = item;
        _slots[idx].ready.store(true, std::memory_order_release);
        _head.store(head + 1, std::memory_order_release);
        _total_pushed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool try_pop(T& out_item) noexcept {
        hs_ticket_guard_t guard(_pop_lock);
        uint64_t tail = _tail.load(std::memory_order_relaxed);
        uint64_t head = _head.load(std::memory_order_acquire);

        if (tail >= head) {
            return false; // Ring buffer empty
        }

        size_t idx = tail & (Capacity - 1);
        if (!_slots[idx].ready.load(std::memory_order_acquire)) {
            return false;
        }

        out_item = _slots[idx].data;
        _slots[idx].ready.store(false, std::memory_order_release);
        _tail.store(tail + 1, std::memory_order_release);
        _total_popped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    size_t size() const noexcept {
        uint64_t h = _head.load(std::memory_order_relaxed);
        uint64_t t = _tail.load(std::memory_order_relaxed);
        return h >= t ? static_cast<size_t>(h - t) : 0;
    }

    bool is_empty() const noexcept { return size() == 0; }
    bool is_full() const noexcept { return size() >= Capacity; }
    constexpr size_t capacity() const noexcept { return Capacity; }
    uint64_t total_pushed() const noexcept { return _total_pushed.load(std::memory_order_relaxed); }
    uint64_t total_popped() const noexcept { return _total_popped.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 3. REDJET NATIVE TRACKER (TLS Ring & Bidirectional Live Reference Updater)*/
/* ========================================================================= */

struct alignas(64) RedjetLiveRefSlot {
    std::atomic<void*>    target_address{ nullptr }; // Points to live object in heap/slab
    void**                holder_slot{ nullptr };     // Address of pointer inside live_ref ControlBlock
    uint64_t              ref_id{ 0 };
    uint32_t              thread_id{ 0 };
    std::atomic<bool>     is_occupied{ false };
    std::atomic<uint64_t> generation{ 0 };
};

class RedjetNativeTracker {
public:
    static constexpr size_t TRACKER_CAPACITY = 1024;
    static constexpr size_t TRACKER_MASK     = TRACKER_CAPACITY - 1;

private:
    RedjetLiveRefSlot   _slots[TRACKER_CAPACITY];
    alignas(64) std::atomic<uint64_t> _alloc_cursor{ 0 };
    std::atomic<size_t> _active_slots{ 0 };
    uint32_t            _thread_id{ 0 };
    RedjetNativeTracker* _next{ nullptr };

    static alignas(64) std::atomic<RedjetNativeTracker*> _head_tracker;
    static hs_ticket_lock_t                              _registry_lock;
    static std::atomic<uint64_t>                         _total_relocations_updated;
    static std::atomic<uint64_t>                         _total_reverse_syncs;

public:
    RedjetNativeTracker(uint32_t tid = 0);
    ~RedjetNativeTracker();

    // TLS thread instance access
    RJ_API static RedjetNativeTracker& thread_tracker();

    // Register a live reference into the TLS ring
    RJ_API uint64_t register_live_slot(void** holder_ptr, void* initial_target);

    // Unregister a live reference slot
    RJ_API void unregister_live_slot(uint64_t slot_id, void** holder_ptr);

    // GC Relocation Hook: Called by Redjet GC when objects are compacted or evacuated
    // Scans all registered native trackers across threads and atomically updates holder slots
    RJ_API static size_t update_relocated_object(const void* old_addr, void* new_addr);

    // Bulk range relocation update (e.g., slab compaction)
    RJ_API static size_t update_relocated_range(const void* old_base, void* new_base, size_t range_bytes);

    // Reverse Native -> Java Synchronization (e.g. realloc)
    RJ_API static bool update_native_memory_segment(const void* old_native_ptr, void* new_native_ptr, size_t new_size = 0);

    // Telemetry
    static uint64_t total_relocations_updated() noexcept { return _total_relocations_updated.load(std::memory_order_relaxed); }
    static uint64_t total_reverse_syncs() noexcept { return _total_reverse_syncs.load(std::memory_order_relaxed); }
    size_t active_slots() const noexcept { return _active_slots.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 4. onejdk::live_ref<T> BIDIRECTIONAL NATIVE POINTER TRACKING SMART POINTER*/
/* ========================================================================= */

namespace onejdk {

template <typename T>
class live_ref {
private:
    struct ControlBlock {
        T*                    ptr{ nullptr };
        uint64_t              registry_ref_id{ 0 };
        uint64_t              tracker_slot_id{ 0 };
        std::atomic<uint32_t> ref_count{ 1 };

        ControlBlock(T* p, uint64_t ref_id, uint64_t slot_id)
            : ptr(p), registry_ref_id(ref_id), tracker_slot_id(slot_id), ref_count(1) {}
    };

    ControlBlock* _ctrl{ nullptr };

    void release_internal() noexcept {
        if (_ctrl != nullptr) {
            if (_ctrl->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (_ctrl->tracker_slot_id != 0) {
                    RedjetNativeTracker::thread_tracker().unregister_live_slot(
                        _ctrl->tracker_slot_id, reinterpret_cast<void**>(&(_ctrl->ptr)));
                }
                if (_ctrl->registry_ref_id != 0) {
                    g_redjet_native_registry.release_ref(_ctrl->registry_ref_id);
                }
                delete _ctrl;
            }
            _ctrl = nullptr;
        }
    }

public:
    live_ref() noexcept : _ctrl(nullptr) {}

    explicit live_ref(T* raw_ptr, uint32_t thread_id = 0) {
        if (raw_ptr != nullptr) {
            uint64_t ref_id = g_redjet_native_registry.register_ref(static_cast<void*>(raw_ptr), thread_id);
            _ctrl = new ControlBlock(raw_ptr, ref_id, 0);
            uint64_t slot_id = RedjetNativeTracker::thread_tracker().register_live_slot(
                reinterpret_cast<void**>(&(_ctrl->ptr)), static_cast<void*>(raw_ptr));
            _ctrl->tracker_slot_id = slot_id;
        }
    }

    live_ref(const live_ref& other) noexcept : _ctrl(other._ctrl) {
        if (_ctrl != nullptr) {
            _ctrl->ref_count.fetch_add(1, std::memory_order_relaxed);
            if (_ctrl->registry_ref_id != 0) {
                g_redjet_native_registry.retain_ref(_ctrl->registry_ref_id);
            }
        }
    }

    live_ref(live_ref&& other) noexcept : _ctrl(other._ctrl) {
        other._ctrl = nullptr;
    }

    live_ref& operator=(const live_ref& other) noexcept {
        if (this != &other) {
            release_internal();
            _ctrl = other._ctrl;
            if (_ctrl != nullptr) {
                _ctrl->ref_count.fetch_add(1, std::memory_order_relaxed);
                if (_ctrl->registry_ref_id != 0) {
                    g_redjet_native_registry.retain_ref(_ctrl->registry_ref_id);
                }
            }
        }
        return *this;
    }

    live_ref& operator=(live_ref&& other) noexcept {
        if (this != &other) {
            release_internal();
            _ctrl = other._ctrl;
            other._ctrl = nullptr;
        }
        return *this;
    }

    ~live_ref() {
        release_internal();
    }

    T* get() const noexcept {
        return _ctrl != nullptr ? _ctrl->ptr : nullptr;
    }

    T& operator*() const noexcept {
        return *get();
    }

    T* operator->() const noexcept {
        return get();
    }

    bool is_valid() const noexcept {
        return get() != nullptr;
    }

    explicit operator bool() const noexcept {
        return is_valid();
    }

    uint32_t use_count() const noexcept {
        return _ctrl != nullptr ? _ctrl->ref_count.load(std::memory_order_relaxed) : 0;
    }

    void reset(T* new_ptr = nullptr, uint32_t thread_id = 0) {
        release_internal();
        if (new_ptr != nullptr) {
            uint64_t ref_id = g_redjet_native_registry.register_ref(static_cast<void*>(new_ptr), thread_id);
            _ctrl = new ControlBlock(new_ptr, ref_id, 0);
            uint64_t slot_id = RedjetNativeTracker::thread_tracker().register_live_slot(
                reinterpret_cast<void**>(&(_ctrl->ptr)), static_cast<void*>(new_ptr));
            _ctrl->tracker_slot_id = slot_id;
        }
    }

    void swap(live_ref& other) noexcept {
        std::swap(_ctrl, other._ctrl);
    }
};

template <typename T, typename... Args>
inline live_ref<T> make_live(Args&&... args) {
    return live_ref<T>(new T(std::forward<Args>(args)...));
}

} // namespace onejdk

extern "C" {
  RJ_API void onejdk_update_native_ref(void* old_ptr, void* new_ptr);
  RJ_API void* onejdk_get_live_ref_target(uint64_t ref_id);
}

#endif // SHARE_GC_REDJET_API_REDJETNATIVESDK_HPP
