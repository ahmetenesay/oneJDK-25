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

#include "gc/redlet/redletPitstop.hpp"
#include "gc/redlet/redletPointCoordinator.hpp"
#include "gc/redlet/redletThreadLocalData.hpp"
#include "gc/redlet/redletFlags.hpp"
#include <new>

namespace {

static inline void release_entry_pause_if_held(sacculus_pitstop_entry_t* entry) noexcept {
    if (!entry) return;
    bool expected = true;
    if (entry->pause_engaged.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        if (entry->slot_addr != nullptr) {
            RedletPointCoordinator::release_object_pause(entry->slot_addr);
        }
    }
}

} // namespace

sf_puzzler_morph_e RedletPitstop::resolve_morph_mode() noexcept {
    const RedletConfig& cfg = RedletFlags::config();
    if (cfg.strings.is_safety_critical || cfg.strings.profile == RedletProfile::SSIOLSC ||
        cfg.strings.no_sfgc_overwrite_mode) {
        return sf_puzzler_morph_e::EXCISION_EXCLUSIVE;
    }
    uint8_t mode = g_sf_puzzler_morph_method.load(std::memory_order_relaxed);
    if (mode == static_cast<uint8_t>(sf_puzzler_morph_e::EXCISION_EXCLUSIVE)) {
        return sf_puzzler_morph_e::EXCISION_EXCLUSIVE;
    }
    return sf_puzzler_morph_e::SHADOW_RCU;
}

RedletPitstop::RedletPitstop() = default;

RedletPitstop::~RedletPitstop() {
    destroy();
}

void RedletPitstop::destroy() {
    sf_ticket_guard_t g(_lock);
    auto* cur = _head;
    while (cur) {
        auto* next = cur->next;
        release_entry_pause_if_held(cur);
        cur->is_busy.store(false, std::memory_order_release);
        cur->wait_cv.notify_all();
        cur->release();
        cur = next;
    }
    _head = nullptr;
    _count = 0;
}

sacculus_pitstop_entry_t* RedletPitstop::admit_excision(sfLiveHeader* obj) {
    if (!obj) return nullptr;

    // Engage 4-Points per-object Pausepoint so any mutator polling Pausepoint on obj yields cleanly
    RedletPointCoordinator::request_object_pause(static_cast<void*>(obj));

    sf_ticket_guard_t g(_lock);
    auto* entry = new (std::nothrow) sacculus_pitstop_entry_t();
    if (!entry) {
        RedletPointCoordinator::release_object_pause(static_cast<void*>(obj));
        return nullptr;
    }

    entry->saved_header.relocate_or_clone_from(obj, /*transfer_cold_ownership=*/true);
    entry->slot_addr = static_cast<void*>(obj);
    entry->slot_bytes = entry->saved_header.total_cell_bytes;
    entry->mode = sf_puzzler_morph_e::EXCISION_EXCLUSIVE;
    entry->pause_engaged.store(true, std::memory_order_release);
    entry->is_busy.store(true, std::memory_order_release);

    obj->~SF_custominiVm_live_header_t();
    auto* eb = new (entry->slot_addr) SF_custominiVm_emptybox_header_t();
    eb->init(entry->saved_header.function_id, entry->slot_bytes, entry->slot_bytes, static_cast<void*>(entry), 0);

    entry->next = _head;
    entry->prev = nullptr;
    if (_head) _head->prev = entry;
    _head = entry;
    ++_count;
    _total_morphed.fetch_add(1, std::memory_order_relaxed);

    sf_hotlog_emit(SF_Ebin0_PITSTOP_EXCISE, SF_Ebin1_INFO, entry->saved_header.function_id, 0,
                   entry->saved_header.generation, entry->saved_header.region, 0, 0, 0, "pitstop_excision_exclusive");
    return entry;
}

