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

#ifndef SHARE_GC_REDJET_REDJETEMPTYBOX_HPP
#define SHARE_GC_REDJET_REDJETEMPTYBOX_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// RedjetEmptyBox: Phantom Slot & Tombstone Reservation Subsystem
// ============================================================================
// Canonical Magic Word 0xEB0C5107 spells: EB (EmptyBox) 0C (Object/Cell) 5107 (SLOT)
#define HS_EMPTYBOX_MAGIC        0xEB0C5107U
#define HS_EMPTYBOX_MAGIC_LEGACY 0x53464542U
#define HS_EMPTYBOX_BYTE_MAGIC   0xEBU

// EmptyBox Reservation & State Flags
static constexpr uint8_t HS_EMPTYBOX_FLAG_FREE        = 0x00;
static constexpr uint8_t HS_EMPTYBOX_FLAG_RESERVED    = 0x01; // Phantom reservation active
static constexpr uint8_t HS_EMPTYBOX_FLAG_PARK_TOMB   = 0x02; // Tombstone for object parked in park_arena
static constexpr uint8_t HS_EMPTYBOX_FLAG_DECOMMITTED = 0x04; // Backing physical pages decommitted
static constexpr uint8_t HS_EMPTYBOX_FLAG_EXPIRED     = 0x08; // Reservation lease has timed out
static constexpr uint8_t HS_EMPTYBOX_FLAG_CLAIMED     = 0x10; // Successfully reclaimed by returning object
static constexpr uint8_t HS_EMPTYBOX_FLAG_COALESCED   = 0x20; // Merged during VFS compaction
static constexpr uint8_t HS_EMPTYBOX_FLAG_COMPACT_OOL = 0x40; // Slot uses compact guard with out-of-line descriptor

// Compact Phantom Guard for small slots (16 <= bytes < 64)
// Prevents overflowing slots smaller than full 64-byte descriptor
struct alignas(8) HS_compact_emptybox_guard_t {
    uint32_t hsMN{ HS_EMPTYBOX_MAGIC };       // First 4 bytes match hardware TableGen guard
    uint32_t reservation_owner_tid{ 0 };     // Tid owning reservation
    void*    full_descriptor{ nullptr };      // Pointer to full RedjetEmptyBox descriptor
};

// Full 64-byte Cache-Line Aligned EmptyBox Header
struct SACFOR_ALIGN_CL HS_hotspot_emptybox_header_t {
    uint32_t                hsMN{ HS_EMPTYBOX_MAGIC };               // 0..3
    uint32_t                reservation_owner_tid{ 0 };             // 4..7
    size_t                  reserved_slot_bytes{ 0 };               // 8..15

    std::atomic<void*>      forwarding_address{ nullptr };          // 16..23
    uint64_t                timeout_tsc{ 0 };                       // 24..31

    uint32_t                prev_function_id{ 0 };                  // 32..35
    uint8_t                 _magic{ HS_EMPTYBOX_BYTE_MAGIC };       // 36
    uint8_t                 slot_flags{ HS_EMPTYBOX_FLAG_FREE };    // 37
    uint8_t                 is_decommitted{ 0 };                    // 38
    uint8_t                 origin_tier{ 0 };                       // 39

    std::atomic<uint32_t>   claim_lock{ 0 };                        // 40..43
    uint32_t                decommitted_kb{ 0 };                    // 44..47

    HS_hotspot_emptybox_header_t* next{ nullptr };                  // 48..55
    HS_hotspot_emptybox_header_t* prev{ nullptr };                  // 56..63

    // Compatibility accessors for legacy code in SacFor_VM
    inline size_t prev_occupant_size() const noexcept { return reserved_slot_bytes; }
    inline size_t aligned_hole_size() const noexcept  { return reserved_slot_bytes; }
    inline uint64_t created_sweep_no() const noexcept { return timeout_tsc; }

    void init(uint32_t prev_fn, size_t prev_size, size_t aligned_size, void* fwd = nullptr, uint64_t sweep_no = 0) noexcept {
        hsMN = HS_EMPTYBOX_MAGIC;
        reservation_owner_tid = 0;
        reserved_slot_bytes = (aligned_size > 0) ? aligned_size : prev_size;
        forwarding_address.store(fwd, std::memory_order_release);
        timeout_tsc = sweep_no;
        prev_function_id = prev_fn;
        _magic = HS_EMPTYBOX_BYTE_MAGIC;
        slot_flags = HS_EMPTYBOX_FLAG_FREE;
        is_decommitted = 0;
        origin_tier = 0;
        claim_lock.store(0, std::memory_order_relaxed);
        decommitted_kb = 0;
        next = prev = nullptr;
    }

    inline bool isValid() const noexcept {
        return (hsMN == HS_EMPTYBOX_MAGIC || hsMN == HS_EMPTYBOX_MAGIC_LEGACY || _magic == HS_EMPTYBOX_BYTE_MAGIC);
    }

