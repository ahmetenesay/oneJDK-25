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

#ifndef SHARE_GC_REDLET_REDLETRINGLOG_HPP
#define SHARE_GC_REDLET_REDLETRINGLOG_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <vector>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletPagefile.hpp"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef SACFOR_HOTLOG_SIZE
#define SACFOR_HOTLOG_SIZE  65536u
#endif

#ifndef SACFOR_COLDLOG_SIZE
#define SACFOR_COLDLOG_SIZE 16384u
#endif

/* ========================================================================= */
/* 1. CPU PAUSE & FAIR CACHE-LINE ALIGNED TICKET LOCK                        */
/* ========================================================================= */

#ifndef SF_TICKET_LOCK_T_DEFINED
#define SF_TICKET_LOCK_T_DEFINED
static inline void sf_ring_cpu_pause() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
    __yield();
#else
    __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
}

struct SACFOR_ALIGN_CL sf_ticket_lock_t {
    alignas(64) std::atomic<uint32_t> next_ticket{ 0 };
    alignas(64) std::atomic<uint32_t> now_serving{ 0 };

    void lock() noexcept {
        uint32_t my_ticket = next_ticket.fetch_add(1, std::memory_order_relaxed);
        while (now_serving.load(std::memory_order_acquire) != my_ticket) {
            sf_ring_cpu_pause();
        }
    }

    void unlock() noexcept {
        now_serving.fetch_add(1, std::memory_order_release);
    }
};

struct sf_ticket_guard_t {
    sf_ticket_lock_t& l;
    explicit sf_ticket_guard_t(sf_ticket_lock_t& lock_ref) noexcept : l(lock_ref) { l.lock(); }
    ~sf_ticket_guard_t() noexcept { l.unlock(); }
};
#endif

/* ========================================================================= */
/* 2. LOCK-FREE MPMC VYUKOV SEQUENCE RING BUFFER (sacculus_ring_t)           */
/* ========================================================================= */

template<typename Entry, uint32_t Capacity>
struct sacculus_ring_t {
    SACFOR_ALIGN_CL std::atomic<uint64_t> write_head{ 0 };
    SACFOR_ALIGN_CL std::atomic<uint64_t> read_head{ 0 };
    SACFOR_ALIGN_CL sf_ticket_lock_t      drain_lock;
    SACFOR_ALIGN_CL sf_ticket_lock_t      write_lock;
    Entry*                                ring{ nullptr };
    uint8_t*                              ready_flags{ nullptr };

    static constexpr uint32_t CAP = Capacity;

    static inline uint8_t load_ready_flag(const uint8_t* flag_ptr) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
        uint8_t v = *reinterpret_cast<volatile const uint8_t*>(flag_ptr);
        std::atomic_thread_fence(std::memory_order_acquire);
        return v;
#else
        return __atomic_load_n(flag_ptr, __ATOMIC_ACQUIRE);
#endif
    }

    static inline void store_ready_flag(uint8_t* flag_ptr, uint8_t val) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
        std::atomic_thread_fence(std::memory_order_release);
        *reinterpret_cast<volatile uint8_t*>(flag_ptr) = val;
#else
        __atomic_store_n(flag_ptr, val, __ATOMIC_RELEASE);
