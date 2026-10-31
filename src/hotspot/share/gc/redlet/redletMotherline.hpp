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

#ifndef SHARE_GC_REDLET_REDLETMOTHERLINE_HPP
#define SHARE_GC_REDLET_REDLETMOTHERLINE_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <utility>
#include <new>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletSIMD.hpp"
#include "gc/redlet/redletIntentCLI.hpp"

void* sf_os_reserve_memory(size_t bytes);
bool  sf_os_commit_memory(void* addr, size_t bytes);
void  sf_os_decommit_memory(void* addr, size_t bytes);
void  sf_os_release_memory(void* addr, size_t bytes);

template<typename Entry, uint32_t Capacity>
struct sacculus_ring_t;
struct SF_custominiVm_hotlog_entry_t;
#ifndef SACFOR_HOTLOG_SIZE
#define SACFOR_HOTLOG_SIZE 65536u
#endif
using sacculus_hotlog_t = sacculus_ring_t<SF_custominiVm_hotlog_entry_t, SACFOR_HOTLOG_SIZE>;

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

#ifndef SACFOR_MAX_CORES
#define SACFOR_MAX_CORES 256
#endif

#ifndef SACFOR_CORE_WORDS
#define SACFOR_CORE_WORDS 4
#endif

#ifndef SACFOR_MAX_INSTANCES
#define SACFOR_MAX_INSTANCES 65535
#endif

#ifndef SACFOR_LANDFILL_SCALE
#define SACFOR_LANDFILL_SCALE 2
#endif

#ifndef SACFOR_LANDFILL_INIT
#define SACFOR_LANDFILL_INIT (32 * 1024 * 1024)
#endif

#ifndef SACFOR_LANDFILL_CRIT
#define SACFOR_LANDFILL_CRIT 0.85f
#endif

#ifndef SACFOR_HEAT_GAIN
#define SACFOR_HEAT_GAIN 1.0f
#endif

#ifndef SACFOR_HEAT_DECAY
#define SACFOR_HEAT_DECAY 0.95f
#endif

#ifndef SACFOR_HEAT_HOT
#define SACFOR_HEAT_HOT 5.0f
#endif

#ifndef SACFOR_OVERHEAD_MIDDLE
#define SACFOR_OVERHEAD_MIDDLE 500
#endif

#ifndef SACFOR_OVERHEAD_DEMAND
#define SACFOR_OVERHEAD_DEMAND 100
#endif

#ifndef SACFOR_IDLE_YOUNG_NS
#define SACFOR_IDLE_YOUNG_NS 100000000ULL
#endif

#ifndef SACFOR_IDLE_ADOL_NS
#define SACFOR_IDLE_ADOL_NS 500000000ULL
#endif

#ifndef SACFOR_IDLE_SENIOR_NS
#define SACFOR_IDLE_SENIOR_NS 2000000000ULL
#endif

#ifndef SF_PRETOUCH_MEMORY_DEFINED
#define SF_PRETOUCH_MEMORY_DEFINED
static inline void sf_pretouch_memory(void* base, size_t size, size_t page_stride = 4096) {
    if (!base || size == 0) return;
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(base);
    for (size_t off = 0; off < size; off += page_stride) {
        volatile uint8_t dummy = p[off];
        p[off] = dummy;
    }
}
#endif

/* ========================================================================= */
/* 1. CORE BITMAP UTILITIES                                                  */
/* ========================================================================= */

static inline void sacculus_core_bitmap_set(std::atomic<uint64_t> bm[SACFOR_CORE_WORDS], uint16_t core) {
    if (core >= SACFOR_MAX_CORES) return;
    bm[core / 64].fetch_or(1ULL << (core % 64), std::memory_order_relaxed);
}

static inline void sacculus_core_bitmap_set(uint64_t bm[SACFOR_CORE_WORDS], uint16_t core) {
    if (core >= SACFOR_MAX_CORES) return;
    bm[core / 64] |= (1ULL << (core % 64));
}

static inline int sacculus_core_bitmap_test(const std::atomic<uint64_t> bm[SACFOR_CORE_WORDS], uint16_t core) {
    if (core >= SACFOR_MAX_CORES) return 0;
    return (int)((bm[core / 64].load(std::memory_order_relaxed) >> (core % 64)) & 1u);
}

static inline int sacculus_core_bitmap_test(const uint64_t bm[SACFOR_CORE_WORDS], uint16_t core) {
    if (core >= SACFOR_MAX_CORES) return 0;
    return (int)((bm[core / 64] >> (core % 64)) & 1u);
}