    inline bool is_reserved() const noexcept {
        return (slot_flags & HS_EMPTYBOX_FLAG_RESERVED) != 0;
    }

    inline bool is_park_tombstone() const noexcept {
        return (slot_flags & HS_EMPTYBOX_FLAG_PARK_TOMB) != 0;
    }

    inline bool is_compact_ool() const noexcept {
        return (slot_flags & HS_EMPTYBOX_FLAG_COMPACT_OOL) != 0;
    }

    inline bool is_timeout(uint64_t current_tsc = 0) const noexcept {
        if (timeout_tsc == 0) return false;
        if (current_tsc == 0) current_tsc = sacculus_rdtsc();
        return current_tsc >= timeout_tsc;
    }

    inline uint8_t* hole_body() noexcept {
        return reinterpret_cast<uint8_t*>(this) + sizeof(HS_hotspot_emptybox_header_t);
    }

    static inline HS_hotspot_emptybox_header_t* from_body(void* body_ptr) noexcept {
        if (!body_ptr) return nullptr;
        auto* eb = reinterpret_cast<HS_hotspot_emptybox_header_t*>(
            static_cast<uint8_t*>(body_ptr) - sizeof(HS_hotspot_emptybox_header_t));
        const volatile uint32_t* magic32 = &eb->hsMN;
        const volatile uint8_t* magic8 = &eb->_magic;
        if (*magic32 != HS_EMPTYBOX_MAGIC && *magic32 != HS_EMPTYBOX_MAGIC_LEGACY && *magic8 != HS_EMPTYBOX_BYTE_MAGIC) {
            return nullptr;
        }
        return eb;
    }

    inline void scrub_coalesced_tombstone() noexcept {
        *const_cast<volatile uint32_t*>(&hsMN) = 0;
        *const_cast<volatile uint8_t*>(&_magic) = 0;
        slot_flags = HS_EMPTYBOX_FLAG_COALESCED;
        forwarding_address.store(nullptr, std::memory_order_release);
        HS_COMPILER_BARRIER(this);
    }

    // Reserves a phantom slot preventing runway or hotfield allocators from stealing it
    inline bool reserve_phantom_slot(size_t slot_bytes, uint32_t tid, uint64_t timeout_cycles,
                                    void* parked_obj, uint32_t tier = 0) noexcept {
        uint32_t expected = 0;
        if (!claim_lock.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            return false; // Already locked by another worker
        }

        hsMN = HS_EMPTYBOX_MAGIC;
        _magic = HS_EMPTYBOX_BYTE_MAGIC;
        reservation_owner_tid = tid;
        reserved_slot_bytes = slot_bytes;
        origin_tier = static_cast<uint8_t>(tier);
        uint64_t now_tsc = sacculus_rdtsc();
        timeout_tsc = (timeout_cycles > 0) ? (now_tsc + timeout_cycles) : 0;
        slot_flags = static_cast<uint8_t>(HS_EMPTYBOX_FLAG_RESERVED | HS_EMPTYBOX_FLAG_PARK_TOMB);
        forwarding_address.store(parked_obj, std::memory_order_release);

        claim_lock.store(0, std::memory_order_release);
        return true;
    }

    // Atomically exchange forwarding address
    inline bool atomic_exchange_forwarding(void* expected_fwd, void* new_fwd) noexcept {
        return forwarding_address.compare_exchange_strong(expected_fwd, new_fwd,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire);
    }

    inline void* atomic_swap_forwarding(void* new_fwd) noexcept {
        return forwarding_address.exchange(new_fwd, std::memory_order_acq_rel);
    }

    // Reclaim slot when object returns from park_arena
    inline bool reclaim_slot(uint32_t caller_tid, void** out_parked_obj) noexcept {
        uint32_t expected_lock = 0;
        if (!claim_lock.compare_exchange_strong(expected_lock, 1, std::memory_order_acq_rel)) {
            return false;
        }

        if (!is_reserved() || !is_park_tombstone()) {
            claim_lock.store(0, std::memory_order_release);
            return false;
        }

        // Check if reservation expired
        if (is_timeout()) {
            slot_flags |= HS_EMPTYBOX_FLAG_EXPIRED;
            claim_lock.store(0, std::memory_order_release);
            return false;
        }

        void* parked = forwarding_address.load(std::memory_order_acquire);
        if (out_parked_obj != nullptr) {
            *out_parked_obj = parked;
        }

        // Clear magic word so hardware TableGen guard ceases treating this slot as a tombstone
        *const_cast<volatile uint32_t*>(&hsMN) = 0;
        *const_cast<volatile uint8_t*>(&_magic) = 0;
        slot_flags = HS_EMPTYBOX_FLAG_CLAIMED;
        forwarding_address.store(nullptr, std::memory_order_release);
        claim_lock.store(0, std::memory_order_release);
        return true;
    }