#endif
    }

    sacculus_ring_t() {
        size_t entry_bytes = static_cast<size_t>(Capacity) * sizeof(Entry);
        size_t flag_bytes  = (static_cast<size_t>(Capacity) + 63ULL) & ~63ULL;
        size_t total_bytes = entry_bytes + flag_bytes;
        void* raw = sf_os_reserve_memory(total_bytes);
        if (raw && sf_os_commit_memory(raw, total_bytes)) {
            std::memset(raw, 0, total_bytes);
            ring        = static_cast<Entry*>(raw);
            ready_flags = static_cast<uint8_t*>(raw) + entry_bytes;
        } else if (raw) {
            sf_os_release_memory(raw, total_bytes);
        }
    }

    ~sacculus_ring_t() {
        if (ring) {
            size_t entry_bytes = static_cast<size_t>(Capacity) * sizeof(Entry);
            size_t flag_bytes  = (static_cast<size_t>(Capacity) + 63ULL) & ~63ULL;
            sf_os_release_memory(ring, entry_bytes + flag_bytes);
            ring        = nullptr;
            ready_flags = nullptr;
        }
    }

    sacculus_ring_t(const sacculus_ring_t&) = delete;
    sacculus_ring_t& operator=(const sacculus_ring_t&) = delete;

    inline uint64_t map_index(uint64_t seq) const noexcept {
        if constexpr ((Capacity & (Capacity - 1)) == 0) {
            return seq & static_cast<uint64_t>(Capacity - 1);
        } else {
            return seq % static_cast<uint64_t>(Capacity);
        }
    }

    // Lock-Free MPMC Vyukov-style Sequence Push with non-blocking oldest-drop on full ring
    void push(const Entry& e) noexcept {
        if (!ring || !ready_flags) return;
        uint64_t w = write_head.fetch_add(1, std::memory_order_acq_rel);
        uint64_t r = read_head.load(std::memory_order_acquire);
        while (w - r >= static_cast<uint64_t>(Capacity)) {
            uint64_t next_r  = r + 1;
            uint64_t old_idx = map_index(r);
            store_ready_flag(&ready_flags[old_idx], 0);
            if (read_head.compare_exchange_weak(r, next_r, std::memory_order_acq_rel, std::memory_order_acquire)) {
                r = next_r;
            }
        }
        uint64_t idx = map_index(w);
        store_ready_flag(&ready_flags[idx], 0);
        Entry tmp = e;
        if constexpr (requires { tmp.ready; }) {
            tmp.ready = 1;
        }
        ring[idx] = tmp;
        store_ready_flag(&ready_flags[idx], 1);
    }

    size_t drain(void (*cb)(const Entry*, void*), void* user) {
        if (!ring || !ready_flags || !cb) return 0;
        sf_ticket_guard_t dg(drain_lock);
        size_t drained = 0;
        uint64_t w = write_head.load(std::memory_order_acquire);
        uint64_t r = read_head.load(std::memory_order_relaxed);
        while (r < w) {
            uint64_t idx = map_index(r);
            if (load_ready_flag(&ready_flags[idx]) != 1) {
                uint32_t spins = 0;
                while (load_ready_flag(&ready_flags[idx]) != 1 && ++spins < 256) {
                    sf_ring_cpu_pause();
                }
                if (load_ready_flag(&ready_flags[idx]) != 1) {
                    break;
                }
            }
            Entry local_copy = ring[idx];
            store_ready_flag(&ready_flags[idx], 0);
            uint64_t expected_r = r;
            if (read_head.compare_exchange_strong(expected_r, r + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                ++r;
                ++drained;
                cb(&local_copy, user);
            } else {
                r = expected_r;
            }
        }
        return drained;
    }

    // Non-destructive snapshot of up to `max_entries` recent entries (for Chrome Tracing / Perfetto export)
    size_t snapshot_recent(std::vector<Entry>& out_entries, size_t max_entries = 4096) const {
        out_entries.clear();
        if (!ring) return 0;
        uint64_t w = write_head.load(std::memory_order_acquire);
        uint64_t start = (w > static_cast<uint64_t>(Capacity)) ? (w - static_cast<uint64_t>(Capacity)) : 0;
        if (w - start > max_entries) {
            start = w - max_entries;
        }
        out_entries.reserve(static_cast<size_t>(w - start));
        for (uint64_t seq = start; seq < w; ++seq) {
            uint64_t idx = map_index(seq);
            Entry snap = ring[idx];
            if (snap.ts_ns != 0) {
                out_entries.push_back(snap);
            }
        }
        return out_entries.size();
    }

    uint64_t pending_count() const noexcept {
        uint64_t w = write_head.load(std::memory_order_acquire);
        uint64_t r = read_head.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : 0;
    }

    void reset() noexcept {
        sf_ticket_guard_t dg(drain_lock);
        write_head.store(0, std::memory_order_relaxed);
        read_head.store(0, std::memory_order_relaxed);
        if (ring && ready_flags) {
            std::memset(ready_flags, 0, static_cast<size_t>(Capacity));
        }
    }
};

template<typename Entry, uint32_t Capacity>
using sfRing = sacculus_ring_t<Entry, Capacity>;

using sacculus_hotlog_t  = sacculus_ring_t<SF_custominiVm_hotlog_entry_t, SACFOR_HOTLOG_SIZE>;
using sacculus_coldlog_t = sacculus_ring_t<SF_custominiVm_coldlog_entry_t, SACFOR_COLDLOG_SIZE>;
using sfHotlog           = sacculus_hotlog_t;
using sfColdlog          = sacculus_coldlog_t;

/* ========================================================================= */
/* 3. EXTENDED STRING STORE, PRE-BOOT CAPTURE & HOT->COLD DRAIN ENGINE       */
/* ========================================================================= */