sacculus_pitstop_entry_t* RedletPitstop::admit_shadow(sfLiveHeader* obj) {
    if (!obj) return nullptr;

    // Engage 4-Points per-object Pausepoint during critical shadow snapshot & RCU transition
    RedletPointCoordinator::request_object_pause(static_cast<void*>(obj));

    sf_ticket_guard_t g(_lock);
    auto* entry = new (std::nothrow) sacculus_pitstop_entry_t();
    if (!entry) {
        RedletPointCoordinator::release_object_pause(static_cast<void*>(obj));
        return nullptr;
    }

    entry->saved_header.relocate_or_clone_from(obj, /*transfer_cold_ownership=*/false);
    entry->slot_addr = static_cast<void*>(obj);
    entry->slot_bytes = obj->total_cell_bytes;
    entry->mode = sf_puzzler_morph_e::SHADOW_RCU;
    entry->pause_engaged.store(true, std::memory_order_release);
    entry->is_busy.store(false, std::memory_order_relaxed);

    entry->next = _head;
    entry->prev = nullptr;
    if (_head) _head->prev = entry;
    _head = entry;
    ++_count;
    _total_morphed.fetch_add(1, std::memory_order_relaxed);

    sf_hotlog_emit(SF_Ebin0_PITSTOP_SHADOW, SF_Ebin1_INFO, obj->function_id, 0,
                   obj->generation, obj->region, 0, 0, 0, "pitstop_shadow_rcu");
    return entry;
}

sacculus_pitstop_entry_t* RedletPitstop::admit(sfLiveHeader* obj) {
    if (!obj) return nullptr;
    if (resolve_morph_mode() == sf_puzzler_morph_e::EXCISION_EXCLUSIVE) {
        return admit_excision(obj);
    } else {
        return admit_shadow(obj);
    }
}

void RedletPitstop::evict(sfLiveHeader* obj) {
    if (!obj) return;
    auto* entry = find_by_slot(static_cast<void*>(obj));
    if (entry) {
        restore_return(entry, nullptr);
    } else {
        // Guarantee no lingering object pause remains on the slot so mutators can never deadlock
        RedletPointCoordinator::release_object_pause(static_cast<void*>(obj));
    }
}

bool RedletPitstop::restore_return(sacculus_pitstop_entry_t* entry, sfLiveHeader** out_restored) {
    if (!entry) return false;

    void* const raw_slot = entry->slot_addr;
    SF_custominiVm_live_header_t* slot = nullptr;
    if (entry->mode == sf_puzzler_morph_e::EXCISION_EXCLUSIVE) {
        auto* eb = static_cast<SF_custominiVm_emptybox_header_t*>(raw_slot);
        eb->~SF_custominiVm_emptybox_header_t();
        slot = new (raw_slot) SF_custominiVm_live_header_t();
        slot->relocate_or_clone_from(&entry->saved_header, /*transfer_cold_ownership=*/true);
        slot->has_optimized.store(1, std::memory_order_release);
        slot->opt_success = 1;
        slot->last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
    } else {
        slot = static_cast<SF_custominiVm_live_header_t*>(raw_slot);
        slot->apply_optimized_state_from(entry->saved_header);
    }

    // Verify and reseal SF_TAIL_CANARY if payload footprint is present
    if (slot != nullptr && slot->memory_footprint.load(std::memory_order_relaxed) > 0) {
        if (!slot->isTailCanaryValid()) {
            slot->writeTailCanary();
        }
    }

    if (g_sf_global_volatility.load(std::memory_order_relaxed) == static_cast<uint8_t>(sf_global_volatility_e::SRAM)) {
        sf_sram_commit_to_dram(slot, sizeof(SF_custominiVm_live_header_t));
    }

    {
        sf_ticket_guard_t g(_lock);
        if (entry->prev) entry->prev->next = entry->next;
        else _head = entry->next;
        if (entry->next) entry->next->prev = entry->prev;
        entry->next = entry->prev = nullptr;
        if (_count > 0) --_count;
    }

    // Always release the per-object Pausepoint before waking waiters or invoking ready callbacks
    release_entry_pause_if_held(entry);

    sf_object_ready_callback_t cb = nullptr;
    void* cb_user = nullptr;
    {
        std::lock_guard<std::mutex> lk(entry->wait_mtx);
        entry->is_busy.store(false, std::memory_order_release);
        cb = entry->ready_callback;
        cb_user = entry->callback_user_data;
        entry->wait_cv.notify_all();
    }
    if (cb) {
        sf_hotlog_emit(SF_Ebin0_PITSTOP_CALLBACK, SF_Ebin1_INFO, slot->function_id,
                       slot->active_simd_idx, slot->generation, slot->region, 0, 0, 0, "pitstop_ready_cb");
        cb(slot->body(), cb_user);
    }

    if (out_restored) *out_restored = slot;

    sf_hotlog_emit(SF_Ebin0_PITSTOP_RETURN, SF_Ebin1_INFO, slot->function_id, slot->active_simd_idx,
                   slot->generation, slot->region, 0, 0, 0, "pitstop_return");

    entry->release();
    return true;
}

