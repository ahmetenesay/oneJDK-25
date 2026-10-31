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

#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include <new>
#include <algorithm>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#include <smmintrin.h>
#endif

/* ========================================================================= */
/* 1. COLD METADATA CONSTRUCTOR                                              */
/* ========================================================================= */

sf_live_cold_metadata_t::sf_live_cold_metadata_t() {
    memset(debug_name, 0, sizeof(debug_name));
    type_sig_hash = 0;
    birth_ns = 0;
    call_step_count.store(0, std::memory_order_relaxed);
    pagefile_slot_offset = 0;
    preferred_numa_node = 0;
    for (int i = 0; i < 256; ++i) per_core_cycles[i].store(0, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) visited_cores[i].store(0, std::memory_order_relaxed);
    age_ns.store(0, std::memory_order_relaxed);
    phase_count.store(0, std::memory_order_relaxed);
    bitop_count.store(0, std::memory_order_relaxed);
    avg_cycles_per_call.store(0, std::memory_order_relaxed);
    error_count.store(0, std::memory_order_relaxed);
    is_paged_out.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) per_numa_heat[i].store(0.0f, std::memory_order_relaxed);
}

sf_live_cold_metadata_t::sf_live_cold_metadata_t(const sf_live_cold_metadata_t& o) {
    memcpy(debug_name, o.debug_name, sizeof(debug_name));
    type_sig_hash = o.type_sig_hash;
    birth_ns = o.birth_ns;
    call_step_count.store(o.call_step_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    pagefile_slot_offset = o.pagefile_slot_offset;
    preferred_numa_node = o.preferred_numa_node;
    for (int i = 0; i < 256; ++i) per_core_cycles[i].store(o.per_core_cycles[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) visited_cores[i].store(o.visited_cores[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    age_ns.store(o.age_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    phase_count.store(o.phase_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    bitop_count.store(o.bitop_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    avg_cycles_per_call.store(o.avg_cycles_per_call.load(std::memory_order_relaxed), std::memory_order_relaxed);
    error_count.store(o.error_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    is_paged_out.store(o.is_paged_out.load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) per_numa_heat[i].store(o.per_numa_heat[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
}

sf_live_cold_metadata_t& sf_live_cold_metadata_t::operator=(const sf_live_cold_metadata_t& o) {
    if (this != &o) {
        memcpy(debug_name, o.debug_name, sizeof(debug_name));
        type_sig_hash = o.type_sig_hash;
        birth_ns = o.birth_ns;
        call_step_count.store(o.call_step_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        pagefile_slot_offset = o.pagefile_slot_offset;
        preferred_numa_node = o.preferred_numa_node;
        for (int i = 0; i < 256; ++i) per_core_cycles[i].store(o.per_core_cycles[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) visited_cores[i].store(o.visited_cores[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        age_ns.store(o.age_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
        phase_count.store(o.phase_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        bitop_count.store(o.bitop_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        avg_cycles_per_call.store(o.avg_cycles_per_call.load(std::memory_order_relaxed), std::memory_order_relaxed);
        error_count.store(o.error_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        is_paged_out.store(o.is_paged_out.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) per_numa_heat[i].store(o.per_numa_heat[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
}



/* ========================================================================= */
/* 2. PIN GUARD RAII                                                         */
/* ========================================================================= */

sacculus_pin_guard_t::sacculus_pin_guard_t(SF_custominiVm_live_header_t* o) : obj(o) {
    if (obj) {
        acquired = obj->try_pin();
    }
}

sacculus_pin_guard_t::~sacculus_pin_guard_t() {
    if (obj && acquired) {
        obj->unpin();
    }
}

sacculus_pin_guard_t::sacculus_pin_guard_t(sacculus_pin_guard_t&& other) noexcept
    : obj(other.obj), acquired(other.acquired) {
    other.obj = nullptr;
    other.acquired = false;
}

sacculus_pin_guard_t& sacculus_pin_guard_t::operator=(sacculus_pin_guard_t&& other) noexcept {
    if (this != &other) {
        if (obj && acquired) obj->unpin();
        obj = other.obj;
        acquired = other.acquired;
        other.obj = nullptr;
        other.acquired = false;
    }
    return *this;
}

/* ========================================================================= */
/* 3. LIVE HEADER IMPLEMENTATIONS                                            */
/* ========================================================================= */

SF_custominiVm_live_header_t::SF_custominiVm_live_header_t() = default;

SF_custominiVm_live_header_t::SF_custominiVm_live_header_t(const SF_custominiVm_live_header_t& o) {
    relocate_or_clone_from(const_cast<SF_custominiVm_live_header_t*>(&o), /*transfer_cold_ownership=*/false);
}

SF_custominiVm_live_header_t& SF_custominiVm_live_header_t::operator=(const SF_custominiVm_live_header_t& o) {
    if (this != &o) {
        relocate_or_clone_from(const_cast<SF_custominiVm_live_header_t*>(&o), /*transfer_cold_ownership=*/false);
    }
    return *this;
}

SF_custominiVm_live_header_t::~SF_custominiVm_live_header_t() {
    sf_live_cold_metadata_t* c = cold_meta.exchange(nullptr, std::memory_order_acq_rel);
    if (c) delete c;
}

sf_live_cold_metadata_t* SF_custominiVm_live_header_t::get_cold() {
    sf_live_cold_metadata_t* cold = cold_meta.load(std::memory_order_acquire);
    if (!cold) {
        auto* new_cold = new (std::nothrow) sf_live_cold_metadata_t();
        if (!new_cold) return nullptr;
        if (cold_meta.compare_exchange_strong(cold, new_cold, std::memory_order_acq_rel)) {
            cold = new_cold;
        } else {
            delete new_cold;
        }
    }
    return cold;
}

const sf_live_cold_metadata_t* SF_custominiVm_live_header_t::get_cold_ro() const {
    return cold_meta.load(std::memory_order_acquire);
}

void SF_custominiVm_live_header_t::relocate_or_clone_from(SF_custominiVm_live_header_t* src, bool transfer_cold_ownership) {
    if (!src || src == this) return;

    sf_live_cold_metadata_t* prev_cold = cold_meta.exchange(nullptr, std::memory_order_acq_rel);
    if (prev_cold) delete prev_cold;

    if (transfer_cold_ownership) {
        sf_live_cold_metadata_t* stolen = src->cold_meta.exchange(nullptr, std::memory_order_acq_rel);
        cold_meta.store(stolen, std::memory_order_release);
    } else {
        const auto* src_cold = src->get_cold_ro();
        if (src_cold) {
            auto* cloned = new (std::nothrow) sf_live_cold_metadata_t(*src_cold);
            cold_meta.store(cloned, std::memory_order_release);
        } else {
            cold_meta.store(nullptr, std::memory_order_release);
        }
    }

    sfMN = src->sfMN;
    total_cell_bytes = src->total_cell_bytes;
    function_id = src->function_id;
    total_cycles.store(src->total_cycles.load(std::memory_order_acquire), std::memory_order_release);
    current_core.store(src->current_core.load(std::memory_order_acquire), std::memory_order_release);
    last_access_ns.store(src->last_access_ns.load(std::memory_order_acquire), std::memory_order_release);
    access_count.store(src->access_count.load(std::memory_order_acquire), std::memory_order_release);
    pin_count.store(src->pin_count.load(std::memory_order_acquire), std::memory_order_release);
    state.store(src->state.load(std::memory_order_acquire), std::memory_order_release);
    memory_footprint.store(src->memory_footprint.load(std::memory_order_acquire), std::memory_order_release);
    heat_score.store(src->heat_score.load(std::memory_order_acquire), std::memory_order_release);
    generation = src->generation;
    region = src->region;
    gc_lock.store(src->gc_lock.load(std::memory_order_acquire), std::memory_order_release);
    priority = src->priority;
    instance_count.store(src->instance_count.load(std::memory_order_acquire), std::memory_order_release);
    instance_cap = src->instance_cap;
    gc_sweep_count = src->gc_sweep_count;
    falline_marked = src->falline_marked;
    needs_rehabilitation = src->needs_rehabilitation;
    _pad0[0] = src->_pad0[0];

    has_optimized.store(src->has_optimized.load(std::memory_order_acquire), std::memory_order_release);
    opt_permitted = src->opt_permitted;
    opt_success = src->opt_success;
    group_priority = src->group_priority;
    group_hex_id = src->group_hex_id;
    last_optimized_time.store(src->last_optimized_time.load(std::memory_order_acquire), std::memory_order_release);
    optimization_duration_cycles.store(src->optimization_duration_cycles.load(std::memory_order_acquire), std::memory_order_release);
    cycles_gained.store(src->cycles_gained.load(std::memory_order_acquire), std::memory_order_release);
    active_simd_idx = src->active_simd_idx;
    simd_candidate_count = src->simd_candidate_count;
    memcpy(simd_table, src->simd_table, sizeof(simd_table));

    forwarding_address.store(src->forwarding_address.load(std::memory_order_acquire), std::memory_order_release);
    child_view_count.store(src->child_view_count.load(std::memory_order_acquire), std::memory_order_release);
    parent_header.store(src->parent_header.load(std::memory_order_acquire), std::memory_order_release);
    slice_byte_offset = src->slice_byte_offset;
    memcpy(strides, src->strides, sizeof(strides));
    is_strided_view = src->is_strided_view;
    next = src->next;
    prev = src->prev;
}

void SF_custominiVm_live_header_t::apply_optimized_state_from(const SF_custominiVm_live_header_t& src) {
    active_simd_idx = src.active_simd_idx;
    simd_candidate_count = src.simd_candidate_count;
    memcpy(simd_table, src.simd_table, sizeof(simd_table));
    opt_permitted = src.opt_permitted;
    opt_success = 1;
    group_priority = src.group_priority;
    group_hex_id = src.group_hex_id;
    cycles_gained.store(src.cycles_gained.load(std::memory_order_acquire), std::memory_order_release);
    optimization_duration_cycles.store(src.optimization_duration_cycles.load(std::memory_order_acquire), std::memory_order_release);
    last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
    has_optimized.store(1, std::memory_order_release);
}

void SF_custominiVm_live_header_t::init(uint32_t fn_id, uint64_t sig_hash, const char* name, uint8_t prio) {
    sfMN = SF_LIVE_MAGIC;
    total_cell_bytes = 0;
    function_id = fn_id;
    sf_live_cold_metadata_t* cold = get_cold();
    if (cold) {
        cold->type_sig_hash = sig_hash;
        memset(cold->debug_name, 0, sizeof(cold->debug_name));
        if (name) {
            size_t len = strlen(name);
            if (len > sizeof(cold->debug_name) - 1) len = sizeof(cold->debug_name) - 1;
            memcpy(cold->debug_name, name, len);
        }
        cold->birth_ns = sacculus_now_ns();
        cold->age_ns.store(0, std::memory_order_relaxed);
        for (uint32_t c = 0; c < SACFOR_MAX_CORES; ++c)
            cold->per_core_cycles[c].store(0, std::memory_order_relaxed);
        for (int i = 0; i < SACFOR_CORE_WORDS; ++i)
            cold->visited_cores[i].store(0, std::memory_order_relaxed);
        cold->phase_count.store(0, std::memory_order_relaxed);
        cold->bitop_count.store(0, std::memory_order_relaxed);
        cold->error_count.store(0, std::memory_order_relaxed);
        cold->avg_cycles_per_call.store(0, std::memory_order_relaxed);
        cold->call_step_count.store(0, std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) cold->per_numa_heat[i].store(0.0f, std::memory_order_relaxed);
        cold->preferred_numa_node = 0;
        cold->is_paged_out.store(0, std::memory_order_relaxed);
        cold->pagefile_slot_offset = 0;
        last_access_ns.store(cold->birth_ns, std::memory_order_relaxed);
    } else {
        last_access_ns.store(sacculus_now_ns(), std::memory_order_relaxed);
    }

    total_cycles.store(0, std::memory_order_relaxed);
    current_core.store(0xFFFF, std::memory_order_relaxed);
    access_count.store(0, std::memory_order_relaxed);
    pin_count.store(0, std::memory_order_relaxed);
    state.store(static_cast<uint8_t>(sf_live_state_e::LIVE), std::memory_order_relaxed);
    memory_footprint.store(0, std::memory_order_relaxed);
    heat_score.store(0.0f, std::memory_order_relaxed);
    generation = 0;
    region = 0;
    gc_lock.store(0, std::memory_order_relaxed);
    priority = prio;
    instance_count.store(0, std::memory_order_relaxed);
    instance_cap = SACFOR_MAX_INSTANCES;
    gc_sweep_count = 0;
    falline_marked = 0;
    needs_rehabilitation = 0;
    forwarding_address.store(nullptr, std::memory_order_relaxed);
    child_view_count.store(0, std::memory_order_relaxed);
    parent_header.store(nullptr, std::memory_order_relaxed);
    slice_byte_offset = 0;
    memset(strides, 0, sizeof(strides));
    is_strided_view = 0;

    has_optimized.store(0, std::memory_order_relaxed);
    opt_permitted = 1;
    opt_success = 0;
    group_priority = 5;
    group_hex_id = 0;
    last_optimized_time.store(0, std::memory_order_relaxed);
    optimization_duration_cycles.store(0, std::memory_order_relaxed);
    cycles_gained.store(0, std::memory_order_relaxed);
    active_simd_idx = 0;
    simd_candidate_count = 0;
    memset(simd_table, 0, sizeof(simd_table));
    set_active_simd_candidate("SCALAR", 1);
}

void SF_custominiVm_live_header_t::set_active_simd_candidate(const char* isa_name, uint32_t level) {
    if (!isa_name || !isa_name[0]) return;
    int found_idx = -1;
    for (uint16_t i = 0; i < simd_candidate_count && i < 8; ++i) {
#if defined(_WIN32)
        if (_stricmp(simd_table[i].name, isa_name) == 0) {
#else
        if (strcasecmp(simd_table[i].name, isa_name) == 0) {
#endif
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        if (simd_candidate_count < 8) {
            found_idx = (int)simd_candidate_count++;
        } else {
            found_idx = 7;
        }
        strncpy(simd_table[found_idx].name, isa_name, sizeof(simd_table[found_idx].name) - 1);
        simd_table[found_idx].name[sizeof(simd_table[found_idx].name) - 1] = '\0';
        simd_table[found_idx].ladder_level = level;
    }
    for (uint16_t i = 0; i < simd_candidate_count && i < 8; ++i) {
        simd_table[i].is_active = (i == (uint16_t)found_idx) ? 1 : 0;
    }
    active_simd_idx = (uint16_t)found_idx;
}

void SF_custominiVm_live_header_t::mark_already_on_best_path() {
    has_optimized.store(1, std::memory_order_release);
    opt_success = 1;
    last_optimized_time.store(sacculus_now_ns(), std::memory_order_relaxed);
}

void SF_custominiVm_live_header_t::record_access(uint64_t cycles, uint16_t core_id) {
    if (!isTailCanaryValid()) {
        if (auto* cold = get_cold()) {
            cold->error_count.fetch_add(1, std::memory_order_relaxed);
        }
        state.store(static_cast<uint8_t>(sf_live_state_e::QUARANTINED), std::memory_order_release);
        sf_hotlog_emit(SF_Ebin0_HEAL_CANARY_QUARANTINE, SF_Ebin1_ERROR, function_id,
                       0, generation, region, core_id, 0,
                       (uint32_t)(memory_footprint.load(std::memory_order_relaxed) / 1024),
                       "canary_quarantined");
    }

    uint64_t now = sacculus_now_ns();
    last_access_ns.store(now, std::memory_order_release);
    access_count.fetch_add(1, std::memory_order_relaxed);
    total_cycles.fetch_add(cycles, std::memory_order_relaxed);

    auto* cold = get_cold();
    if (cold) {
        cold->age_ns.store(now - cold->birth_ns, std::memory_order_relaxed);
        cold->call_step_count.fetch_add(1, std::memory_order_relaxed);

        if (core_id < SACFOR_MAX_CORES) {
            cold->per_core_cycles[core_id].fetch_add(cycles, std::memory_order_relaxed);
            current_core.store(core_id, std::memory_order_release);
            sacculus_core_bitmap_set(cold->visited_cores, core_id);

            uint8_t numa = (uint8_t)(core_id / 64);
            if (numa < 4) {
                float heat_gain = g_sf_heat_gain_weight.load(std::memory_order_relaxed);
                if (heat_gain <= 0.0f) heat_gain = SACFOR_HEAT_GAIN;
                float h = cold->per_numa_heat[numa].load(std::memory_order_relaxed);
                while (!cold->per_numa_heat[numa].compare_exchange_weak(
                    h, h + heat_gain, std::memory_order_relaxed, std::memory_order_relaxed)) {}
                cold->preferred_numa_node = numa;
            }
        }
    }

    float heat_gain = g_sf_heat_gain_weight.load(std::memory_order_relaxed);
    if (heat_gain <= 0.0f) heat_gain = SACFOR_HEAT_GAIN;
    float old_h = heat_score.load(std::memory_order_relaxed);
    while (!heat_score.compare_exchange_weak(
        old_h, old_h + heat_gain, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void SF_custominiVm_live_header_t::decay_heat() {
    float heat_decay = g_sf_heat_decay_factor.load(std::memory_order_relaxed);
    if (heat_decay <= 0.0f) heat_decay = SACFOR_HEAT_DECAY;
    float h = heat_score.load(std::memory_order_relaxed);
    heat_score.store(h * heat_decay, std::memory_order_relaxed);
    ++gc_sweep_count;
}

bool SF_custominiVm_live_header_t::try_pin() {
    while (true) {
        uint8_t st = state.load(std::memory_order_acquire);
        if (st != static_cast<uint8_t>(sf_live_state_e::LIVE)) return false;

        int32_t pc = pin_count.load(std::memory_order_relaxed);
        if (pin_count.compare_exchange_weak(pc, pc + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            if (state.load(std::memory_order_acquire) == static_cast<uint8_t>(sf_live_state_e::LIVE)) {
                return true;
            }
            pin_count.fetch_sub(1, std::memory_order_release);
            return false;
        }
    }
}

void SF_custominiVm_live_header_t::unpin() {
    int32_t cur = pin_count.load(std::memory_order_relaxed);
    while (cur > 0 && !pin_count.compare_exchange_weak(cur, cur - 1, std::memory_order_release, std::memory_order_relaxed)) {}
}

bool SF_custominiVm_live_header_t::try_transition_to_falline() {
    if (is_pinned()) return false;
    if (child_view_count.load(std::memory_order_acquire) > 0) return false;

    uint8_t expected = static_cast<uint8_t>(sf_live_state_e::LIVE);
    if (state.compare_exchange_strong(expected, static_cast<uint8_t>(sf_live_state_e::TRANSIT_FALLINE),
                                      std::memory_order_acq_rel, std::memory_order_relaxed)) {
        if (is_pinned() || child_view_count.load(std::memory_order_acquire) > 0) {
            state.store(static_cast<uint8_t>(sf_live_state_e::LIVE), std::memory_order_release);
            return false;
        }
        falline_marked = 1;
        return true;
    }
    return false;
}

bool SF_custominiVm_live_header_t::try_transition_to_rehab() {
    if (is_pinned()) return false;
    if (child_view_count.load(std::memory_order_acquire) > 0) return false;

    uint8_t expected = static_cast<uint8_t>(sf_live_state_e::LIVE);
    if (state.compare_exchange_strong(expected, static_cast<uint8_t>(sf_live_state_e::TRANSIT_REHAB),
                                      std::memory_order_acq_rel, std::memory_order_relaxed)) {
        if (is_pinned() || child_view_count.load(std::memory_order_acquire) > 0) {
            state.store(static_cast<uint8_t>(sf_live_state_e::LIVE), std::memory_order_release);
            return false;
        }
        return true;
    }
    return false;
}

bool SF_custominiVm_live_header_t::is_idle() const {
    uint64_t now = sacculus_now_ns();
    uint64_t last = last_access_ns.load(std::memory_order_acquire);
    uint64_t idle = now - last;
    switch (generation) {
    case 0: return idle > SACFOR_IDLE_YOUNG_NS;
    case 1: return idle > SACFOR_IDLE_ADOL_NS;
    case 2: return idle > SACFOR_IDLE_SENIOR_NS;
    default: return false;
    }
}

uint8_t SF_custominiVm_live_header_t::compute_target_region() const {
    float h = heat_score.load(std::memory_order_relaxed);
    if (h > SACFOR_HEAT_HOT) return SF_REGION_HOTFIELD;
    size_t fp = (size_t)memory_footprint.load(std::memory_order_relaxed);
    const auto* cold = get_cold_ro();
    uint64_t avg = cold ? cold->avg_cycles_per_call.load(std::memory_order_relaxed) : 0;

    if (fp >= 64 * 1024 || avg > SACFOR_OVERHEAD_MIDDLE) return SF_REGION_TANK;
    if (avg > SACFOR_OVERHEAD_DEMAND) return SF_REGION_MIDDLE;
    return SF_REGION_DEMAND;
}

inline void* SF_custominiVm_live_header_t::tailCanaryPtr() const noexcept {
    if (parent_header.load(std::memory_order_relaxed) != nullptr) {
        static uint64_t dummy_canary = SF_TAIL_CANARY;
        return &dummy_canary;
    }
    size_t fp = (size_t)memory_footprint.load(std::memory_order_relaxed);
    size_t aligned_fp = (fp + 7) & ~7ULL;
    size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    if (total_cell_bytes > 0 && header_sz + aligned_fp + 8 > total_cell_bytes) {
        static uint64_t dummy_canary = SF_TAIL_CANARY;
        return &dummy_canary;
    }
    return const_cast<uint8_t*>(body() + aligned_fp);
}

void SF_custominiVm_live_header_t::writeTailCanary() noexcept {
    if (parent_header.load(std::memory_order_relaxed) != nullptr) return;
    size_t fp = (size_t)memory_footprint.load(std::memory_order_relaxed);
    size_t aligned_fp = (fp + 7) & ~7ULL;
    size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    if (total_cell_bytes > 0 && header_sz + aligned_fp + 8 > total_cell_bytes) return;
    volatile uint64_t* canary_slot = reinterpret_cast<volatile uint64_t*>(tailCanaryPtr());
    *canary_slot = SF_TAIL_CANARY;
    SF_COMPILER_BARRIER(canary_slot);
}

bool SF_custominiVm_live_header_t::isTailCanaryValid() const noexcept {
    if (parent_header.load(std::memory_order_relaxed) != nullptr) return true;
    size_t fp = (size_t)memory_footprint.load(std::memory_order_relaxed);
    if (fp == 0) return true;
    size_t aligned_fp = (fp + 7) & ~7ULL;
    size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    if (total_cell_bytes > 0 && header_sz + aligned_fp + 8 > total_cell_bytes) return false;

    sf_critical_simd_e m = sf_get_critical_simd_mode();
    if (m == sf_critical_simd_e::DENY) {
        return *reinterpret_cast<const volatile uint64_t*>(body() + aligned_fp) == SF_TAIL_CANARY;
    }
#if defined(_M_X64) || defined(__x86_64__)
    __m128i val = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(body() + aligned_fp));
    __m128i exp = _mm_set_epi64x(0, SF_TAIL_CANARY);
    __m128i cmp = _mm_cmpeq_epi64(val, exp);
    return (_mm_cvtsi128_si64(cmp) != 0);
#else
    return *reinterpret_cast<const volatile uint64_t*>(body() + aligned_fp) == SF_TAIL_CANARY;
#endif
}

void* SF_custominiVm_live_header_t::resolve_active_body() const {
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
    return nullptr;
}

/* ========================================================================= */
/* 4. ORDER OF HEIGHT (sacculus_order_of_height_t)                           */
/* ========================================================================= */

void sacculus_order_of_height_t::insert_nolock(sacculus_live_header_t* obj) {
    if (!obj) return;
    obj->next = obj->prev = nullptr;
    uint32_t obj_steps = sf_get_call_step_count(obj);
    size_t tier = compute_tier_bucket(obj_steps);

    if (!head) {
        head = tail = obj;
        tier_fingers[tier] = obj;
        ++count;
        return;
    }

    if (tail && sf_get_call_step_count(tail) >= obj_steps) {
        tail->next = obj;
        obj->prev = tail;
        tail = obj;
        if (!tier_fingers[tier]) tier_fingers[tier] = obj;
        ++count;
        return;
    }

    if (sf_get_call_step_count(head) <= obj_steps) {
        obj->next = head;
        head->prev = obj;
        head = obj;
        tier_fingers[tier] = obj;
        ++count;
        return;
    }

    sacculus_live_header_t* cur = head;
    for (size_t t = tier; t < NUM_TIER_FINGERS; ++t) {
        if (tier_fingers[t] && sf_get_call_step_count(tier_fingers[t]) > obj_steps) {
            cur = tier_fingers[t];
            break;
        }
    }
    while (cur && sf_get_call_step_count(cur) > obj_steps) {
        cur = cur->next;
    }

    if (!cur) {
        tail->next = obj;
        obj->prev = tail;
        tail = obj;
    } else {
        sacculus_live_header_t* p = cur->prev;
        if (p) p->next = obj;
        else head = obj;
        obj->prev = p;
        obj->next = cur;
        cur->prev = obj;
    }
    if (!tier_fingers[tier]) tier_fingers[tier] = obj;
    ++count;
}

void sacculus_order_of_height_t::insert(sacculus_live_header_t* obj) {
    sf_ticket_guard_t g(lock);
    insert_nolock(obj);
}

void sacculus_order_of_height_t::remove_nolock(sacculus_live_header_t* obj) {
    if (!obj) return;
    for (size_t t = 0; t < NUM_TIER_FINGERS; ++t) {
        if (tier_fingers[t] == obj) {
            tier_fingers[t] = obj->next ? obj->next : obj->prev;
        }
    }
    if (obj->prev) obj->prev->next = obj->next;
    else           head = obj->next;
    if (obj->next) obj->next->prev = obj->prev;
    else           tail = obj->prev;
    obj->next = obj->prev = nullptr;
    if (count > 0) --count;
    if (count == 0) {
        head = tail = nullptr;
        memset(tier_fingers, 0, sizeof(tier_fingers));
    }
}

void sacculus_order_of_height_t::remove(sacculus_live_header_t* obj) {
    sf_ticket_guard_t g(lock);
    remove_nolock(obj);
}

void sacculus_order_of_height_t::bubble_up_nolock(sacculus_live_header_t* obj) {
    if (!obj || obj->region != SF_REGION_HOTFIELD) return;
    uint32_t my_steps = sf_get_call_step_count(obj);
    while (obj->prev && my_steps > sf_get_call_step_count(obj->prev)) {
        sacculus_live_header_t* p = obj->prev;
        sacculus_live_header_t* pp = p->prev;
        sacculus_live_header_t* n = obj->next;

        if (pp) pp->next = obj;
        else head = obj;
        obj->prev = pp;

        obj->next = p;
        p->prev = obj;

        p->next = n;
        if (n) n->prev = p;
        else tail = p;
    }
    size_t tier = compute_tier_bucket(my_steps);
    tier_fingers[tier] = obj;
}

void sacculus_order_of_height_t::bubble_up(sacculus_live_header_t* obj) {
    sf_ticket_guard_t g(lock);
    bubble_up_nolock(obj);
}

/* ========================================================================= */
/* 5. FIBONACCI INDEXED BUCKET (sf_fibonacci_indexed_bucket_t)               */
/* ========================================================================= */

sf_fibonacci_indexed_bucket_t::sf_fibonacci_indexed_bucket_t() {
    memset(slots, 0, sizeof(slots));
}

void sf_fibonacci_indexed_bucket_t::insert_nolock(sacculus_live_header_t* obj) {
    if (!obj) return;
    size_t idx = hash_index(obj->function_id);
    size_t attempts = 0;
    while (slots[idx] != nullptr && attempts < BUCKET_CAP) {
        idx = (idx + 1) & BUCKET_MASK;
        ++attempts;
    }
    if (attempts < BUCKET_CAP) {
        slots[idx] = obj;
    }

    obj->prev = nullptr;
    obj->next = head;
    if (head) head->prev = obj;
    head = obj;
    ++count;
}

void sf_fibonacci_indexed_bucket_t::insert(sacculus_live_header_t* obj) {
    sf_ticket_guard_t g(lock);
    insert_nolock(obj);
}

void sf_fibonacci_indexed_bucket_t::remove_nolock(sacculus_live_header_t* obj) {
    if (!obj) return;
    size_t idx = hash_index(obj->function_id);
    size_t attempts = 0;
    size_t found_idx = BUCKET_CAP;
    while (attempts < BUCKET_CAP) {
        if (slots[idx] == obj) {
            found_idx = idx;
            break;
        }
        if (slots[idx] == nullptr) break;
        idx = (idx + 1) & BUCKET_MASK;
        ++attempts;
    }

    if (found_idx < BUCKET_CAP) {
        size_t empty_slot = found_idx;
        size_t curr = (empty_slot + 1) & BUCKET_MASK;
        size_t probes = 0;
        while (slots[curr] != nullptr && curr != found_idx && probes++ < BUCKET_CAP) {
            size_t natural_slot = hash_index(slots[curr]->function_id);
            bool can_shift = ((empty_slot - natural_slot) & BUCKET_MASK) < ((curr - natural_slot) & BUCKET_MASK);
            if (can_shift) {
                slots[empty_slot] = slots[curr];
                empty_slot = curr;
            }
            curr = (curr + 1) & BUCKET_MASK;
        }
        slots[empty_slot] = nullptr;
    }

    if (obj->prev) obj->prev->next = obj->next;
    else           head = obj->next;
    if (obj->next) obj->next->prev = obj->prev;
    obj->next = obj->prev = nullptr;
    if (count > 0) --count;
}

void sf_fibonacci_indexed_bucket_t::remove(sacculus_live_header_t* obj) {
    sf_ticket_guard_t g(lock);
    remove_nolock(obj);
}

sacculus_live_header_t* sf_fibonacci_indexed_bucket_t::lookup_direct(uint32_t fn_id) {
    sf_ticket_guard_t g(lock);
    size_t idx = hash_index(fn_id);
    size_t attempts = 0;
    while (attempts < 16) {
        sacculus_live_header_t* o = slots[idx];
        if (!o) break;
        if (o->function_id == fn_id) return o;
        idx = (idx + 1) & BUCKET_MASK;
        ++attempts;
    }

    sacculus_live_header_t* cur = head;
    while (cur) {
        if (cur->function_id == fn_id) return cur;
        cur = cur->next;
    }
    return nullptr;
}

/* ========================================================================= */
/* 6. REGION & ROOM OPERATIONS                                               */
/* ========================================================================= */

void sf_region_t::place_nolock(sfLiveHeader* obj) {
    if (!obj) return;
    obj->region = static_cast<uint8_t>(id);
    if (id == SF_REGION_HOTFIELD) {
        hot_order.insert_nolock(obj);
    } else {
        bucket.insert_nolock(obj);
    }
    count.fetch_add(1, std::memory_order_relaxed);
    total_bytes.fetch_add(obj->memory_footprint.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void sf_region_t::place(sfLiveHeader* obj) {
    if (!obj) return;
    sf_ticket_guard_t g(get_lock());
    place_nolock(obj);
}

void sf_region_t::evict_nolock(sfLiveHeader* obj) {
    if (!obj) return;
    if (id == SF_REGION_HOTFIELD) {
        hot_order.remove_nolock(obj);
    } else {
        bucket.remove_nolock(obj);
    }
    count.fetch_sub(1, std::memory_order_relaxed);
    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    size_t cur = total_bytes.load(std::memory_order_relaxed);
    while (cur > 0) {
        size_t next_b = (cur >= fp) ? (cur - fp) : 0;
        if (total_bytes.compare_exchange_weak(cur, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }
}

void sf_region_t::evict(sfLiveHeader* obj) {
    if (!obj) return;
    sf_ticket_guard_t g(get_lock());
    evict_nolock(obj);
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
    size_t co = total_objects.load(std::memory_order_relaxed);
    while (co > 0 && !total_objects.compare_exchange_weak(co, co - 1, std::memory_order_relaxed)) {}

    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    size_t cur = total_bytes.load(std::memory_order_relaxed);
    while (cur > 0) {
        size_t next_b = (cur >= fp) ? (cur - fp) : 0;
        if (total_bytes.compare_exchange_weak(cur, next_b, std::memory_order_relaxed)) break;
    }
}

/* ========================================================================= */
/* 7. REDLET MOTHERLINE IMPLEMENTATION                                       */
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

void RedletMotherline::destroy() {
    for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
        for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
            auto& region = _rooms[r].regions[reg];
            sf_ticket_guard_t g(region.get_lock());
            auto* cur = region.get_head();
            while (cur) {
                auto* nxt = cur->next;
                cur->~SF_custominiVm_live_header_t();
                cur = nxt;
            }
            if (region.id == SF_REGION_HOTFIELD) {
                region.hot_order.head = nullptr;
                region.hot_order.tail = nullptr;
                memset(region.hot_order.tier_fingers, 0, sizeof(region.hot_order.tier_fingers));
                region.hot_order.count = 0;
            } else {
                region.bucket.head = nullptr;
                region.bucket.count = 0;
                memset(region.bucket.slots, 0, sizeof(region.bucket.slots));
            }
            region.count.store(0, std::memory_order_relaxed);
            region.total_bytes.store(0, std::memory_order_relaxed);
        }
    }
    _egg_count.store(0, std::memory_order_relaxed);
    _fledgling_count.store(0, std::memory_order_relaxed);
}

size_t RedletMotherline::batch_optimize_egg_objects(uint32_t fn_id, const char* new_isa_name,
                                                    uint16_t new_simd_idx, uint64_t cycles_saved) {
    size_t updated = 0;
    for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
        for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
            auto& region = _rooms[r].regions[reg];
            sf_ticket_guard_t g(region.get_lock());
            auto* cur = region.get_head();
            while (cur) {
                if (cur->function_id == fn_id && cur->has_optimized.load(std::memory_order_relaxed) == 0) {
                    if (cur->opt_permitted == 0) {
                        cur = cur->next;
                        continue;
                    }
                    if (new_isa_name && new_isa_name[0]) {
                        cur->set_active_simd_candidate(new_isa_name, 0);
                    } else {
                        cur->active_simd_idx = new_simd_idx;
                        for (uint16_t i = 0; i < cur->simd_candidate_count && i < 8; ++i) {
                            cur->simd_table[i].is_active = (i == new_simd_idx) ? 1 : 0;
                        }
                    }
                    cur->has_optimized.store(1, std::memory_order_release);
                    cur->opt_success = 1;
                    cur->cycles_gained.store(cycles_saved, std::memory_order_relaxed);
                    cur->last_optimized_time.store(sacculus_now_ns(), std::memory_order_relaxed);
                    cur->optimization_duration_cycles.store(250, std::memory_order_relaxed);

                    size_t ec = _egg_count.load(std::memory_order_relaxed);
                    while (ec > 0 && !_egg_count.compare_exchange_weak(ec, ec - 1, std::memory_order_relaxed)) {}
                    _fledgling_count.fetch_add(1, std::memory_order_relaxed);

                    sf_hotlog_emit(SF_Ebin0_MOTHERLINE_FLEDGLING, SF_Ebin1_INFO, fn_id,
                                   cur->active_simd_idx, cur->generation, cur->region,
                                   cur->current_core.load(std::memory_order_relaxed), 0,
                                   (uint32_t)cycles_saved, cur->simd_table[cur->active_simd_idx].name);
                    ++updated;
                }
                cur = cur->next;
            }
        }
    }
    return updated;
}

/* ========================================================================= */
/* 8. LANDFILL HEADER IMPLEMENTATION                                         */
/* ========================================================================= */

void SF_custominiVm_landfill_header_t::populate_from_live(const SF_custominiVm_live_header_t* live,
                                                          sf_death_reason_e reason,
                                                          uint64_t evac_duration_ns,
                                                          size_t compacted_sz) {
    if (!live) return;
    function_id = live->function_id;
    const auto* cold = live->get_cold_ro();
    type_sig_hash = cold ? cold->type_sig_hash : 0;
    birth_ns = cold ? cold->birth_ns : 0;
    death_ns = sacculus_now_ns();
    lifetime_ns = death_ns - birth_ns;
    freight_timestamp_ns = death_ns;
    evacuation_duration_ns = evac_duration_ns;

    total_cpu_cycles = live->total_cycles.load(std::memory_order_relaxed);
    for (uint32_t c = 0; c < SACFOR_MAX_CORES; ++c) {
        per_core_cycles[c] = cold ? cold->per_core_cycles[c].load(std::memory_order_relaxed) : 0;
    }

    previous_size = (size_t)live->memory_footprint.load(std::memory_order_relaxed);
    pit_stored_size = sizeof(SF_custominiVm_live_header_t);
    compacted_size = compacted_sz ? compacted_sz : pit_stored_size;

    lasting = (reason == SF_DEATH_REHABILITATE || live->needs_rehabilitation) ? 1 : 0;
    header_pane_byte_ref = 0;
    peak_generation = live->generation;
    peak_region = live->region;
    death_reason = reason;
    error_count = (uint8_t)(cold ? cold->error_count.load(std::memory_order_relaxed) : 0);
    last_core = live->current_core.load(std::memory_order_relaxed);
    unique_cores_visited = (uint16_t)(cold ? sacculus_core_bitmap_count(cold->visited_cores) : 0);
    header_pane_ref = nullptr;
    crc32 = 0;
    rehab_offset = 0;
    rehab_payload_ptr = nullptr;

    if (live->isValid()) {
        size_t fp = (size_t)live->memory_footprint.load(std::memory_order_relaxed);
        if (fp > 0 && live->body()) {
            crc32 = sf_compute_crc32(live->body(), fp);
        }
    }
}

/* ========================================================================= */
/* 9. ARENA STORAGE (sf_arena_storage_t)                                     */
/* ========================================================================= */

bool sf_arena_storage_t::init(size_t initial_bytes, bool pretouch) {
    Advance_Payment = pretouch;
    uint8_t* b = (uint8_t*)sf_os_reserve_memory(initial_bytes);
    if (!b) return false;
    if (!sf_os_commit_memory(b, initial_bytes)) {
        sf_os_release_memory(b, initial_bytes);
        return false;
    }
    if (Advance_Payment) sf_pretouch_memory(b, initial_bytes);
    base.store(b, std::memory_order_release);
    capacity.store(initial_bytes, std::memory_order_release);
    return true;
}

bool sf_arena_storage_t::grow_nolock(size_t min_needed) {
    size_t old_cap = capacity.load(std::memory_order_relaxed);
    uint8_t* old_b = base.load(std::memory_order_relaxed);
    size_t new_cap = old_cap ? (old_cap * SACFOR_LANDFILL_SCALE) : SACFOR_LANDFILL_INIT;
    while (new_cap < min_needed) {
        new_cap *= SACFOR_LANDFILL_SCALE;
    }
    uint8_t* new_b = (uint8_t*)sf_os_reserve_memory(new_cap);
    if (!new_b) return false;
    if (!sf_os_commit_memory(new_b, new_cap)) {
        sf_os_release_memory(new_b, new_cap);
        return false;
    }
    size_t snap_used = used.load(std::memory_order_relaxed);
    if (old_b && snap_used > 0) {
        memcpy(new_b, old_b, snap_used);
    }
    if (Advance_Payment && new_cap > old_cap)
        sf_pretouch_memory(new_b + old_cap, new_cap - old_cap);

    base.store(new_b, std::memory_order_release);
    capacity.store(new_cap, std::memory_order_release);
    if (old_b) {
        old_bases.push_back({ old_b, old_cap });
    }
    return true;
}

void* sf_arena_storage_t::store_with_offset(const void* body, size_t size, size_t* out_offset) {
    if (!body || size == 0) return nullptr;
    sf_ticket_guard_t g(grow_lock);
    size_t off = used.load(std::memory_order_relaxed);
    size_t cap = capacity.load(std::memory_order_relaxed);
    if (off + size > cap) {
        if (!grow_nolock(off + size)) {
            return nullptr;
        }
    }
    uint8_t* b = base.load(std::memory_order_relaxed);
    if (!b) return nullptr;
    memcpy(b + off, body, size);
    used.store(off + size, std::memory_order_release);
    if (out_offset) *out_offset = off;
    return b + off;
}

void* sf_arena_storage_t::store(const void* body, size_t size) {
    return store_with_offset(body, size, nullptr);
}

float sf_arena_storage_t::fill_ratio() const noexcept {
    size_t cap = capacity.load(std::memory_order_relaxed);
    if (cap == 0) return 1.0f;
    return (float)used.load(std::memory_order_relaxed) / (float)cap;
}

void sf_arena_storage_t::flush() {
    sf_ticket_guard_t g(grow_lock);
    for (auto& old : old_bases) {
        sf_os_release_memory(old.first, old.second);
    }
    old_bases.clear();
    uint8_t* b = base.load(std::memory_order_acquire);
    size_t cap = capacity.load(std::memory_order_relaxed);
    if (b && cap > 0) {
        sf_os_decommit_memory(b, cap);
        sf_os_commit_memory(b, cap);
    }
    used.store(0, std::memory_order_release);
}

bool sf_arena_storage_t::grow() {
    sf_ticket_guard_t g(grow_lock);
    return grow_nolock(capacity.load(std::memory_order_relaxed) * SACFOR_LANDFILL_SCALE);
}

void sf_arena_storage_t::destroy() {
    for (auto& old : old_bases) {
        sf_os_release_memory(old.first, old.second);
    }
    old_bases.clear();
    uint8_t* b = base.load(std::memory_order_acquire);
    if (!b) return;
    sf_os_release_memory(b, capacity.load(std::memory_order_relaxed));
    base.store(nullptr, std::memory_order_release);
    capacity.store(0, std::memory_order_release);
    used.store(0, std::memory_order_release);
}

/* ========================================================================= */
/* 10. TRASH PIT (sf_trash_pit_t)                                            */
/* ========================================================================= */

bool sf_trash_pit_t::init(size_t initial_bytes, bool Advance_Payment) {
    size_t half = initial_bytes / 2;
    if (half < 1024 * 1024) half = 1024 * 1024;
    return matured_arena.init(half, Advance_Payment) && rehab_arena.init(half, Advance_Payment);
}

void* sf_trash_pit_t::store_matured(const void* body, size_t size) {
    return matured_arena.store(body, size);
}

void* sf_trash_pit_t::store_rehab(const void* body, size_t size) {
    return rehab_arena.store(body, size);
}

void* sf_trash_pit_t::store_rehab_with_offset(const void* body, size_t size, size_t* out_offset) {
    return rehab_arena.store_with_offset(body, size, out_offset);
}

float sf_trash_pit_t::max_fill_ratio() const noexcept {
    float f1 = matured_arena.fill_ratio();
    float f2 = rehab_arena.fill_ratio();
    return (f1 > f2) ? f1 : f2;
}

size_t sf_trash_pit_t::total_used() const noexcept {
    return matured_arena.used.load(std::memory_order_relaxed) +
           rehab_arena.used.load(std::memory_order_relaxed);
}

void sf_trash_pit_t::flush() {
    matured_arena.flush();
    rehab_arena.flush();
}

void sf_trash_pit_t::destroy() {
    matured_arena.destroy();
    rehab_arena.destroy();
}

/* ========================================================================= */
/* 11. HEADER PANE (sacculus_header_pane_t)                                  */
/* ========================================================================= */

bool sacculus_header_pane_t::init() {
    if (entries_ptr) return true;
    entries_ptr = std::make_unique<SF_custominiVm_landfill_header_t[]>(PANE_CAP);
    entries = entries_ptr.get();
    return entries_ptr != nullptr;
}

int32_t sacculus_header_pane_t::push(const SF_custominiVm_landfill_header_t& h) {
    if (!entries_ptr) return -1;
    sf_ticket_guard_t g(lock);
    uint32_t idx = count.load(std::memory_order_relaxed);
    if (idx >= PANE_CAP) return -1;
    entries_ptr[idx] = h;
    entries_ptr[idx].header_pane_byte_ref = (uint8_t)(idx & 0xFF);
    entries_ptr[idx].header_pane_ref = &entries_ptr[idx];
    count.fetch_add(1, std::memory_order_release);
    return (int32_t)idx;
}

void sacculus_header_pane_t::flush() {
    sf_ticket_guard_t g(lock);
    count.store(0, std::memory_order_release);
}

/* ========================================================================= */
/* 12. REDLET LANDFILL IMPLEMENTATION                                        */
/* ========================================================================= */

RedletLandfill::RedletLandfill() {
    init();
}

RedletLandfill::~RedletLandfill() {
    destroy();
}

bool RedletLandfill::init(size_t initial, bool advance_payment) {
    return _pit.init(initial, advance_payment) && _pane.init();
}

bool RedletLandfill::accept(SF_custominiVm_live_header_t* live,
                            sf_death_reason_e reason,
                            sacculus_hotlog_t* hotlog,
                            uint64_t sweep_no,
                            uint64_t evac_duration_ns) {
    if (!live) return false;
    SF_custominiVm_landfill_header_t lh{};
    lh.populate_from_live(live, reason, evac_duration_ns);

    void* pit_addr = nullptr;
    size_t pit_offset = 0;
    bool is_rehab = (reason == SF_DEATH_REHABILITATE || live->needs_rehabilitation);
    if (is_rehab) {
        size_t fp = (size_t)live->memory_footprint.load(std::memory_order_relaxed);
        size_t cell_sz = live->total_cell_bytes > 0 ? live->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
        pit_addr = _pit.store_rehab_with_offset(live, cell_sz, &pit_offset);
        if (pit_addr) {
            reinterpret_cast<SF_custominiVm_live_header_t*>(pit_addr)->cold_meta.store(nullptr, std::memory_order_relaxed);
        }
        lh.pit_stored_size = cell_sz;
        lh.rehab_offset = pit_offset;
        lh.rehab_payload_ptr = pit_addr;
    } else {
        pit_addr = _pit.store_matured(live, sizeof(SF_custominiVm_live_header_t));
        if (pit_addr) {
            reinterpret_cast<SF_custominiVm_live_header_t*>(pit_addr)->cold_meta.store(nullptr, std::memory_order_relaxed);
        }
        lh.pit_stored_size = sizeof(SF_custominiVm_live_header_t);
    }

    _pane.init();
    int32_t pane_idx = _pane.push(lh);

    if (hotlog) {
        sacculus_hotlog_write(hotlog, SF_Ebin0_TO_LANDFILL,
                              SF_Ebin1_INFO, live->function_id,
                              (uint16_t)(pane_idx >= 0 ? (pane_idx & 0xFFFF) : 0),
                              live->generation, live->region,
                              live->current_core.load(std::memory_order_relaxed), sweep_no,
                              (uint32_t)(_pit.total_used() / 1024),
                              is_rehab ? "rehab_accept" : "matured_accept");
    }
    return pit_addr != nullptr;
}

bool RedletLandfill::is_critical(float critical_ratio) const {
    float crit = (critical_ratio > 0.0f) ? critical_ratio : SACFOR_LANDFILL_CRIT;
    return _pit.max_fill_ratio() > crit ||
           _pane.count.load(std::memory_order_relaxed) > sacculus_header_pane_t::PANE_CAP * 9 / 10;
}

void RedletLandfill::evacuate(sacculus_hotlog_t* hotlog, uint64_t sweep_no) {
    if (hotlog) {
        sacculus_hotlog_write(hotlog, SF_Ebin0_LANDFILL_EVAC,
                              SF_Ebin1_WARN, 0, 0, 0, 0, 0, sweep_no,
                              (uint32_t)(_pit.total_used() / 1024), "sync_evac");
    }
    _pit.flush();
    _pane.flush();
}

void RedletLandfill::destroy() {
    _pit.destroy();
    _pane.flush();
}