typedef void (*sf_raw_log_stream_fn_t)(const char* text, size_t len, void* user_data);
typedef void (*sf_hotlog_drain_cb_t)(const SF_custominiVm_hotlog_entry_t* entry, void* user_data);
typedef void (*sf_coldlog_drain_cb_t)(const SF_custominiVm_coldlog_entry_t* entry, void* user_data);

uint16_t    sf_ring_store_ext_string(const char* full_text) noexcept;
const char* sf_ring_lookup_ext_string(uint16_t slot) noexcept;

void   sf_ring_bind_active_logs(sacculus_hotlog_t* hl, sacculus_coldlog_t* cl);
sacculus_hotlog_t*  sf_ring_get_active_hotlog() noexcept;
sacculus_coldlog_t* sf_ring_get_active_coldlog() noexcept;

void   sf_ring_hotlog_write(sacculus_hotlog_t* log,
                            SF_custominiVm_event_category_e ev,
                            SF_custominiVm_severity_e sev,
                            uint32_t fn_id, uint16_t slot, uint8_t gen, uint8_t reg,
                            uint16_t core, uint64_t sweep_no, uint32_t heap_kb,
                            const char* msg,
                            bool broadcast_raw = false);

void   sf_ring_coldlog_write(sacculus_coldlog_t* log,
                             SF_custominiVm_event_category_e ev,
                             SF_custominiVm_severity_e sev,
                             uint32_t fn_id,
                             const char* msg,
                             bool broadcast_raw = false);

void   sf_ring_unified_emit(SF_custominiVm_event_category_e ev,
                            SF_custominiVm_severity_e sev,
                            uint32_t fn_id,
                            const char* short_tag,
                            const char* full_text,
                            bool broadcast_raw = false);

size_t sf_flush_preboot_hotlog(sacculus_hotlog_t* hl, sacculus_coldlog_t* cl);
size_t drain_hotlog_to_coldlog(sacculus_hotlog_t* hl, sacculus_coldlog_t* cl);

static inline void sacculus_hotlog_write(sacculus_hotlog_t* log,
                                         uint8_t ev, uint8_t sev,
                                         uint32_t fid, uint16_t slot,
                                         uint8_t gen, uint8_t reg,
                                         uint16_t core, uint64_t sweep,
                                         uint32_t val, const char* msg,
                                         bool force_flush = false) {
    if (log) {
        sf_ring_hotlog_write(log, static_cast<SF_custominiVm_event_category_e>(ev),
                             static_cast<SF_custominiVm_severity_e>(sev),
                             fid, slot, gen, reg, core, sweep, val, msg, force_flush);
    } else {
        sf_hotlog_emit(static_cast<SF_custominiVm_event_category_e>(ev),
                       static_cast<SF_custominiVm_severity_e>(sev),
                       fid, slot, gen, reg, core, sweep, val, msg);
    }
}

static inline void sacculus_hotlog_write(sacculus_hotlog_t* log,
                                         SF_custominiVm_event_category_e ev,
                                         SF_custominiVm_severity_e sev,
                                         uint32_t fid, uint16_t slot,
                                         uint8_t gen, uint8_t reg,
                                         uint16_t core, uint64_t sweep,
                                         uint32_t val, const char* msg,
                                         bool force_flush = false) {
    if (log) {
        sf_ring_hotlog_write(log, ev, sev, fid, slot, gen, reg, core, sweep, val, msg, force_flush);
    } else {
        sf_hotlog_emit(ev, sev, fid, slot, gen, reg, core, sweep, val, msg);
    }
}



#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API void     SF_custominiVm_set_raw_log_stream(sf_raw_log_stream_fn_t fn, void* user_data);
MSIMD_API void     SF_custominiVm_set_console_mirroring(int enable);
MSIMD_API int      SF_custominiVm_get_console_mirroring(void);
MSIMD_API void     SF_custominiVm_set_telemetry_filter_mask(uint32_t mask);
MSIMD_API uint32_t SF_custominiVm_get_telemetry_filter_mask(void);
MSIMD_API size_t   SF_custominiVm_hotlog_drain(sf_hotlog_drain_cb_t cb, void* user_data);
MSIMD_API size_t   SF_custominiVm_coldlog_drain(sf_coldlog_drain_cb_t cb, void* user_data);
MSIMD_API size_t   SF_custominiVm_drain_hot_to_cold(void);
MSIMD_API size_t   SF_custominiVm_export_chrome_trace_json(char* out_buf, size_t buf_cap, size_t max_entries);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETRINGLOG_HPP