static inline uint32_t sacculus_core_bitmap_count(const std::atomic<uint64_t> bm[SACFOR_CORE_WORDS]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SACFOR_CORE_WORDS; ++i) {
        uint64_t w = bm[i].load(std::memory_order_relaxed);
        while (w) { n++; w &= w - 1; }
    }
    return n;
}

static inline uint32_t sacculus_core_bitmap_count(const uint64_t bm[SACFOR_CORE_WORDS]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SACFOR_CORE_WORDS; ++i) {
        uint64_t x = bm[i];
        while (x) { n++; x &= x - 1; }
    }
    return n;
}

/* ========================================================================= */
/* 2. ENUMS & MAGIC DEFINITIONS                                              */
/* ========================================================================= */

#define SF_LIVE_MAGIC   0x53464D4EU
#define SF_TAIL_CANARY  0xDEADBEEFCAFEF00DULL

enum class sf_live_state_e : uint8_t {
    LIVE            = 0,
    TRANSIT_FALLINE = 1,
    TRANSIT_REHAB   = 2,
    IN_FALLINE      = 3,
    IN_LANDFILL     = 4,
    QUARANTINED     = 5,
    DEAD            = 6,
    ALLOCATED       = 0,
    PROMOTED        = 7,
    DEMOTED         = 8
};

typedef enum sf_death_reason_e : uint8_t {
    SF_DEATH_IDLE         = 0x01,
    SF_DEATH_PRESSURE     = 0x02,
    SF_DEATH_ERROR        = 0x03,
    SF_DEATH_EXPLICIT     = 0x04,
    SF_DEATH_REHABILITATE = 0x05,
} sf_death_reason_e;

using sfDeathReason = sf_death_reason_e;
using sacculus_death_reason_t = sf_death_reason_e;

#ifndef SF_SIMD_LADDER_SLOT_T_DEFINED
#define SF_SIMD_LADDER_SLOT_T_DEFINED
struct sf_simd_ladder_slot_t {
    char     name[32]{ 0 };
    uint32_t ladder_level{ 0 };
    uint8_t  is_active{ 0 };
    uint8_t  _pad[3]{ 0 };
};
#endif

static inline uint32_t sf_compute_crc32(const void* data, size_t len) {
    if (!data || len == 0) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0 - (crc & 1)));
        }
    }
    return ~crc;
}

/* ========================================================================= */
/* 3. COLD OBJECT METADATA                                                   */
/* ========================================================================= */

struct sf_live_cold_metadata_t {
    std::atomic<uint64_t> per_core_cycles[256];
    std::atomic<uint64_t> visited_cores[4];
    char                  debug_name[24];
    uint64_t              type_sig_hash{ 0 };
    uint64_t              birth_ns{ 0 };
    std::atomic<uint64_t> age_ns{ 0 };
    std::atomic<uint64_t> phase_count{ 0 };
    std::atomic<uint64_t> bitop_count{ 0 };
    std::atomic<uint64_t> avg_cycles_per_call{ 0 };
    std::atomic<uint32_t> call_step_count{ 0 };
    std::atomic<uint32_t> error_count{ 0 };
    uint64_t              pagefile_slot_offset{ 0 };
    std::atomic<uint8_t>  is_paged_out{ 0 };
    std::atomic<float>    per_numa_heat[4];
    uint8_t               preferred_numa_node{ 0 };

    sf_live_cold_metadata_t();
    sf_live_cold_metadata_t(const sf_live_cold_metadata_t& o);
    sf_live_cold_metadata_t& operator=(const sf_live_cold_metadata_t& o);
};

struct SF_custominiVm_live_header_t;
using sfLiveHeader = SF_custominiVm_live_header_t;
using sacculus_live_header_t = sfLiveHeader;

static inline sf_live_cold_metadata_t* get_cold(SF_custominiVm_live_header_t* hdr);
static inline const sf_live_cold_metadata_t* get_cold_ro(const SF_custominiVm_live_header_t* hdr);
static inline uint32_t sf_get_call_step_count(const SF_custominiVm_live_header_t* hdr);
static inline const char* sf_get_debug_name(const SF_custominiVm_live_header_t* hdr);

/* ========================================================================= */
/* 4. PIN GUARD RAII                                                         */
/* ========================================================================= */

struct sacculus_pin_guard_t {
    SF_custominiVm_live_header_t* obj{ nullptr };
    bool                          acquired{ false };

