/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:4400-4700.
 */

#ifndef SHARE_GC_REDLET_REDLETMOTHERLINE_HPP
#define SHARE_GC_REDLET_REDLETMOTHERLINE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redlet/redletGearkit.hpp"

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

/* ========================================================================= */
/* 1. LIVE HEADER MAGIC & TAIL CANARY DEFINITIONS                            */
/* ========================================================================= */

#define SF_LIVE_MAGIC   0x53464D4EU
#define SF_TAIL_CANARY  0xDEADBEEFCAFEF00DULL

enum class sf_live_state_e : uint8_t {
    ALLOCATED   = 0,
    LIVE        = 1,
    PROMOTED    = 2,
    DEMOTED     = 3,
    QUARANTINED = 4,
    DEAD        = 5
};

enum sf_region_id_t : uint8_t {
    SF_REGION_DEMAND   = 0,
    SF_REGION_MIDDLE   = 1,
    SF_REGION_TANK     = 2,
    SF_REGION_HOTFIELD = 3,
    SF_REGION_ARENA    = 4,
    SF_REGION_BURST    = 5,
    SF_REGION_STATIC   = 6,
    SF_REGION_SPARE    = 7
};

/* ========================================================================= */
/* 2. SACCULUS LIVE HEADER (sfLiveHeader)                                    */
/* ========================================================================= */

struct alignas(SACFOR_CACHE_LINE) SF_custominiVm_live_header_t {
    uint32_t            sfMN{ SF_LIVE_MAGIC };
    uint32_t            function_id{ 0 };
    std::atomic<void*>  forwarding_address{ nullptr };
    std::atomic<void*>  parent_header{ nullptr };
    std::atomic<size_t> memory_footprint{ 0 };
    size_t              total_cell_bytes{ 0 };

    uint8_t             generation{ 0 }; // Room ID: Young, Adolescent, Senior, Immortal
    uint8_t             region{ 0 };     // Region ID: Demand, Middle, Tank, Hotfield...
    std::atomic<uint8_t> state{ static_cast<uint8_t>(sf_live_state_e::LIVE) };
    uint8_t             opt_success{ 0 };
    std::atomic<uint8_t> has_optimized{ 0 };
    uint16_t            active_simd_idx{ 0 };

    std::atomic<float>  heat_score{ 1.0f };
    uint64_t            gc_sweep_count{ 0 };
    std::atomic<uint16_t> current_core{ 0 };
    std::atomic<int32_t>  pin_count{ 0 };
    size_t                slice_byte_offset{ 0 };

    SF_custominiVm_live_header_t* next{ nullptr };
    SF_custominiVm_live_header_t* prev{ nullptr };

    SF_custominiVm_live_header_t() = default;