bool RedletPitstop::morph_isa_to_fledgling(sfLiveHeader* obj, uint16_t target_simd_idx, uint64_t cycles_gained) {
    if (!obj || obj->sfMN != SF_LIVE_MAGIC) {
        return false;
    }
    if (obj->opt_permitted == 0u) {
        return false; // Locked by BYPASS_WARMUP
    }
    if (target_simd_idx >= 8u) {
        target_simd_idx = 0u;
    }

    const uint64_t morph_start_tsc = sacculus_rdtsc();

    // Pre-verify tail canary integrity before staging into Pitstop
    const size_t body_fp = obj->memory_footprint.load(std::memory_order_relaxed);
    if (body_fp > 0 && !obj->isTailCanaryValid()) {
        if (auto* cold = obj->get_cold()) {
            cold->error_count.fetch_add(1u, std::memory_order_relaxed);
        }
        obj->state.store(static_cast<uint8_t>(sf_live_state_e::QUARANTINED), std::memory_order_release);
        sf_hotlog_emit(SF_Ebin0_HEAL_CANARY_QUARANTINE, SF_Ebin1_ERROR, obj->function_id,
                       target_simd_idx, obj->generation, obj->region, 0, 0,
                       static_cast<uint32_t>(body_fp / 1024), "pitstop_canary_fault");
        return false;
    }

    // Stage object in Pitstop according to RedletFlags / morph policy (engages Pausepoint + EmptyBox in excision)
    sacculus_pitstop_entry_t* entry = nullptr;
    if (resolve_morph_mode() == sf_puzzler_morph_e::EXCISION_EXCLUSIVE) {
        entry = admit_excision(obj);
    } else {
        entry = admit_shadow(obj);
    }
    if (!entry) {
        RedletPointCoordinator::release_object_pause(static_cast<void*>(obj));
        return false;
    }

    // Mutate saved_header optimization state (Egg -> Fledgling)
    SF_custominiVm_live_header_t& staged = entry->saved_header;
    if (staged.simd_candidate_count <= target_simd_idx) {
        staged.simd_candidate_count = static_cast<uint16_t>(target_simd_idx + 1u);
    }
    staged.active_simd_idx = target_simd_idx;
    for (uint16_t i = 0u; i < staged.simd_candidate_count && i < 8u; ++i) {
        staged.simd_table[i].is_active = (i == target_simd_idx) ? 1u : 0u;
    }

    const uint64_t morph_end_tsc = sacculus_rdtsc();
    const uint64_t elapsed_cycles = (morph_end_tsc >= morph_start_tsc) ? (morph_end_tsc - morph_start_tsc) : 1ULL;

    staged.cycles_gained.store(cycles_gained, std::memory_order_release);
    staged.optimization_duration_cycles.store(elapsed_cycles, std::memory_order_release);
    staged.last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
    staged.opt_success = 1u;
    staged.has_optimized.store(1u, std::memory_order_release);

    sfLiveHeader* restored = nullptr;
    const bool ok = restore_return(entry, &restored);
    if (!ok || restored == nullptr) {
        RedletPointCoordinator::release_object_pause(static_cast<void*>(obj));
        return false;
    }

    // Reseal and verify SF_TAIL_CANARY post-restoration
    if (restored->memory_footprint.load(std::memory_order_relaxed) > 0) {
        restored->writeTailCanary();
    }
    restored->last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
    restored->cycles_gained.store(cycles_gained, std::memory_order_release);
    restored->has_optimized.store(1u, std::memory_order_release);

    // Defensive guarantee that Pausepoint on obj is fully released
    RedletPointCoordinator::release_object_pause(static_cast<void*>(restored));
    return true;
}