    explicit sacculus_pin_guard_t(SF_custominiVm_live_header_t* o);
    bool is_valid() const noexcept { return obj != nullptr && acquired; }
    ~sacculus_pin_guard_t();
    sacculus_pin_guard_t(const sacculus_pin_guard_t&) = delete;
    sacculus_pin_guard_t& operator=(const sacculus_pin_guard_t&) = delete;
    sacculus_pin_guard_t(sacculus_pin_guard_t&& other) noexcept;
    sacculus_pin_guard_t& operator=(sacculus_pin_guard_t&& other) noexcept;
};

using sfPinGuard = sacculus_pin_guard_t;
using sf_pin_guard_t = sacculus_pin_guard_t;

/* ========================================================================= */
/* 5. LIVE HEADER (SF_custominiVm_live_header_t)                             */
/* ========================================================================= */

struct alignas(SACFOR_CACHE_LINE) SF_custominiVm_live_header_t {
    std::atomic<sf_live_cold_metadata_t*> cold_meta{ nullptr };

    uint32_t              sfMN{ SF_LIVE_MAGIC };
    size_t                total_cell_bytes{ 0 };
    uint32_t              function_id{ 0 };

    std::atomic<uint64_t> total_cycles{ 0 };
    std::atomic<uint16_t> current_core{ 0xFFFF };
    std::atomic<uint64_t> last_access_ns{ 0 };
    std::atomic<uint64_t> access_count{ 0 };
    std::atomic<int32_t>  pin_count{ 0 };
    std::atomic<uint8_t>  state{ static_cast<uint8_t>(sf_live_state_e::LIVE) };

    std::atomic<size_t>   memory_footprint{ 0 };
    std::atomic<float>    heat_score{ 1.0f };
    uint8_t               generation{ 0 };
    uint8_t               region{ 0 };
    std::atomic<uint8_t>  gc_lock{ 0 };
    uint8_t               priority{ 5 };

    std::atomic<uint16_t> instance_count{ 0 };
    uint16_t              instance_cap{ SACFOR_MAX_INSTANCES };
    uint8_t               gc_sweep_count{ 0 };
    uint8_t               falline_marked{ 0 };
    uint8_t               needs_rehabilitation{ 0 };
    uint8_t               _pad0[1]{ 0 };

    /* Puzzler & Optimization State Fields */
    std::atomic<uint8_t>  has_optimized{ 0 };
    uint8_t               opt_permitted{ 1 };
    uint8_t               opt_success{ 0 };
    uint8_t               group_priority{ 5 };
    uint32_t              group_hex_id{ 0 };
    std::atomic<uint64_t> last_optimized_time{ 0 };
    std::atomic<uint64_t> optimization_duration_cycles{ 0 };
    std::atomic<uint64_t> cycles_gained{ 0 };
    uint16_t              active_simd_idx{ 0 };
    uint16_t              simd_candidate_count{ 0 };
    sf_simd_ladder_slot_t simd_table[8]{};

    std::atomic<void*>    forwarding_address{ nullptr };
    std::atomic<uint32_t> child_view_count{ 0 };
    std::atomic<void*>    parent_header{ nullptr };
    size_t                slice_byte_offset{ 0 };
    int64_t               strides[8]{ 0 };
    uint8_t               is_strided_view{ 0 };

    SF_custominiVm_live_header_t* next{ nullptr };
    SF_custominiVm_live_header_t* prev{ nullptr };

    SF_custominiVm_live_header_t();
    SF_custominiVm_live_header_t(const SF_custominiVm_live_header_t& o);
    SF_custominiVm_live_header_t& operator=(const SF_custominiVm_live_header_t& o);
    ~SF_custominiVm_live_header_t();

    sf_live_cold_metadata_t* get_cold();
    const sf_live_cold_metadata_t* get_cold_ro() const;

    void relocate_or_clone_from(SF_custominiVm_live_header_t* src, bool transfer_cold_ownership);
    void apply_optimized_state_from(const SF_custominiVm_live_header_t& src);
    void init(uint32_t fn_id, uint64_t sig_hash, const char* name, uint8_t prio = 5);
    void set_active_simd_candidate(const char* isa_name, uint32_t level = 0);
    void mark_already_on_best_path();

    bool isValid() const noexcept { return sfMN == SF_LIVE_MAGIC; }
    void record_access(uint64_t cycles, uint16_t core_id);
    void decay_heat();

    bool try_pin();
    void unpin();
    bool is_pinned() const noexcept { return pin_count.load(std::memory_order_acquire) > 0; }