    SF_custominiVm_live_header_t(const SF_custominiVm_live_header_t& o) {
        sfMN = o.sfMN;
        function_id = o.function_id;
        forwarding_address.store(o.forwarding_address.load(std::memory_order_relaxed), std::memory_order_relaxed);
        parent_header.store(o.parent_header.load(std::memory_order_relaxed), std::memory_order_relaxed);
        memory_footprint.store(o.memory_footprint.load(std::memory_order_relaxed), std::memory_order_relaxed);
        total_cell_bytes = o.total_cell_bytes;
        generation = o.generation;
        region = o.region;
        state.store(o.state.load(std::memory_order_relaxed), std::memory_order_relaxed);
        opt_success = o.opt_success;
        has_optimized.store(o.has_optimized.load(std::memory_order_relaxed), std::memory_order_relaxed);
        active_simd_idx = o.active_simd_idx;
        heat_score.store(o.heat_score.load(std::memory_order_relaxed), std::memory_order_relaxed);
        gc_sweep_count = o.gc_sweep_count;
        current_core.store(o.current_core.load(std::memory_order_relaxed), std::memory_order_relaxed);
        pin_count.store(o.pin_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        slice_byte_offset = o.slice_byte_offset;
        next = nullptr;
        prev = nullptr;
    }

    SF_custominiVm_live_header_t& operator=(const SF_custominiVm_live_header_t& o) {
        if (this != &o) {
            sfMN = o.sfMN;
            function_id = o.function_id;
            forwarding_address.store(o.forwarding_address.load(std::memory_order_relaxed), std::memory_order_relaxed);
            parent_header.store(o.parent_header.load(std::memory_order_relaxed), std::memory_order_relaxed);
            memory_footprint.store(o.memory_footprint.load(std::memory_order_relaxed), std::memory_order_relaxed);
            total_cell_bytes = o.total_cell_bytes;
            generation = o.generation;
            region = o.region;
            state.store(o.state.load(std::memory_order_relaxed), std::memory_order_relaxed);
            opt_success = o.opt_success;
            has_optimized.store(o.has_optimized.load(std::memory_order_relaxed), std::memory_order_relaxed);
            active_simd_idx = o.active_simd_idx;
            heat_score.store(o.heat_score.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gc_sweep_count = o.gc_sweep_count;
            current_core.store(o.current_core.load(std::memory_order_relaxed), std::memory_order_relaxed);
            pin_count.store(o.pin_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
            slice_byte_offset = o.slice_byte_offset;
        }
        return *this;
    }

    bool try_pin(void) {
        while (true) {
            uint8_t st = state.load(std::memory_order_acquire);
            if (st != static_cast<uint8_t>(sf_live_state_e::LIVE)) {
                return false;
            }
            int32_t pc = pin_count.load(std::memory_order_relaxed);
            if (pin_count.compare_exchange_weak(pc, pc + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
                if (state.load(std::memory_order_acquire) == static_cast<uint8_t>(sf_live_state_e::LIVE)) {
                    return true;
                }
                pin_count.fetch_sub(1, std::memory_order_release);
                return false;
            }
        }
    }

    void unpin(void) {
        int32_t cur = pin_count.load(std::memory_order_relaxed);
        while (cur > 0 && !pin_count.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
    }

    bool is_pinned(void) const {
        return pin_count.load(std::memory_order_acquire) > 0;
    }

    inline void* resolve_active_body(void) const {
        static constexpr int MAX_DEPTH = 16;
        const SF_custominiVm_live_header_t* cur = this;
        size_t accumulated_offset = 0;
        for (int depth = 0; depth < MAX_DEPTH; ++depth) {
            auto* p = static_cast<const SF_custominiVm_live_header_t*>(cur->parent_header.load(std::memory_order_acquire));
            if (!p) {
                void* fwd = cur->forwarding_address.load(std::memory_order_acquire);
                uint8_t* base_ptr = fwd ? static_cast<uint8_t*>(fwd) : const_cast<uint8_t*>(cur->body());
                return base_ptr + accumulated_offset;
            }
            accumulated_offset += cur->slice_byte_offset;
            cur = p;
        }
        return const_cast<uint8_t*>(body());
    }

    inline uint8_t* body() const noexcept {
        size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        return const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(this) + hdr_sz);
    }

    inline void* tailCanaryPtr() const noexcept {
        if (parent_header.load(std::memory_order_relaxed) != nullptr) {
            static uint64_t dummy_canary = SF_TAIL_CANARY;
            return &dummy_canary;
        }
        size_t fp = memory_footprint.load(std::memory_order_relaxed);
        size_t aligned_fp = (fp + 7) & ~7ULL;
        size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        if (total_cell_bytes > 0 && header_sz + aligned_fp + 8 > total_cell_bytes) {
            static uint64_t dummy_canary = SF_TAIL_CANARY;
            return &dummy_canary;
        }
        return body() + aligned_fp;
    }

    inline void writeTailCanary() noexcept {
        if (parent_header.load(std::memory_order_relaxed) != nullptr) return;
        size_t fp = memory_footprint.load(std::memory_order_relaxed);
        size_t aligned_fp = (fp + 7) & ~7ULL;
        size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        if (total_cell_bytes > 0 && header_sz + aligned_fp + 8 > total_cell_bytes) return;
        volatile uint64_t* canary = reinterpret_cast<volatile uint64_t*>(body() + aligned_fp);
        *canary = SF_TAIL_CANARY;
    }

    inline bool isTailCanaryValid() const noexcept {
        if (parent_header.load(std::memory_order_relaxed) != nullptr) return true;
        size_t fp = memory_footprint.load(std::memory_order_relaxed);
        if (fp == 0) return true;
        size_t aligned_fp = (fp + 7) & ~7ULL;
        size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        if (total_cell_bytes > 0 && header_sz + aligned_fp + 8 > total_cell_bytes) return false;
        return *reinterpret_cast<const volatile uint64_t*>(body() + aligned_fp) == SF_TAIL_CANARY;
    }

    inline uint8_t compute_target_region() const noexcept {
        float h = heat_score.load(std::memory_order_relaxed);
        if (h > 5.0f) return SF_REGION_HOTFIELD;
        if (memory_footprint.load(std::memory_order_relaxed) > 1024 * 1024) return SF_REGION_TANK;
        return SF_REGION_DEMAND;
    }

    static inline SF_custominiVm_live_header_t* from_body(void* body_ptr) noexcept {
        if (!body_ptr) return nullptr;
        size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        auto* hdr = reinterpret_cast<SF_custominiVm_live_header_t*>(static_cast<uint8_t*>(body_ptr) - hdr_sz);
        return (hdr->sfMN == SF_LIVE_MAGIC) ? hdr : nullptr;
    }
};

using sfLiveHeader = SF_custominiVm_live_header_t;
using sacculus_live_header_t = SF_custominiVm_live_header_t;

/* ========================================================================= */
/* 3. MOTHERLINE REGIONS & ROOMS                                             */
/* ========================================================================= */

struct sf_region_t {
    sf_region_id_t       id{ SF_REGION_DEMAND };
    sfLiveHeader*        head{ nullptr };
    std::mutex           lock;
    std::atomic<size_t>  total_bytes{ 0 };
    std::atomic<size_t>  count{ 0 };

    void place(sfLiveHeader* obj);
    void evict(sfLiveHeader* obj);
};

struct sf_room_t {
    sf_room_id_t        id{ SF_ROOM_YOUNG };
    sf_region_t         regions[SACFOR_REGION_COUNT];
    std::atomic<size_t> total_objects{ 0 };
    std::atomic<size_t> total_bytes{ 0 };

    void init(sf_room_id_t rid);
    void place(sfLiveHeader* obj);
    void evict(sfLiveHeader* obj);
};

/* ========================================================================= */
/* 4. REDLET MOTHERLINE ENGINE                                               */
/* ========================================================================= */

class RedletMotherline {
private:
    sf_room_t           _rooms[SACFOR_ROOM_COUNT];
    std::atomic<size_t> _egg_count{ 0 };
    std::atomic<size_t> _fledgling_count{ 0 };

public:
    RedletMotherline();
    ~RedletMotherline();

    void init();
    void admit(sfLiveHeader* obj);
    void evict(sfLiveHeader* obj);
    void decrement_counts(sfLiveHeader* obj);
    void admit_egg_object(sfLiveHeader* obj);
    void promote_to_fledgling(sfLiveHeader* obj);

    size_t egg_count() const { return _egg_count.load(std::memory_order_relaxed); }
    size_t fledgling_count() const { return _fledgling_count.load(std::memory_order_relaxed); }
    sf_room_t& room(size_t r) { return _rooms[r]; }
};

using sacculus_motherline_t = RedletMotherline;

/* ========================================================================= */
/* 5. REDLET LANDFILL (QUARANTINE DISPOSAL)                                  */
/* ========================================================================= */

class RedletLandfill {
private:
    sfLiveHeader*       _head{ nullptr };
    std::mutex          _lock;
    std::atomic<size_t> _quarantine_count{ 0 };
    std::atomic<size_t> _quarantine_bytes{ 0 };

public:
    RedletLandfill();
    ~RedletLandfill();

    void admit_quarantine(sfLiveHeader* obj);
    bool is_critical(float critical_ratio = 0.85f) const;
    void evacuate(uint64_t sweep_no);

    size_t quarantine_count() const { return _quarantine_count.load(std::memory_order_relaxed); }
    size_t quarantine_bytes() const { return _quarantine_bytes.load(std::memory_order_relaxed); }
};

using sacculus_landfill_t = RedletLandfill;

#endif // SHARE_GC_REDLET_REDLETMOTHERLINE_HPP
