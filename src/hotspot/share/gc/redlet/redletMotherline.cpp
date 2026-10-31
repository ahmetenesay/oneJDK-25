/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:4400-4700.
 */

#include "gc/redlet/redletMotherline.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

/* ========================================================================= */
/* REGION & ROOM OPERATIONS                                                  */
/* ========================================================================= */

void sf_region_t::place(sfLiveHeader* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> g(lock);
    obj->region = static_cast<uint8_t>(id);
    obj->next = head;
    obj->prev = nullptr;
    if (head) head->prev = obj;
    head = obj;

    count.fetch_add(1, std::memory_order_relaxed);
    total_bytes.fetch_add(obj->memory_footprint.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void sf_region_t::evict(sfLiveHeader* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> g(lock);
    if (obj->prev) obj->prev->next = obj->next;
    if (obj->next) obj->next->prev = obj->prev;
    if (head == obj) head = obj->next;
    obj->next = nullptr;
    obj->prev = nullptr;

    count.fetch_sub(1, std::memory_order_relaxed);
    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    size_t cur = total_bytes.load(std::memory_order_relaxed);
    while (cur > 0) {
        size_t next_b = (cur >= fp) ? (cur - fp) : 0;
        if (total_bytes.compare_exchange_weak(cur, next_b, std::memory_order_relaxed)) break;
    }
}

void sf_room_t::init(sf_room_id_t rid) {
    id = rid;
    for (int i = 0; i < SACFOR_REGION_COUNT; ++i) {
        regions[i].id = static_cast<sf_region_id_t>(i);
    }
}

void sf_room_t::place(sfLiveHeader* obj) {
    if (!obj) return;
    obj->generation = static_cast<uint8_t>(id);
    uint8_t reg = obj->compute_target_region();
    if (reg >= SACFOR_REGION_COUNT) reg = 0;
    regions[reg].place(obj);
    total_objects.fetch_add(1, std::memory_order_relaxed);
    total_bytes.fetch_add(obj->memory_footprint.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void sf_room_t::evict(sfLiveHeader* obj) {
    if (!obj) return;
    uint8_t reg = obj->region;
    if (reg < SACFOR_REGION_COUNT) {
        regions[reg].evict(obj);
    }
    total_objects.fetch_sub(1, std::memory_order_relaxed);
    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    size_t cur = total_bytes.load(std::memory_order_relaxed);
    while (cur > 0) {
        size_t next_b = (cur >= fp) ? (cur - fp) : 0;
        if (total_bytes.compare_exchange_weak(cur, next_b, std::memory_order_relaxed)) break;
    }
}

/* ========================================================================= */
/* REDLET MOTHERLINE IMPLEMENTATION                                          */
/* ========================================================================= */

RedletMotherline::RedletMotherline() {
    init();
}

RedletMotherline::~RedletMotherline() = default;

void RedletMotherline::init() {
    _egg_count.store(0, std::memory_order_relaxed);
    _fledgling_count.store(0, std::memory_order_relaxed);
    for (int i = 0; i < SACFOR_ROOM_COUNT; ++i) {
        _rooms[i].init(static_cast<sf_room_id_t>(i));
    }
}

void RedletMotherline::admit(sfLiveHeader* obj) {
    if (!obj) return;
    _rooms[SF_ROOM_YOUNG].place(obj);
    if (obj->has_optimized.load(std::memory_order_relaxed) == 0) {
        _egg_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        _fledgling_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void RedletMotherline::decrement_counts(sfLiveHeader* obj) {
    if (!obj) return;
    if (obj->has_optimized.load(std::memory_order_relaxed) == 0) {
        size_t c = _egg_count.load(std::memory_order_relaxed);
        while (c > 0 && !_egg_count.compare_exchange_weak(c, c - 1, std::memory_order_relaxed)) {}
    } else {
        size_t c = _fledgling_count.load(std::memory_order_relaxed);
        while (c > 0 && !_fledgling_count.compare_exchange_weak(c, c - 1, std::memory_order_relaxed)) {}
    }
}

void RedletMotherline::admit_egg_object(sfLiveHeader* obj) {
    if (!obj) return;
    obj->has_optimized.store(0, std::memory_order_relaxed);
    obj->opt_success = 0;
    admit(obj);
    sf_hotlog_emit(SF_Ebin0_MOTHERLINE_EGG_ADMIT, SF_Ebin1_TRACE, obj->function_id, 0,
                   obj->generation, obj->region, 0, 0,
                   (uint32_t)(obj->memory_footprint.load(std::memory_order_relaxed) / 1024),
                   "egg_admitted");
}

void RedletMotherline::promote_to_fledgling(sfLiveHeader* obj) {
    if (!obj) return;
    if (obj->has_optimized.load(std::memory_order_relaxed) == 1) return;
    obj->has_optimized.store(1, std::memory_order_release);
    obj->opt_success = 1;

    size_t c = _egg_count.load(std::memory_order_relaxed);
    while (c > 0 && !_egg_count.compare_exchange_weak(c, c - 1, std::memory_order_relaxed)) {}
    _fledgling_count.fetch_add(1, std::memory_order_relaxed);

    sf_hotlog_emit(SF_Ebin0_MOTHERLINE_FLEDGLING, SF_Ebin1_INFO, obj->function_id,
                   obj->active_simd_idx, obj->generation, obj->region,
                   obj->current_core.load(std::memory_order_relaxed), 0,
                   (uint32_t)(obj->memory_footprint.load(std::memory_order_relaxed) / 1024),
                   "fledgling_promoted");
}

void RedletMotherline::evict(sfLiveHeader* obj) {
    if (!obj) return;
    decrement_counts(obj);
    if (obj->generation < SACFOR_ROOM_COUNT) {
        _rooms[obj->generation].evict(obj);
    }
}

/* ========================================================================= */
/* REDLET LANDFILL IMPLEMENTATION                                            */
/* ========================================================================= */

RedletLandfill::RedletLandfill() = default;

RedletLandfill::~RedletLandfill() = default;

void RedletLandfill::admit_quarantine(sfLiveHeader* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> g(_lock);
    obj->state.store(static_cast<uint8_t>(sf_live_state_e::QUARANTINED), std::memory_order_release);
    obj->next = _head;
    obj->prev = nullptr;
    if (_head) _head->prev = obj;
    _head = obj;

    _quarantine_count.fetch_add(1, std::memory_order_relaxed);
    _quarantine_bytes.fetch_add(obj->memory_footprint.load(std::memory_order_relaxed), std::memory_order_relaxed);
    sf_hotlog_emit(SF_Ebin0_TO_LANDFILL, SF_Ebin1_WARN, obj->function_id, 0,
                   obj->generation, obj->region, 0, 0,
                   (uint32_t)(obj->memory_footprint.load(std::memory_order_relaxed) / 1024),
                   "landfill_quarantined");
}

bool RedletLandfill::is_critical(float critical_ratio) const {
    (void)critical_ratio;
    return _quarantine_count.load(std::memory_order_relaxed) > 10000;
}

void RedletLandfill::evacuate(uint64_t sweep_no) {
    std::lock_guard<std::mutex> g(_lock);
    sf_hotlog_emit(SF_Ebin0_LANDFILL_EVAC, SF_Ebin1_WARN, 0, 0, 0, 0, 0, sweep_no,
                   (uint32_t)(_quarantine_bytes.load(std::memory_order_relaxed) / 1024), "landfill_evacuated");
    _head = nullptr;
    _quarantine_count.store(0, std::memory_order_relaxed);
    _quarantine_bytes.store(0, std::memory_order_relaxed);
}