    bool try_transition_to_falline();
    bool try_transition_to_rehab();
    bool is_idle() const;
    uint8_t compute_target_region() const;

    inline uint8_t* body() noexcept {
        size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        return reinterpret_cast<uint8_t*>(this) + hdr_sz;
    }

    inline const uint8_t* body() const noexcept {
        size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        return reinterpret_cast<const uint8_t*>(this) + hdr_sz;
    }

    inline void* tailCanaryPtr() const noexcept;
    void writeTailCanary() noexcept;
    bool isTailCanaryValid() const noexcept;

    void* resolve_active_body() const;

    static inline SF_custominiVm_live_header_t* from_body(void* body_ptr) noexcept {
        if (!body_ptr) return nullptr;
        size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        auto* hdr = reinterpret_cast<SF_custominiVm_live_header_t*>(static_cast<uint8_t*>(body_ptr) - hdr_sz);
        return (hdr->sfMN == SF_LIVE_MAGIC) ? hdr : nullptr;
    }

    static inline size_t total_cell_size(size_t body_bytes) noexcept {
        size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        size_t body_aligned = (body_bytes + 7) & ~7ULL;
        size_t with_canary = body_aligned + sizeof(uint64_t);
        return (hdr_sz + with_canary + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    }
};

static inline sf_live_cold_metadata_t* get_cold(SF_custominiVm_live_header_t* hdr) {
    return hdr ? hdr->get_cold() : nullptr;
}

static inline const sf_live_cold_metadata_t* get_cold_ro(const SF_custominiVm_live_header_t* hdr) {
    return hdr ? hdr->get_cold_ro() : nullptr;
}

static inline uint32_t sf_get_call_step_count(const SF_custominiVm_live_header_t* hdr) {
    const auto* c = get_cold_ro(hdr);
    return c ? c->call_step_count.load(std::memory_order_relaxed) : 0u;
}

static inline const char* sf_get_debug_name(const SF_custominiVm_live_header_t* hdr) {
    const auto* c = get_cold_ro(hdr);
    return (c && c->debug_name[0]) ? c->debug_name : "";
}

/* ========================================================================= */
/* 6. ORDER OF HEIGHT (LOGARITHMIC TIER FINGERS FOR HOTFIELD)                */
/* ========================================================================= */

struct sacculus_order_of_height_t {
    static constexpr size_t NUM_TIER_FINGERS = 16;
    sacculus_live_header_t* head{ nullptr };
    sacculus_live_header_t* tail{ nullptr };
    sacculus_live_header_t* tier_fingers[NUM_TIER_FINGERS]{ nullptr };
    size_t                  count{ 0 };
    sf_ticket_lock_t        lock;

    static inline size_t compute_tier_bucket(uint32_t steps) noexcept {
        if (steps == 0) return 0;
        size_t b = 0;
        while (steps > 1 && b + 1 < NUM_TIER_FINGERS) {
            steps >>= 1;
            ++b;
        }
        return b;
    }

    void insert_nolock(sacculus_live_header_t* obj);
    void insert(sacculus_live_header_t* obj);
    void remove_nolock(sacculus_live_header_t* obj);
    void remove(sacculus_live_header_t* obj);
    void bubble_up_nolock(sacculus_live_header_t* obj);
    void bubble_up(sacculus_live_header_t* obj);
};

using sfOrderOfHeight = sacculus_order_of_height_t;
using sf_order_of_height_t = sacculus_order_of_height_t;

/* ========================================================================= */
/* 7. FIBONACCI INDEXED BUCKET (ROBIN HOOD HASHING)                          */
/* ========================================================================= */

struct sf_fibonacci_indexed_bucket_t {
    static constexpr size_t   BUCKET_CAP = 1024;
    static constexpr size_t   BUCKET_MASK = BUCKET_CAP - 1;
    static constexpr uint64_t FIBONACCI_PRIME = 11400714819323198485ULL;

    sacculus_live_header_t* slots[BUCKET_CAP];
    sacculus_live_header_t* head{ nullptr };
    size_t                  count{ 0 };
    sf_ticket_lock_t        lock;

    sf_fibonacci_indexed_bucket_t();

    inline size_t hash_index(uint32_t fn_id) const noexcept {
        return (size_t)((fn_id * FIBONACCI_PRIME) >> (64 - 10)) & BUCKET_MASK;
    }