sacculus_pitstop_entry_t* RedletPitstop::find_by_slot(void* slot_or_body, bool retain_ref) {
    if (!slot_or_body) return nullptr;
    sf_ticket_guard_t g(_lock);
    auto* cur = _head;
    size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    while (cur) {
        if (cur->slot_addr == slot_or_body ||
            static_cast<uint8_t*>(cur->slot_addr) + hdr_sz == slot_or_body) {
            if (retain_ref) cur->retain();
            return cur;
        }
        cur = cur->next;
    }
    return nullptr;
}

bool RedletPitstop::is_busy(const void* ptr) {
    if (!ptr) return false;
    size_t hdr_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
    const void* hdr_ptr = static_cast<const uint8_t*>(ptr) - hdr_sz;

    sf_ticket_guard_t g(_lock);
    for (auto* cur = _head; cur; cur = cur->next) {
        if (cur->slot_addr == ptr || cur->slot_addr == hdr_ptr) {
            return cur->is_busy.load(std::memory_order_acquire);
        }
    }
    return false;
}

int RedletPitstop::await_ready(void* ptr, uint32_t timeout_ms) {
    if (!ptr) return -1;
    sacculus_pitstop_entry_t* entry = find_by_slot(ptr, /*retain_ref=*/true);
    if (!entry) return 0;

    if (!entry->is_busy.load(std::memory_order_acquire)) {
        entry->release();
        return 0;
    }

    sf_hotlog_emit(SF_Ebin0_PITSTOP_AWAIT_STALL, SF_Ebin1_WARN,
                   entry->saved_header.function_id, 0,
                   entry->saved_header.generation, entry->saved_header.region,
                   0, 0, timeout_ms, "pitstop_await_stall");
    int rc = 0;
    {
        std::unique_lock<std::mutex> lk(entry->wait_mtx);
        if (timeout_ms == 0) {
            entry->wait_cv.wait(lk, [&]() {
                return !entry->is_busy.load(std::memory_order_acquire);
            });
            rc = 0;
        } else {
            bool ok = entry->wait_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]() {
                return !entry->is_busy.load(std::memory_order_acquire);
            });
            rc = ok ? 0 : -1;
        }
    }
    entry->release();
    return rc;
}

int RedletPitstop::register_callback(void* ptr, sf_object_ready_callback_t callback, void* user_data) {
    if (!ptr || !callback) return -1;
    sacculus_pitstop_entry_t* entry = find_by_slot(ptr, /*retain_ref=*/true);
    if (!entry) {
        callback(ptr, user_data);
        return 0;
    }
    bool invoke_now = false;
    {
        std::lock_guard<std::mutex> lk(entry->wait_mtx);
        if (!entry->is_busy.load(std::memory_order_acquire)) {
            invoke_now = true;
        } else {
            entry->ready_callback = callback;
            entry->callback_user_data = user_data;
        }
    }
    entry->release();
    if (invoke_now) {
        sf_hotlog_emit(SF_Ebin0_PITSTOP_CALLBACK, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0, 0, "pitstop_cb_immediate");
        callback(ptr, user_data);
    }
    return 0;
}