    // Cancels and frees reservation, resetting hardware magic word
    inline void release_reservation() noexcept {
        *const_cast<volatile uint32_t*>(&hsMN) = 0;
        *const_cast<volatile uint8_t*>(&_magic) = 0;
        slot_flags = HS_EMPTYBOX_FLAG_FREE;
        forwarding_address.store(nullptr, std::memory_order_release);
        reservation_owner_tid = 0;
        timeout_tsc = 0;
    }

    // Resolves descriptor from either inline or compact guard
    static inline HS_hotspot_emptybox_header_t* resolve_tombstone(void* slot_addr, size_t slot_bytes) noexcept {
        if (!slot_addr || slot_bytes < 16) return nullptr;
        const volatile uint32_t* magic = reinterpret_cast<const volatile uint32_t*>(slot_addr);
        if (*magic != HS_EMPTYBOX_MAGIC) return nullptr;

        if (slot_bytes >= sizeof(HS_hotspot_emptybox_header_t)) {
            return reinterpret_cast<HS_hotspot_emptybox_header_t*>(slot_addr);
        }

        // Compact guard: pointer to descriptor is at offset 8
        auto* compact = reinterpret_cast<HS_compact_emptybox_guard_t*>(slot_addr);
        return reinterpret_cast<HS_hotspot_emptybox_header_t*>(compact->full_descriptor);
    }

    // Places a tombstone header at slot_addr.
    // If slot_bytes >= 64, places full descriptor inline at slot_addr.
    // If 16 <= slot_bytes < 64, uses provided out_of_line_desc (or allocates) and writes 16-byte compact guard.
    static inline HS_hotspot_emptybox_header_t* place_tombstone(
        void* slot_addr, size_t slot_bytes, void* parked_obj,
        uint32_t tid, uint64_t timeout_cycles, uint32_t tier = 0,
        HS_hotspot_emptybox_header_t* out_of_line_desc = nullptr) noexcept {

        if (!slot_addr || slot_bytes < 16) {
            return nullptr;
        }

        if (slot_bytes >= sizeof(HS_hotspot_emptybox_header_t)) {
            // Full inline tombstone
            auto* eb = reinterpret_cast<HS_hotspot_emptybox_header_t*>(slot_addr);
            eb->init(0, slot_bytes, slot_bytes, parked_obj, 0);
            eb->reserve_phantom_slot(slot_bytes, tid, timeout_cycles, parked_obj, tier);
            return eb;
        } else {
            // Compact guard for small slots (16..63 bytes):
            // Does NOT overflow the small slot boundaries!
            if (!out_of_line_desc) return nullptr;
            out_of_line_desc->init(0, slot_bytes, slot_bytes, parked_obj, 0);
            out_of_line_desc->reserve_phantom_slot(slot_bytes, tid, timeout_cycles, parked_obj, tier);
            out_of_line_desc->slot_flags |= HS_EMPTYBOX_FLAG_COMPACT_OOL;

            auto* compact = reinterpret_cast<HS_compact_emptybox_guard_t*>(slot_addr);
            compact->hsMN = HS_EMPTYBOX_MAGIC;
            compact->reservation_owner_tid = tid;
            compact->full_descriptor = out_of_line_desc;
            return out_of_line_desc;
        }
    }
};

static_assert(sizeof(HS_hotspot_emptybox_header_t) == 64, "HS_hotspot_emptybox_header_t must be exactly 64 bytes");
static_assert(sizeof(HS_compact_emptybox_guard_t) == 16, "HS_compact_emptybox_guard_t must be exactly 16 bytes");

using hsEmptyboxHeader = HS_hotspot_emptybox_header_t;
using sacculus_emptybox_header_t = hsEmptyboxHeader;
using RedjetEmptyBox = HS_hotspot_emptybox_header_t;

// Telemetry & Statistics
struct RedjetEmptyBoxTelemetry {
    uint64_t total_reservations_created;
    uint64_t total_reservations_claimed;
    uint64_t total_timeouts_detected;
    uint64_t total_theft_attempts_prevented;
    uint64_t total_tombstones_scrubbed;
    size_t   active_reserved_bytes;
};

class RedjetEmptyBoxManager {
public:
    RJ_API static void record_reservation(size_t bytes);
    RJ_API static void record_claim(size_t bytes);
    RJ_API static void record_timeout();
    RJ_API static void record_theft_prevented();
    RJ_API static void record_scrub(size_t bytes);
    RJ_API static RedjetEmptyBoxTelemetry get_telemetry();
    RJ_API static void reset_telemetry();
};

#endif // SHARE_GC_REDJET_REDJETEMPTYBOX_HPP
