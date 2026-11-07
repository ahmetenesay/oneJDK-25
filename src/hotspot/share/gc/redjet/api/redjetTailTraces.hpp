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

#ifndef SHARE_GC_REDJET_API_REDJETTAILTRACES_HPP
#define SHARE_GC_REDJET_API_REDJETTAILTRACES_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// TailTraces: Dead Reference Clasp Signaling to /heap/on_heap/rpss
// Controlled by -XX:+RedjetGCUseDeadReferenceSignals (PLAN.md Section 9.4)
// Thread Clasp Lifecycle, False-Positive Protection (Escape-Bound + 2-Phase Morgue)
// ============================================================================

enum class RedjetClaspState : uint8_t {
    CLASP_UNATTACHED = 0,
    CLASP_ACTIVE     = 1,
    CLASP_PROBATION  = 2,
    CLASP_DETACHING  = 3,
    CLASP_TERMINATED = 4
};

enum class RedjetSignalPhase : uint8_t {
    PHASE_1_PROBATION    = 0, // Staged, waiting for escape-bound verification
    PHASE_2_CONDEMNATION = 1, // Escape-bound cleared, staged for morgue
    PHASE_CANCELLED      = 2, // Escape detected or resurrected: signal discarded
    PHASE_COMMITTED      = 3  // Irreversibly reclaimed
};

struct alignas(64) RedjetClaspSignal {
    std::atomic<uint64_t> dead_oop_address;
    uint32_t              thread_id;
    uint32_t              region_id;
    uint64_t              timestamp_ns;
    uint64_t              probation_epoch;
    uint64_t              bpu_site_hash{ 0 };
    uint8_t               bpu_confidence_pct{ 0 };
    bool                  bpu_fast_probation{ false };
    std::atomic<uint8_t>  phase{ static_cast<uint8_t>(RedjetSignalPhase::PHASE_1_PROBATION) };
    std::atomic<bool>     escape_detected{ false };
};

struct alignas(64) RedjetThreadClaspDescriptor {
    uint32_t              thread_id{ 0 };
    std::atomic<uint8_t>  state{ static_cast<uint8_t>(RedjetClaspState::CLASP_UNATTACHED) };
    uint64_t              birth_timestamp_ns{ 0 };
    uint64_t              detach_timestamp_ns{ 0 };
    std::atomic<uint64_t> signals_emitted{ 0 };
    std::atomic<uint64_t> false_positives_filtered{ 0 };
};

class RedjetTailTraces {
public:
    static const size_t RPSS_RING_CAPACITY = 4096;
    static const size_t MAX_TRACKED_THREADS = 1024;
    static constexpr uint64_t DEFAULT_PROBATION_EPOCH_DELAY = 2;

private:
    RedjetClaspSignal                 _rpss_ring[RPSS_RING_CAPACITY];
    alignas(64) std::atomic<uint64_t> _write_cursor{ 0 };
    alignas(64) std::atomic<uint64_t> _read_cursor{ 0 };
    std::atomic<uint64_t>             _total_signals_received{ 0 };
    std::atomic<uint64_t>             _total_signals_drained{ 0 };
    std::atomic<uint64_t>             _total_false_positives_cancelled{ 0 };
    std::atomic<uint64_t>             _total_quarantined_in_morgue{ 0 };
    std::atomic<uint64_t>             _total_rehabilitated_objects{ 0 };
    std::atomic<uint64_t>             _total_contention_events{ 0 };
    std::atomic<uint64_t>             _current_tailtraces_epoch{ 1 };

    // Thread Clasp Lifecycle Registry
    RedjetThreadClaspDescriptor       _clasps[MAX_TRACKED_THREADS];
    std::atomic<size_t>               _active_clasp_count{ 0 };
    hs_ticket_lock_t                  _clasp_lock;

    // Checks whether dead reference signals flag is active
    static bool is_dead_reference_signals_enabled() noexcept;

    // Escape-Bound Analyzer: checks if object escaped to global or cross-thread visibility
    bool is_object_escape_bound(void* dead_obj, uint32_t thread_id) const noexcept;

public:
    RedjetTailTraces();

    // Thread Clasp Lifecycle: Attach on thread birth, detach on thread termination
    RJ_API void register_thread_clasp(uint32_t thread_id);
    RJ_API void detach_thread_clasp(uint32_t thread_id);
    RJ_API bool is_clasp_active(uint32_t thread_id) const noexcept;

    // Emits dead reference signal when Liveness Dead End is reached (Zero STW)
    RJ_API void emit_dead_reference_signal(uint32_t thread_id, void* dead_obj, uint32_t region_id);

    // Two-Phase Morgue Epoch Processing: Advances epoch and commits probation signals
    RJ_API size_t advance_epoch_and_process_probation(uint64_t new_epoch);

    // Invalidate signal if object is accessed or resurrected (False-Positive Protection)
    RJ_API bool cancel_signal_on_escape(void* obj_ptr);

    // Drains pending confirmed signals into Redjet GC mark bitmap / landfill
    RJ_API size_t drain_signals(void (*reclaim_callback)(void* obj, uint32_t region_id));

    // Branchless Clasp Emission (MOV [r15 + clasp_offset], id)
    RJ_API static inline void emit_branchless_clasp(uint32_t thread_clasp_id,
                                                    uintptr_t r15_thread_addr,
                                                    size_t clasp_offset) noexcept {
      volatile uint32_t* target = reinterpret_cast<volatile uint32_t*>(r15_thread_addr + clasp_offset);
#if defined(_MSC_VER) && (defined(_M_X64) || defined(__x86_64__))
      *target = thread_clasp_id;
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(_M_X64))
      __asm__ __volatile__ (
        "movl %0, (%1, %2)"
        :
        : "r"(thread_clasp_id), "r"(r15_thread_addr), "r"(clasp_offset)
        : "memory"
      );
#else
      *target = thread_clasp_id;
#endif
    }

    // 2-Stage Morgue Quarantine in Falline with 1-epoch retention
    RJ_API bool stage_morgue_quarantine_falline(void* dead_obj, size_t footprint_bytes, uint32_t thread_id);

    // Contention Detection on Clasp Rings
    RJ_API bool detect_clasp_contention(uint32_t thread_id) noexcept;

    // Object Rehabilitation from Morgue Quarantine
    RJ_API bool rehabilitate_quarantined_object(void* obj_ptr);

    // Telemetry
    RJ_API uint64_t total_signals() const { return _total_signals_received.load(std::memory_order_relaxed); }
    RJ_API uint64_t pending_signals() const;
    RJ_API uint64_t false_positives_cancelled() const { return _total_false_positives_cancelled.load(std::memory_order_relaxed); }
    RJ_API uint64_t current_epoch() const { return _current_tailtraces_epoch.load(std::memory_order_relaxed); }
    RJ_API size_t   active_clasps() const { return _active_clasp_count.load(std::memory_order_relaxed); }
    RJ_API uint64_t quarantined_in_morgue() const { return _total_quarantined_in_morgue.load(std::memory_order_relaxed); }
    RJ_API uint64_t rehabilitated_objects() const { return _total_rehabilitated_objects.load(std::memory_order_relaxed); }
    RJ_API uint64_t contention_events() const { return _total_contention_events.load(std::memory_order_relaxed); }
};

extern RedjetTailTraces g_redjet_tail_traces;

#endif // SHARE_GC_REDJET_API_REDJETTAILTRACES_HPP