    void insert_nolock(sacculus_live_header_t* obj);
    void insert(sacculus_live_header_t* obj);
    void remove_nolock(sacculus_live_header_t* obj);
    void remove(sacculus_live_header_t* obj);
    sacculus_live_header_t* lookup_direct(uint32_t fn_id);
};

using sfFibonacciIndexedBucket = sf_fibonacci_indexed_bucket_t;
using sfUnorderedBucket = sf_fibonacci_indexed_bucket_t;
using sacculus_unordered_bucket_t = sf_fibonacci_indexed_bucket_t;

/* ========================================================================= */
/* 8. REGIONS & ROOMS                                                        */
/* ========================================================================= */

enum sf_region_id_t : uint8_t {
    SF_REGION_DEMAND   = 0,
    SF_REGION_MIDDLE   = 1,
    SF_REGION_TANK     = 2,
    SF_REGION_HOTFIELD = 3
};

#define SACFOR_REGION_DEMAND   SF_REGION_DEMAND
#define SACFOR_REGION_MIDDLE   SF_REGION_MIDDLE
#define SACFOR_REGION_TANK     SF_REGION_TANK
#define SACFOR_REGION_HOTFIELD SF_REGION_HOTFIELD

struct sf_region_t {
    sf_region_id_t                id{ SF_REGION_DEMAND };
    sf_order_of_height_t          hot_order;
    sf_fibonacci_indexed_bucket_t bucket;
    std::atomic<size_t>           total_bytes{ 0 };
    std::atomic<size_t>           count{ 0 };

    sf_ticket_lock_t& get_lock() noexcept {
        return (id == SF_REGION_HOTFIELD) ? hot_order.lock : bucket.lock;
    }

    sfLiveHeader* get_head() noexcept {
        return (id == SF_REGION_HOTFIELD) ? hot_order.head : bucket.head;
    }

    void place_nolock(sfLiveHeader* obj);
    void place(sfLiveHeader* obj);
    void evict_nolock(sfLiveHeader* obj);
    void evict(sfLiveHeader* obj);
};

using sfRoomId = sf_room_id_t;
using sacculus_room_id_t = sf_room_id_t;

#define SACFOR_ROOM_YOUNG      SF_ROOM_YOUNG
#define SACFOR_ROOM_ADOLESCENT SF_ROOM_ADOLESCENT
#define SACFOR_ROOM_SENIOR     SF_ROOM_SENIOR

struct sf_room_t {
    sf_room_id_t        id{ SF_ROOM_YOUNG };
    sf_region_t         regions[SACFOR_REGION_COUNT];
    std::atomic<size_t> total_objects{ 0 };
    std::atomic<size_t> total_bytes{ 0 };

    void init(sf_room_id_t rid);
    void place(sfLiveHeader* obj);
    void evict(sfLiveHeader* obj);
};

using sfRoom = sf_room_t;
using sacculus_room_t = sf_room_t;

/* ========================================================================= */
/* 9. REDLET MOTHERLINE ENGINE                                               */
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
    void admit_egg_object(sfLiveHeader* obj);
    void promote_to_fledgling(sfLiveHeader* obj);
    void evict(sfLiveHeader* obj);
    void decrement_counts(sfLiveHeader* obj);
    void destroy();

    size_t batch_optimize_egg_objects(uint32_t fn_id, const char* new_isa_name, uint16_t new_simd_idx, uint64_t cycles_saved);
    size_t batch_optimize_egg_objects(uint32_t fn_id, uint16_t new_simd_idx, uint64_t cycles_saved) {
        return batch_optimize_egg_objects(fn_id, nullptr, new_simd_idx, cycles_saved);
    }

    size_t egg_count() const noexcept { return _egg_count.load(std::memory_order_relaxed); }
    size_t fledgling_count() const noexcept { return _fledgling_count.load(std::memory_order_relaxed); }
    sf_room_t& room(size_t r) { return _rooms[r]; }
    const sf_room_t& room(size_t r) const { return _rooms[r]; }
};

using sfMotherline = RedletMotherline;
using sf_motherline_t = RedletMotherline;
using sacculus_motherline_t = RedletMotherline;

/* ========================================================================= */
/* 10. LANDFILL HEADERS, ARENAS & DISPOSAL BAY                               */
/* ========================================================================= */

struct SF_custominiVm_landfill_header_t {
    uint32_t          function_id{ 0 };
    uint64_t          type_sig_hash{ 0 };

    uint64_t          birth_ns{ 0 };
    uint64_t          death_ns{ 0 };
    uint64_t          lifetime_ns{ 0 };
    uint64_t          freight_timestamp_ns{ 0 };
    uint64_t          evacuation_duration_ns{ 0 };

    uint64_t          total_cpu_cycles{ 0 };
    uint64_t          per_core_cycles[SACFOR_MAX_CORES]{ 0 };

    size_t            previous_size{ 0 };
    size_t            pit_stored_size{ 0 };
    size_t            compacted_size{ 0 };

    uint8_t           lasting{ 0 };
    uint8_t           header_pane_byte_ref{ 0 };
    uint8_t           peak_generation{ 0 };
    uint8_t           peak_region{ 0 };
    sf_death_reason_e death_reason{ SF_DEATH_IDLE };
    uint8_t           error_count{ 0 };
    uint16_t          last_core{ 0 };
    uint16_t          unique_cores_visited{ 0 };

    uint32_t          crc32{ 0 };
    void*             header_pane_ref{ nullptr };
    size_t            rehab_offset{ 0 };
    void*             rehab_payload_ptr{ nullptr };

    void populate_from_live(const SF_custominiVm_live_header_t* live,
                            sf_death_reason_e reason,
                            uint64_t evac_duration_ns = 0,
                            size_t compacted_sz = 0);
};

using sfLandfillHeader = SF_custominiVm_landfill_header_t;
using sacculus_landfill_header_t = sfLandfillHeader;

struct sf_arena_storage_t {
    std::atomic<uint8_t*> base{ nullptr };
    std::atomic<size_t>   capacity{ 0 };
    std::atomic<size_t>   used{ 0 };
    sf_ticket_lock_t      grow_lock;
    bool                  Advance_Payment{ false };
    std::vector<std::pair<uint8_t*, size_t>> old_bases;

    bool init(size_t initial_bytes, bool pretouch = false);
    void* store(const void* body, size_t size);
    void* store_with_offset(const void* body, size_t size, size_t* out_offset);
    bool grow_nolock(size_t min_needed);
    bool grow();
    float fill_ratio() const noexcept;
    void flush();
    void destroy();
};

struct sf_trash_pit_t {
    sf_arena_storage_t matured_arena;
    sf_arena_storage_t rehab_arena;

    bool init(size_t initial_bytes = SACFOR_LANDFILL_INIT, bool Advance_Payment = false);
    void* store_matured(const void* body, size_t size);
    void* store_rehab(const void* body, size_t size);
    void* store_rehab_with_offset(const void* body, size_t size, size_t* out_offset);
    float max_fill_ratio() const noexcept;
    size_t total_used() const noexcept;
    void flush();
    void destroy();
};

using sfTrashPit = sf_trash_pit_t;
using sacculus_trash_pit_t = sf_trash_pit_t;

struct sacculus_header_pane_t {
    static constexpr uint32_t PANE_CAP = 65536;

    std::unique_ptr<SF_custominiVm_landfill_header_t[]> entries_ptr;
    SF_custominiVm_landfill_header_t* entries{ nullptr };
    std::atomic<uint32_t>             count{ 0 };
    sf_ticket_lock_t                  lock;

    bool init();
    int32_t push(const SF_custominiVm_landfill_header_t& h);
    void flush();
};

using sfHeaderPane = sacculus_header_pane_t;

class RedletLandfill {
private:
    sf_trash_pit_t         _pit;
    sacculus_header_pane_t _pane;

public:
    RedletLandfill();
    ~RedletLandfill();

    bool init(size_t initial = SACFOR_LANDFILL_INIT, bool advance_payment = false);
    bool accept(SF_custominiVm_live_header_t* live,
                sf_death_reason_e reason,
                sacculus_hotlog_t* hotlog = nullptr,
                uint64_t sweep_no = 0,
                uint64_t evac_duration_ns = 0);
    bool is_critical(float critical_ratio = SACFOR_LANDFILL_CRIT) const;
    void evacuate(sacculus_hotlog_t* hotlog = nullptr, uint64_t sweep_no = 0);
    void destroy();

    sf_trash_pit_t& pit() noexcept { return _pit; }
    const sf_trash_pit_t& pit() const noexcept { return _pit; }
    sacculus_header_pane_t& pane() noexcept { return _pane; }
    const sacculus_header_pane_t& pane() const noexcept { return _pane; }
};

using sfLandfill = RedletLandfill;
using sacculus_landfill_t = RedletLandfill;

#endif // SHARE_GC_REDLET_REDLETMOTHERLINE_HPP
