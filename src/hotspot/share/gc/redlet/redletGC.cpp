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

#include "gc/redlet/redletGC.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include "gc/redlet/redletPuzzleArena.hpp"
#include "gc/redlet/redletALU.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include <cstdlib>

#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_trace(...) if (false) printf
#endif

static inline void sf_cpu_pause() noexcept {
    sf_ring_cpu_pause();
}

static inline void sf_sync_store_fence() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

static RedletGC g_default_redlet_gc;

/* ========================================================================= */
/* 1. CONSTRUCTOR & INITIALIZATION                                           */
/* ========================================================================= */

MSIMD_API RedletGC::RedletGC() {
    init();
}

MSIMD_API RedletGC::~RedletGC() {
    stop();
}

MSIMD_API void RedletGC::init(RedletRunway* r,
                             RedletMotherline* m,
                             RedletFalline* f,
                             RedletLandfill* l,
                             size_t pagefile_bytes) {
    if (r) _runway = r;
    if (m) _motherline = m;
    if (f) _falline = f;
    if (l) _landfill = l;

    pagefile.init(pagefile_bytes, SF_PAGING_FIFO);
    current_phase.store(SF_GC_PHASE_IDLE, std::memory_order_relaxed);
    matured_flush_count.store(0, std::memory_order_relaxed);
    rehab_scan_count.store(0, std::memory_order_relaxed);
    resurrection_count.store(0, std::memory_order_relaxed);
    falline_compact_count.store(0, std::memory_order_relaxed);
    last_sweep_ns.store(0, std::memory_order_relaxed);
    last_line_compaction_ts_ns.store(0, std::memory_order_relaxed);

    for (uint32_t i = 0; i < RESURRECT_QUEUE_CAP; ++i) {
        resurrect_queue[i].store(0, std::memory_order_relaxed);
    }
    resurrect_write.store(0, std::memory_order_relaxed);
    resurrect_read.store(0, std::memory_order_relaxed);
}

MSIMD_API void RedletGC::set_gc_phase(sf_gc_phase_e next_phase, const char* phase_label) {
    (void)phase_label;
    current_phase.store(static_cast<uint8_t>(next_phase), std::memory_order_acq_rel);
}

MSIMD_API void RedletGC::trigger_hook(SF_custominiVm_gc_hook_event_e ev) {
    if (gc_hook) {
        gc_hook(ev, sweep_count.load(std::memory_order_relaxed),
                total_heap_bytes.load(std::memory_order_relaxed),
                gc_hook_user);
    }
}

/* ========================================================================= */
/* 2. 4-POINTS COOPERATIVE RENDEZVOUS HANDSHAKE                              */
/* ========================================================================= */

MSIMD_API void RedletGC::wait_for_safepoint_rendezvous(void) {
    auto* coord = RedletGearkit::point_coordinator();
    if (!coord) return;

    uint32_t in_flight = coord->active_in_flight_kernels.load(std::memory_order_acquire);
    if (in_flight == 0) return;

    // Fast SMT-aware spin phase
    uint32_t spin = 0;
    constexpr uint32_t MAX_RENDEZVOUS_SPINS = 256;
    while (spin < MAX_RENDEZVOUS_SPINS) {
        uint32_t cur_inf = coord->active_in_flight_kernels.load(std::memory_order_acquire);
        uint32_t cur_parked = coord->safepoint_parked_threads.load(std::memory_order_acquire);
        if (cur_inf == 0 || cur_parked >= cur_inf) {
            return;
        }
        sf_smt_ht_engine_t::smt_pause_backoff(spin);
        spin++;
    }

    // Atomic wait on rendezvous ticket
    while (true) {
        uint64_t cur_ticket = coord->rendezvous_ticket.load(std::memory_order_acquire);
        uint32_t cur_inf = coord->active_in_flight_kernels.load(std::memory_order_acquire);
        uint32_t cur_parked = coord->safepoint_parked_threads.load(std::memory_order_acquire);
        if (cur_inf == 0 || cur_parked >= cur_inf) {
            break;
        }
        coord->rendezvous_ticket.wait(cur_ticket, std::memory_order_relaxed);
    }
}

/* ========================================================================= */
/* 3. OBJECT ADMISSION & LIFECYCLE                                           */
/* ========================================================================= */

MSIMD_API void RedletGC::admit(sfLiveHeader* obj) {
    if (!obj) return;
    total_heap_bytes.fetch_add((size_t)obj->memory_footprint.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    _motherline->admit(obj);
}

MSIMD_API void RedletGC::admit_egg_object(sfLiveHeader* obj) {
    if (!obj) return;
    total_heap_bytes.fetch_add((size_t)obj->memory_footprint.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    _motherline->admit_egg_object(obj);
}

MSIMD_API void RedletGC::mark_garbage(sfLiveHeader* obj) {
    if (!obj || obj->is_pinned()) return;
    if (obj->falline_marked) return;
    if (obj->child_view_count.load(std::memory_order_acquire) > 0) {
        if (!salvage_orphaned_slice_views(obj)) return;
    }
    _motherline->evict(obj);
    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    size_t cell_sz = obj->total_cell_bytes > 0 ? obj->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
    uint32_t fid = obj->function_id;
    obj->~SF_custominiVm_live_header_t();
    auto* eb = new (obj) SF_custominiVm_emptybox_header_t();
    eb->init(fid, fp, cell_sz, nullptr, sweep_count.load(std::memory_order_relaxed));
    _falline->register_emptybox(cell_sz, eb, sweep_count.load(std::memory_order_relaxed));
}

/* ========================================================================= */
/* 4. SLICE VIEWS SALVAGE & RESURRECTION ENGINE                              */
/* ========================================================================= */

MSIMD_API bool RedletGC::salvage_orphaned_slice_views(sfLiveHeader* parent, sf_region_t* already_locked_region) {
    if (!parent || parent->child_view_count.load(std::memory_order_acquire) == 0) return true;

    size_t parent_fp = (size_t)parent->memory_footprint.load(std::memory_order_relaxed);
    if (parent_fp < 128 * 1024) return false;

    auto* parent_cold = get_cold(parent);
    if (parent_cold && parent_cold->is_paged_out.load(std::memory_order_acquire)) {
        pagefile.page_in(parent);
    }

    std::vector<sfLiveHeader*> views;
    for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
        for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
            auto& region = _motherline->room(r).regions[reg];
            if (&region == already_locked_region) {
                auto* cur = region.get_head();
                while (cur) {
                    if (cur->parent_header.load(std::memory_order_relaxed) == parent) {
                        if (cur->try_pin()) views.push_back(cur);
                    }
                    cur = cur->next;
                }
            } else {
                sf_ticket_guard_t g(region.get_lock());
                auto* cur = region.get_head();
                while (cur) {
                    if (cur->parent_header.load(std::memory_order_relaxed) == parent) {
                        if (cur->try_pin()) views.push_back(cur);
                    }
                    cur = cur->next;
                }
            }
        }
    }

    bool all_salvaged = true;
    for (auto* cur : views) {
        if (cur->parent_header.load(std::memory_order_acquire) == parent) {
            size_t slice_bytes = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
            void* slice_src = cur->resolve_active_body();
            if (slice_bytes > 0 && slice_src) {
                size_t cell_sz = SF_custominiVm_live_header_t::total_cell_size(slice_bytes);
                void* raw = _runway->bump_alloc(cell_sz);
                if (raw) {
                    auto* new_obj = new (raw) SF_custominiVm_live_header_t();
                    new_obj->init(cur->function_id, 0, "salvaged_slice");
                    new_obj->memory_footprint.store(slice_bytes, std::memory_order_relaxed);
                    new_obj->total_cell_bytes = cell_sz;
                    memcpy(new_obj->body(), slice_src, slice_bytes);
                    new_obj->writeTailCanary();
                    total_heap_bytes.fetch_add(slice_bytes, std::memory_order_relaxed);

                    cur->forwarding_address.store(new_obj->body(), std::memory_order_release);
                    cur->parent_header.store(nullptr, std::memory_order_release);
                    cur->slice_byte_offset = 0;
                    parent->child_view_count.fetch_sub(1, std::memory_order_acq_rel);

                    uint8_t treg = new_obj->compute_target_region();
                    auto& target_region = _motherline->room(SF_ROOM_YOUNG).regions[treg];
                    new_obj->generation = (uint8_t)SF_ROOM_YOUNG;
                    if (&target_region == already_locked_region) {
                        target_region.place_nolock(new_obj);
                    } else {
                        target_region.place(new_obj);
                    }
                    _motherline->room(SF_ROOM_YOUNG).total_objects.fetch_add(1, std::memory_order_relaxed);
                    _motherline->room(SF_ROOM_YOUNG).total_bytes.fetch_add(slice_bytes, std::memory_order_relaxed);
                    sf_hotlog_emit(SF_Ebin0_SLICEVIEW_SALVAGED, SF_Ebin1_INFO,
                        new_obj->function_id, 0, new_obj->generation, new_obj->region,
                        0, sweep_count.load(std::memory_order_relaxed),
                        (uint32_t)(slice_bytes / 1024), "sliceview_salvaged");
                } else {
                    all_salvaged = false;
                }
            }
        }
        cur->unpin();
    }
    return all_salvaged && (parent->child_view_count.load(std::memory_order_acquire) == 0);
}

MSIMD_API bool RedletGC::restore_rehab_payload_and_link(const rehab_restore_info_t& info) {
    uint8_t* rehab_base = _landfill->pit().rehab_arena.base.load(std::memory_order_acquire);
    uint8_t* stored_cell = rehab_base ? (rehab_base + info.rehab_offset) : (uint8_t*)info.rehab_payload_ptr;
    if (!stored_cell) return false;

    auto* old_hdr = reinterpret_cast<SF_custominiVm_live_header_t*>(stored_cell);
    if (!old_hdr || !old_hdr->isValid()) return false;
    const uint8_t* old_body = old_hdr->body();
    size_t body_bytes = info.previous_size;

    if (info.crc32 != 0 && body_bytes > 0) {
        uint32_t computed_crc = sf_compute_crc32(old_body, body_bytes);
        if (computed_crc != info.crc32) {
            return false;
        }
    }
    if (body_bytes > 0 && !old_hdr->isTailCanaryValid()) {
        return false;
    }

    size_t cell_sz = SF_custominiVm_live_header_t::total_cell_size(body_bytes);
    auto* eb = _falline->snatch_emptybox(cell_sz, sweep_count.load(std::memory_order_relaxed));
    SF_custominiVm_live_header_t* new_obj = nullptr;

    if (eb && eb->isValid() && eb->aligned_hole_size >= cell_sz) {
        size_t box_capacity = eb->aligned_hole_size;
        new_obj = new (eb) SF_custominiVm_live_header_t();
        new_obj->init(info.function_id, info.type_sig_hash, "resurrected_tensor");
        new_obj->memory_footprint.store(body_bytes, std::memory_order_relaxed);
        new_obj->total_cell_bytes = box_capacity;
        sf_hotlog_emit(SF_Ebin0_FALLINE_SNATCH_HIT, SF_Ebin1_TRACE, info.function_id, 0, 0, 0, 0, 0, (uint32_t)(cell_sz / 1024), "resurrect_snatch_hit");
    } else {
        void* raw = _runway->bump_alloc(cell_sz);
        if (!raw) return false;
        new_obj = new (raw) SF_custominiVm_live_header_t();
        new_obj->init(info.function_id, info.type_sig_hash, "resurrected_tensor");
        new_obj->memory_footprint.store(body_bytes, std::memory_order_relaxed);
        new_obj->total_cell_bytes = cell_sz;
        sf_hotlog_emit(SF_Ebin0_ALLOC_BUMP, SF_Ebin1_TRACE, info.function_id, 0, 0, 0, 0, 0, (uint32_t)(cell_sz / 1024), "resurrect_bump_alloc");
    }

    auto* new_cold = get_cold(new_obj);
    if (new_cold) {
        new_cold->type_sig_hash = info.type_sig_hash;
        new_cold->preferred_numa_node = (uint8_t)(info.last_core / 64);
    }

    if (body_bytes > 0 && old_body) {
        memcpy(new_obj->body(), old_body, body_bytes);
    }
    new_obj->writeTailCanary();

    admit_egg_object(new_obj);

    sf_hotlog_emit(SF_Ebin0_RESURRECT_RESTORED, SF_Ebin1_INFO,
        info.function_id, (uint16_t)info.pane_idx, info.peak_generation, info.peak_region,
        info.last_core, sweep_count.load(std::memory_order_relaxed),
        (uint32_t)(total_heap_bytes.load(std::memory_order_relaxed) / 1024),
        "rehab_resurrected_physical");

    resurrection_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

MSIMD_API size_t RedletGC::resurrect_entries_for_fn(uint32_t fn_id) {
    if (fn_id == 0) return 0;
    std::vector<rehab_restore_info_t> to_restore;
    {
        sf_ticket_guard_t g(_landfill->pane().lock);
        uint32_t cnt = _landfill->pane().count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < cnt; ++i) {
            auto& entry = _landfill->pane().entries[i];
            if (entry.function_id == fn_id && entry.lasting == 1) {
                entry.lasting = 0;
                rehab_restore_info_t info{};
                info.function_id = entry.function_id;
                info.type_sig_hash = entry.type_sig_hash;
                info.previous_size = entry.previous_size;
                info.pit_stored_size = entry.pit_stored_size;
                info.crc32 = entry.crc32;
                info.rehab_offset = entry.rehab_offset;
                info.rehab_payload_ptr = entry.rehab_payload_ptr;
                info.peak_generation = entry.peak_generation;
                info.peak_region = entry.peak_region;
                info.last_core = entry.last_core;
                info.pane_idx = i;
                to_restore.push_back(info);
            }
        }
    }

    size_t restored = 0;
    for (const auto& info : to_restore) {
        if (restore_rehab_payload_and_link(info)) {
            ++restored;
        } else {
            sf_ticket_guard_t g(_landfill->pane().lock);
            if (info.pane_idx < _landfill->pane().count.load(std::memory_order_relaxed)) {
                _landfill->pane().entries[info.pane_idx].lasting = 1;
            }
        }
    }
    return restored;
}

MSIMD_API size_t RedletGC::scan_rehab_arena(void) {
    set_gc_phase(SF_GC_PHASE_REHAB, "phase_rehab");
    rehab_scan_count.fetch_add(1, std::memory_order_relaxed);
    size_t resurrected = 0;
    std::vector<uint32_t> pending_fn_ids;

    {
        sf_ticket_guard_t g(resurrect_lock);
        uint32_t r = resurrect_read.load(std::memory_order_relaxed);
        uint32_t w = resurrect_write.load(std::memory_order_acquire);
        while (r != w) {
            uint32_t fn_id = resurrect_queue[r & (RESURRECT_QUEUE_CAP - 1)].exchange(0, std::memory_order_relaxed);
            if (fn_id != 0) {
                pending_fn_ids.push_back(fn_id);
            }
            ++r;
        }
        resurrect_read.store(r, std::memory_order_release);
    }

    for (uint32_t fn_id : pending_fn_ids) {
        resurrected += resurrect_entries_for_fn(fn_id);
    }

    set_gc_phase(SF_GC_PHASE_IDLE, "phase_idle");
    return resurrected;
}

MSIMD_API void RedletGC::request_resurrection(uint32_t fn_id) {
    sf_ticket_guard_t g(resurrect_lock);
    uint32_t r = resurrect_read.load(std::memory_order_relaxed);
    uint32_t cur_w = resurrect_write.load(std::memory_order_relaxed);
    if (cur_w - r >= RESURRECT_QUEUE_CAP) {
        sf_hotlog_emit(SF_Ebin0_RESURRECT_QUEUE_DROP, SF_Ebin1_WARN,
            fn_id, 0, 0, 0, 0, sweep_count.load(std::memory_order_relaxed), 0, "resurrect_queue_overflow");
        return;
    }
    resurrect_queue[cur_w & (RESURRECT_QUEUE_CAP - 1)].store(fn_id, std::memory_order_relaxed);
    resurrect_write.store(cur_w + 1, std::memory_order_release);
    sf_hotlog_emit(SF_Ebin0_RESURRECT_REQUEST, SF_Ebin1_INFO,
        fn_id, 0, 0, 0, 0, sweep_count.load(std::memory_order_relaxed), 0, "resurrect_request");
}

/* ========================================================================= */
/* 5. RMRP (REDUCE MEMORY RESIDENT PROPAGATION)                              */
/* ========================================================================= */

MSIMD_API size_t RedletGC::execute_rmrp(sf_snatch_rebate_mode_e rebate_mode, sf_rmrp_method_e method) {
    size_t total_reclaimed = 0;
    struct rmrp_span_t {
        void* addr;
        size_t bytes;
        uint32_t fn_id;
        bool is_landfill;
    };
    std::vector<rmrp_span_t> spans;

    // 1. Collect candidate spans from Motherline
    for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
        for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
            auto& region = _motherline->room(r).regions[reg];
            sf_ticket_guard_t g(region.get_lock());
            auto* cur = region.get_head();
            while (cur) {
                if (cur->parent_header.load(std::memory_order_relaxed) != nullptr) {
                    cur = cur->next;
                    continue;
                }
                size_t payload_bytes = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                size_t header_sz = (sizeof(SF_custominiVm_live_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
                size_t aligned_fp = (payload_bytes + 7) & ~7ULL;
                size_t needed = header_sz + aligned_fp + 8 + 1;
                needed = (needed + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);

                if (cur->total_cell_bytes > needed &&
                    (cur->total_cell_bytes - needed) >= (2 * SACFOR_CACHE_LINE)) {
                    size_t shrink_bytes = cur->total_cell_bytes - needed;
                    void* shrink_addr = reinterpret_cast<uint8_t*>(cur) + needed;
                    cur->total_cell_bytes = needed;
                    spans.push_back({ shrink_addr, shrink_bytes, cur->function_id, false });
                }
                cur = cur->next;
            }
        }
    }

    // 2. Collect candidate unused capacity from Landfill
    size_t mat_cap = _landfill->pit().matured_arena.capacity.load(std::memory_order_relaxed);
    size_t mat_used = _landfill->pit().matured_arena.used.load(std::memory_order_relaxed);
    if (mat_cap > mat_used + 4096) {
        size_t mat_tail = mat_cap - mat_used;
        uint8_t* mat_base = _landfill->pit().matured_arena.base.load(std::memory_order_relaxed);
        if (mat_base) {
            spans.push_back({ mat_base + mat_used, mat_tail, 0, true });
        }
    }

    size_t reh_cap = _landfill->pit().rehab_arena.capacity.load(std::memory_order_relaxed);
    size_t reh_used = _landfill->pit().rehab_arena.used.load(std::memory_order_relaxed);
    if (reh_cap > reh_used + 4096) {
        size_t reh_tail = reh_cap - reh_used;
        uint8_t* reh_base = _landfill->pit().rehab_arena.base.load(std::memory_order_relaxed);
        if (reh_base) {
            spans.push_back({ reh_base + reh_used, reh_tail, 0, true });
        }
    }

    // 3. Sort spans from smallest to largest
    std::sort(spans.begin(), spans.end(), [](const rmrp_span_t& a, const rmrp_span_t& b) {
        return a.bytes < b.bytes;
    });

    auto safe_decommit_span_pages = [](void* addr, size_t bytes, bool is_landfill) {
        uintptr_t start = reinterpret_cast<uintptr_t>(addr);
        if (!is_landfill) {
            start += sizeof(SF_custominiVm_emptybox_header_t);
        }
        uintptr_t page_start = (start + 4095ULL) & ~4095ULL;
        uintptr_t page_end = (reinterpret_cast<uintptr_t>(addr) + bytes) & ~4095ULL;
        if (page_end > page_start) {
            sf_os_decommit_memory(reinterpret_cast<void*>(page_start), page_end - page_start);
            return (size_t)(page_end - page_start);
        }
        return (size_t)0;
    };

    // 4. Execute reduction
    if (method == sf_rmrp_method_e::SEQUENTIAL) {
        for (const auto& sp : spans) {
            if (!sp.is_landfill && sp.bytes >= sizeof(SF_custominiVm_emptybox_header_t)) {
                auto* eb = new (sp.addr) SF_custominiVm_emptybox_header_t();
                eb->init(sp.fn_id, sp.bytes, sp.bytes, nullptr, sweep_count.load(std::memory_order_relaxed));
                _falline->register_emptybox(sp.bytes, eb, sweep_count.load(std::memory_order_relaxed));
                if (rebate_mode == sf_snatch_rebate_mode_e::RAPID ||
                    rebate_mode == sf_snatch_rebate_mode_e::DECOMMIT) {
                    size_t dec = safe_decommit_span_pages(sp.addr, sp.bytes, false);
                    if (dec > 0) {
                        eb->is_decommitted = 1;
                        eb->decommitted_kb = (uint32_t)(dec / 1024);
                    }
                }
            } else if (sp.is_landfill &&
                (rebate_mode == sf_snatch_rebate_mode_e::RAPID ||
                 rebate_mode == sf_snatch_rebate_mode_e::DECOMMIT)) {
                safe_decommit_span_pages(sp.addr, sp.bytes, true);
                sf_os_commit_memory(sp.addr, sp.bytes);
            }
            total_reclaimed += sp.bytes;
        }
    } else {
        // PLUMP
        for (const auto& sp : spans) {
            if (!sp.is_landfill && sp.bytes >= sizeof(SF_custominiVm_emptybox_header_t)) {
                auto* eb = new (sp.addr) SF_custominiVm_emptybox_header_t();
                eb->init(sp.fn_id, sp.bytes, sp.bytes, nullptr, sweep_count.load(std::memory_order_relaxed));
                _falline->register_emptybox(sp.bytes, eb, sweep_count.load(std::memory_order_relaxed));
            } else if (sp.is_landfill &&
                (rebate_mode == sf_snatch_rebate_mode_e::RAPID ||
                 rebate_mode == sf_snatch_rebate_mode_e::DECOMMIT)) {
                safe_decommit_span_pages(sp.addr, sp.bytes, true);
                sf_os_commit_memory(sp.addr, sp.bytes);
            }
            total_reclaimed += sp.bytes;
        }
    }

    if (!spans.empty()) {
        sf_hotlog_emit(SF_Ebin0_RMRP_SPAN_TRIM, SF_Ebin1_INFO, 0, (uint16_t)spans.size(),
            0, (uint8_t)method, 0, 0, (uint32_t)(total_reclaimed / 1024), "rmrp_spans_trimmed");
    }
    if (total_reclaimed > 0) {
        sf_hotlog_emit(SF_Ebin0_RMRP_COALESCE_DECOMMIT, SF_Ebin1_INFO, 0, (uint16_t)spans.size(),
            0, (uint8_t)rebate_mode, 0, 0, (uint32_t)(total_reclaimed / 1024), "rmrp_decommitted");
    }
    return total_reclaimed;
}

/* ========================================================================= */
/* 6. FALLINE DRAIN & LANDFILL EVACUATION                                    */
/* ========================================================================= */

MSIMD_API size_t RedletGC::drain_falline(void) {
    set_gc_phase(SF_GC_PHASE_DRAIN, "phase_drain");
    size_t drained_count = 0;

    if (_landfill->is_critical(landfill_critical_ratio)) {
        set_gc_phase(SF_GC_PHASE_EVACUATE, "phase_evacuate");
        sf_hotlog_emit(SF_Ebin0_LANDFILL_CRITICAL_WARN, SF_Ebin1_WARN,
            0, 0, 0, 0, 0, sweep_count.load(std::memory_order_relaxed),
            (uint32_t)(_landfill->pit().total_used() / 1024), "landfill_crit_warn");
        wait_for_safepoint_rendezvous();
        _landfill->evacuate(nullptr, sweep_count.load(std::memory_order_relaxed));
        matured_flush_count.fetch_add(1, std::memory_order_relaxed);
        set_gc_phase(SF_GC_PHASE_DRAIN, "phase_drain");
    }

    size_t eb_count = _falline->emptybox_count();
    drained_count = eb_count;
    set_gc_phase(SF_GC_PHASE_IDLE, "phase_idle");
    return drained_count;
}

/* ========================================================================= */
/* 7. GENERATIONAL ROOM SWEEPER (Young -> Adolescent -> Senior)              */
/* ========================================================================= */

MSIMD_API size_t RedletGC::sweep_single_room(sf_room_t& room, uint8_t room_id, uint64_t sweep_no,
                                            uint32_t heap_kb, float pressure_ratio,
                                            bool is_e_core, bool use_simd) {
    (void)heap_kb;
    size_t evicted_count = 0;
    float decay_f = heat_decay_factor;
    if (decay_f <= 0.0f || decay_f > 1.0f) decay_f = 0.97f;

    struct DeferredMigration {
        sfLiveHeader* obj;
        uint8_t       target_room;
        uint8_t       target_reg;
        bool          is_promotion;
    };
    std::vector<DeferredMigration> deferred_migrations;

    for (int reg_i = SACFOR_REGION_COUNT - 1; reg_i >= 0; --reg_i) {
        sf_region_t& region = room.regions[reg_i];
        sf_ticket_guard_t g(region.get_lock());
        sfLiveHeader* cur = region.get_head();

        if (is_e_core || !use_simd) {
            // E-Core / Cycles Path: Micro-chunking (32KB L1D) with NTA prefetch and pause backoff
            size_t chunk_ops = 0;
            while (cur) {
                sfLiveHeader* next_obj = cur->next;

                if (is_e_core && next_obj) {
#if defined(_M_X64) || defined(__x86_64__)
                    _mm_prefetch((const char*)next_obj, _MM_HINT_NTA);
#endif
                }

                cur->decay_heat();

                if (cur->is_pinned()) { cur = next_obj; continue; }

                if (cur->state.load(std::memory_order_acquire) == static_cast<uint8_t>(sf_live_state_e::QUARANTINED)) {
                    region.evict_nolock(cur);
                    _motherline->decrement_counts(cur);
                    ++evicted_count;
                    room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                    size_t fp = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                    size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                    while (cur_rb > 0) {
                        size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                        if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                            break;
                    }
                    if (auto* c_q = get_cold(cur); c_q && c_q->is_paged_out.load(std::memory_order_acquire)) {
                        pagefile.page_in(cur);
                    }
                    _landfill->accept(cur, SF_DEATH_ERROR, nullptr, sweep_no);

                    size_t chb = total_heap_bytes.load(std::memory_order_relaxed);
                    while (chb > 0) {
                        size_t next_v = (chb >= fp) ? (chb - fp) : 0;
                        if (total_heap_bytes.compare_exchange_weak(chb, next_v, std::memory_order_relaxed, std::memory_order_relaxed))
                            break;
                    }
                    uint32_t fid = cur->function_id;
                    size_t cell_sz = cur->total_cell_bytes > 0 ? cur->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                    cur->~SF_custominiVm_live_header_t();
                    auto* eb = new (cur) SF_custominiVm_emptybox_header_t();
                    eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                    _falline->register_emptybox(cell_sz, eb, sweep_no);
                    cur = next_obj;
                    continue;
                }

                if (cur->needs_rehabilitation) {
                    if (cur->try_transition_to_rehab()) {
                        if (auto* c_reh = get_cold(cur); c_reh && c_reh->is_paged_out.load(std::memory_order_acquire)) {
                            pagefile.page_in(cur);
                        }
                        region.evict_nolock(cur);
                        _motherline->decrement_counts(cur);
                        ++evicted_count;
                        room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                        size_t fp = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                        size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                        while (cur_rb > 0) {
                            size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                            if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        _landfill->accept(cur, SF_DEATH_REHABILITATE, nullptr, sweep_no);

                        size_t chb = total_heap_bytes.load(std::memory_order_relaxed);
                        while (chb > 0) {
                            size_t next_v = (chb >= fp) ? (chb - fp) : 0;
                            if (total_heap_bytes.compare_exchange_weak(chb, next_v, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        uint32_t fid = cur->function_id;
                        size_t cell_sz = cur->total_cell_bytes > 0 ? cur->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                        cur->~SF_custominiVm_live_header_t();
                        auto* eb = new (cur) SF_custominiVm_emptybox_header_t();
                        eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                        _falline->register_emptybox(cell_sz, eb, sweep_no);
                    }
                    cur = next_obj;
                    continue;
                }

                if (cur->is_idle()) {
                    if (cur->child_view_count.load(std::memory_order_acquire) > 0) {
                        salvage_orphaned_slice_views(cur, &region);
                    }
                    if (cur->child_view_count.load(std::memory_order_acquire) == 0 && cur->try_transition_to_falline()) {
                        region.evict_nolock(cur);
                        _motherline->decrement_counts(cur);
                        ++evicted_count;
                        room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                        size_t fp = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                        size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                        while (cur_rb > 0) {
                            size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                            if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        uint32_t fid = cur->function_id;
                        size_t cell_sz = cur->total_cell_bytes > 0 ? cur->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                        cur->~SF_custominiVm_live_header_t();
                        auto* eb = new (cur) SF_custominiVm_emptybox_header_t();
                        eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                        _falline->register_emptybox(cell_sz, eb, sweep_no);
                    }
                    cur = next_obj;
                    continue;
                }

                bool drop_by_pressure = false;
                float heat = cur->heat_score.load(std::memory_order_relaxed);
                if (pressure_ratio >= 0.85f && heat < SACFOR_HEAT_HOT) drop_by_pressure = true;
                else if (pressure_ratio >= 0.70f && cur->generation <= SF_ROOM_ADOLESCENT && heat < SACFOR_HEAT_COLD * 2.0f) drop_by_pressure = true;
                else if (pressure_ratio >= 0.50f && cur->generation == SF_ROOM_YOUNG && heat < SACFOR_HEAT_COLD) drop_by_pressure = true;

                if (drop_by_pressure) {
                    if (cur->child_view_count.load(std::memory_order_acquire) > 0) {
                        salvage_orphaned_slice_views(cur, &region);
                    }
                    if (cur->child_view_count.load(std::memory_order_acquire) == 0 && cur->try_transition_to_falline()) {
                        region.evict_nolock(cur);
                        _motherline->decrement_counts(cur);
                        ++evicted_count;
                        room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                        size_t fp = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                        size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                        while (cur_rb > 0) {
                            size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                            if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        uint32_t fid = cur->function_id;
                        size_t cell_sz = cur->total_cell_bytes > 0 ? cur->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                        cur->~SF_custominiVm_live_header_t();
                        auto* eb = new (cur) SF_custominiVm_emptybox_header_t();
                        eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                        _falline->register_emptybox(cell_sz, eb, sweep_no);
                    }
                    cur = next_obj;
                    continue;
                }

                uint8_t young_max = SACFOR_GEN_YOUNG_MAX;
                uint8_t adol_max = SACFOR_GEN_ADOL_MAX;
                if (pressure_ratio >= 0.70f) { young_max /= 2; adol_max /= 2; }
                if (young_max < 2) young_max = 2;
                if (adol_max < 4) adol_max = 4;

                uint8_t new_gen = cur->generation;
                if (cur->generation == SF_ROOM_YOUNG && cur->gc_sweep_count > young_max)
                    new_gen = SF_ROOM_ADOLESCENT;
                else if (cur->generation == SF_ROOM_ADOLESCENT && cur->gc_sweep_count > adol_max)
                    new_gen = SF_ROOM_SENIOR;

                if (new_gen != cur->generation) {
                    region.evict_nolock(cur);
                    room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                    size_t fp = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                    size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                    while (cur_rb > 0) {
                        size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                        if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                            break;
                    }
                    cur->gc_sweep_count = 0;
                    deferred_migrations.push_back({ cur, new_gen, 0, true });
                    cur = next_obj;
                    continue;
                }

                uint8_t target_reg = cur->compute_target_region();
                if (target_reg != cur->region) {
                    region.evict_nolock(cur);
                    deferred_migrations.push_back({ cur, room_id, target_reg, false });
                    cur = next_obj;
                    continue;
                }

                cur = next_obj;
                if (is_e_core && (++chunk_ops % 16 == 0)) {
                    sf_cpu_pause();
                }
            }
        } else {
            // P-Core / SIMD Path: Vectorized 8-element batches with aggressive T0 prefetch
            static constexpr size_t SIMD_BATCH_SZ = 8;
            sfLiveHeader* batch[SIMD_BATCH_SZ];

            while (cur) {
                size_t count = 0;
                sfLiveHeader* walker = cur;
                while (walker && count < SIMD_BATCH_SZ) {
                    batch[count++] = walker;
#if defined(_M_X64) || defined(__x86_64__)
                    _mm_prefetch((const char*)walker, _MM_HINT_T0);
#endif
                    walker = walker->next;
                }

                if (count == 0) break;

#if defined(__AVX2__) || (defined(_M_X64) && defined(__AVX2__))
                if (count == 8) {
                    __m256 v_heats = _mm256_set_ps(
                        batch[7]->heat_score.load(std::memory_order_relaxed),
                        batch[6]->heat_score.load(std::memory_order_relaxed),
                        batch[5]->heat_score.load(std::memory_order_relaxed),
                        batch[4]->heat_score.load(std::memory_order_relaxed),
                        batch[3]->heat_score.load(std::memory_order_relaxed),
                        batch[2]->heat_score.load(std::memory_order_relaxed),
                        batch[1]->heat_score.load(std::memory_order_relaxed),
                        batch[0]->heat_score.load(std::memory_order_relaxed)
                    );
                    __m256 v_decay = _mm256_set1_ps(decay_f);
                    __m256 v_res = _mm256_mul_ps(v_heats, v_decay);
                    float decayed[8];
                    _mm256_storeu_ps(decayed, v_res);
                    for (size_t b = 0; b < 8; ++b) {
                        batch[b]->heat_score.store(decayed[b], std::memory_order_relaxed);
                        batch[b]->gc_sweep_count++;
                    }
                } else {
                    for (size_t b = 0; b < count; ++b) batch[b]->decay_heat();
                }
#else
                for (size_t b = 0; b < count; ++b) batch[b]->decay_heat();
#endif

                for (size_t b = 0; b < count; ++b) {
                    auto* item = batch[b];
                    if (item->is_pinned()) continue;

                    if (item->state.load(std::memory_order_acquire) == static_cast<uint8_t>(sf_live_state_e::QUARANTINED)) {
                        region.evict_nolock(item);
                        _motherline->decrement_counts(item);
                        ++evicted_count;
                        room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                        size_t fp = (size_t)item->memory_footprint.load(std::memory_order_relaxed);
                        size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                        while (cur_rb > 0) {
                            size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                            if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        if (auto* c_q = get_cold(item); c_q && c_q->is_paged_out.load(std::memory_order_acquire)) {
                            pagefile.page_in(item);
                        }
                        _landfill->accept(item, SF_DEATH_ERROR, nullptr, sweep_no);

                        size_t chb = total_heap_bytes.load(std::memory_order_relaxed);
                        while (chb > 0) {
                            size_t next_v = (chb >= fp) ? (chb - fp) : 0;
                            if (total_heap_bytes.compare_exchange_weak(chb, next_v, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        uint32_t fid = item->function_id;
                        size_t cell_sz = item->total_cell_bytes > 0 ? item->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                        item->~SF_custominiVm_live_header_t();
                        auto* eb = new (item) SF_custominiVm_emptybox_header_t();
                        eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                        _falline->register_emptybox(cell_sz, eb, sweep_no);
                        continue;
                    }

                    if (item->needs_rehabilitation) {
                        if (item->try_transition_to_rehab()) {
                            if (auto* c_reh = get_cold(item); c_reh && c_reh->is_paged_out.load(std::memory_order_acquire)) {
                                pagefile.page_in(item);
                            }
                            region.evict_nolock(item);
                            _motherline->decrement_counts(item);
                            ++evicted_count;
                            room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                            size_t fp = (size_t)item->memory_footprint.load(std::memory_order_relaxed);
                            size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                            while (cur_rb > 0) {
                                size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                                if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                    break;
                            }
                            _landfill->accept(item, SF_DEATH_REHABILITATE, nullptr, sweep_no);

                            size_t chb = total_heap_bytes.load(std::memory_order_relaxed);
                            while (chb > 0) {
                                size_t next_v = (chb >= fp) ? (chb - fp) : 0;
                                if (total_heap_bytes.compare_exchange_weak(chb, next_v, std::memory_order_relaxed, std::memory_order_relaxed))
                                    break;
                            }
                            uint32_t fid = item->function_id;
                            size_t cell_sz = item->total_cell_bytes > 0 ? item->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                            item->~SF_custominiVm_live_header_t();
                            auto* eb = new (item) SF_custominiVm_emptybox_header_t();
                            eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                            _falline->register_emptybox(cell_sz, eb, sweep_no);
                        }
                        continue;
                    }

                    if (item->is_idle()) {
                        if (item->child_view_count.load(std::memory_order_acquire) > 0) {
                            salvage_orphaned_slice_views(item, &region);
                        }
                        if (item->child_view_count.load(std::memory_order_acquire) == 0 && item->try_transition_to_falline()) {
                            region.evict_nolock(item);
                            _motherline->decrement_counts(item);
                            ++evicted_count;
                            room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                            size_t fp = (size_t)item->memory_footprint.load(std::memory_order_relaxed);
                            size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                            while (cur_rb > 0) {
                                size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                                if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                    break;
                            }
                            uint32_t fid = item->function_id;
                            size_t cell_sz = item->total_cell_bytes > 0 ? item->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                            item->~SF_custominiVm_live_header_t();
                            auto* eb = new (item) SF_custominiVm_emptybox_header_t();
                            eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                            _falline->register_emptybox(cell_sz, eb, sweep_no);
                        }
                        continue;
                    }

                    bool drop_by_pressure = false;
                    float heat = item->heat_score.load(std::memory_order_relaxed);
                    if (pressure_ratio >= 0.85f && heat < SACFOR_HEAT_HOT) drop_by_pressure = true;
                    else if (pressure_ratio >= 0.70f && item->generation <= SF_ROOM_ADOLESCENT && heat < SACFOR_HEAT_COLD * 2.0f) drop_by_pressure = true;
                    else if (pressure_ratio >= 0.50f && item->generation == SF_ROOM_YOUNG && heat < SACFOR_HEAT_COLD) drop_by_pressure = true;

                    if (drop_by_pressure) {
                        if (item->child_view_count.load(std::memory_order_acquire) > 0) {
                            salvage_orphaned_slice_views(item, &region);
                        }
                        if (item->child_view_count.load(std::memory_order_acquire) == 0 && item->try_transition_to_falline()) {
                            region.evict_nolock(item);
                            _motherline->decrement_counts(item);
                            ++evicted_count;
                            room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                            size_t fp = (size_t)item->memory_footprint.load(std::memory_order_relaxed);
                            size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                            while (cur_rb > 0) {
                                size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                                if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                    break;
                            }
                            uint32_t fid = item->function_id;
                            size_t cell_sz = item->total_cell_bytes > 0 ? item->total_cell_bytes : SF_custominiVm_live_header_t::total_cell_size(fp);
                            item->~SF_custominiVm_live_header_t();
                            auto* eb = new (item) SF_custominiVm_emptybox_header_t();
                            eb->init(fid, fp, cell_sz, nullptr, sweep_no);
                            _falline->register_emptybox(cell_sz, eb, sweep_no);
                        }
                        continue;
                    }

                    uint8_t young_max = SACFOR_GEN_YOUNG_MAX;
                    uint8_t adol_max = SACFOR_GEN_ADOL_MAX;
                    if (pressure_ratio >= 0.70f) { young_max /= 2; adol_max /= 2; }
                    if (young_max < 2) young_max = 2;
                    if (adol_max < 4) adol_max = 4;

                    uint8_t new_gen = item->generation;
                    if (item->generation == SF_ROOM_YOUNG && item->gc_sweep_count > young_max)
                        new_gen = SF_ROOM_ADOLESCENT;
                    else if (item->generation == SF_ROOM_ADOLESCENT && item->gc_sweep_count > adol_max)
                        new_gen = SF_ROOM_SENIOR;

                    if (new_gen != item->generation) {
                        region.evict_nolock(item);
                        room.total_objects.fetch_sub(1, std::memory_order_relaxed);
                        size_t fp = (size_t)item->memory_footprint.load(std::memory_order_relaxed);
                        size_t cur_rb = room.total_bytes.load(std::memory_order_relaxed);
                        while (cur_rb > 0) {
                            size_t next_b = (cur_rb >= fp) ? (cur_rb - fp) : 0;
                            if (room.total_bytes.compare_exchange_weak(cur_rb, next_b, std::memory_order_relaxed, std::memory_order_relaxed))
                                break;
                        }
                        item->gc_sweep_count = 0;
                        deferred_migrations.push_back({ item, new_gen, 0, true });
                        continue;
                    }

                    uint8_t target_reg = item->compute_target_region();
                    if (target_reg != item->region) {
                        region.evict_nolock(item);
                        deferred_migrations.push_back({ item, room_id, target_reg, false });
                        continue;
                    }
                }
                cur = walker;
            }
        }
    }

    // Apply deferred promotions and migrations
    for (const auto& mig : deferred_migrations) {
        if (mig.is_promotion) {
            _motherline->room(mig.target_room).place(mig.obj);
        } else {
            room.regions[mig.target_reg].place(mig.obj);
        }
    }

    return evicted_count;
}

MSIMD_API size_t RedletGC::sweep_motherline_parallel(bool use_simd) {
    set_gc_phase(SF_GC_PHASE_SWEEP, "phase_sweep");
    trigger_hook(SF_GC_HOOK_PRE_SWEEP);

    size_t cur_bytes = total_heap_bytes.load(std::memory_order_relaxed);
    float pressure_ratio = pressure_threshold.load(std::memory_order_relaxed) > 0 ?
        ((float)cur_bytes / (float)pressure_threshold.load(std::memory_order_relaxed)) : 0.0f;
    uint64_t sweep_no = sweep_count.load(std::memory_order_relaxed);
    uint32_t heap_kb = (uint32_t)(cur_bytes / 1024);

    size_t evicted_count = 0;
    bool is_e = sf_is_efficiency_core_active();

    // Sweep from Senior (2) down to Young (0) so promoted cells are not double-swept in the same step
    for (int r = SACFOR_ROOM_COUNT - 1; r >= 0; --r) {
        evicted_count += sweep_single_room(_motherline->room(r), (uint8_t)r, sweep_no, heap_kb, pressure_ratio, is_e, use_simd);
    }

    trigger_hook(SF_GC_HOOK_POST_SWEEP);
    return evicted_count;
}

MSIMD_API size_t RedletGC::sweep_motherline_cycles(void) {
    return sweep_motherline_parallel(false);
}

MSIMD_API size_t RedletGC::sweep_motherline_simd(void) {
    return sweep_motherline_parallel(true);
}

MSIMD_API size_t RedletGC::sweep_motherline(void) {
    return sweep_motherline_simd();
}

/* ========================================================================= */
/* 8. COMPACTION IMPLEMENTATION                                              */
/* ========================================================================= */

MSIMD_API void RedletGC::maybe_compact_falline(void) {
    size_t eb_count = _falline->emptybox_count();
    if (eb_count > 64) {
        set_gc_phase(SF_GC_PHASE_COMPACT, "phase_compact");
        wait_for_safepoint_rendezvous();
        falline_compact_count.fetch_add(1, std::memory_order_relaxed);
        set_gc_phase(SF_GC_PHASE_IDLE, "phase_idle");
    }
}

MSIMD_API void RedletGC::compact_single_room(sf_room_t& room, uint8_t room_id, uint64_t sweep_no,
                                            size_t max_compact_fp, bool use_simd,
                                            size_t& out_relocated_count, size_t& out_relocated_bytes) {
    (void)room_id;
    for (int reg_i = 0; reg_i < SACFOR_REGION_COUNT; ++reg_i) {
        if (reg_i == SF_REGION_TANK || reg_i == SF_REGION_HOTFIELD) continue;

        auto& region = room.regions[reg_i];
        sf_ticket_guard_t rg(region.get_lock());
        sfLiveHeader* cur = region.get_head();

        while (cur) {
            sfLiveHeader* next_obj = cur->next;

            const auto* cur_cold = get_cold_ro(cur);
            bool is_paged = (cur_cold && cur_cold->is_paged_out.load(std::memory_order_acquire) != 0);
            if (!cur->is_pinned() &&
                !is_paged &&
                cur->parent_header.load(std::memory_order_relaxed) == nullptr &&
                cur->forwarding_address.load(std::memory_order_relaxed) == nullptr) {
                size_t fp = (size_t)cur->memory_footprint.load(std::memory_order_relaxed);
                if (fp > 0 && fp < max_compact_fp) {
                    if (!cur->try_pin()) {
                        cur = next_obj;
                        continue;
                    }
                    cur->unpin();

                    size_t cell_sz = SF_custominiVm_live_header_t::total_cell_size(fp);
                    auto* eb = _falline->snatch_emptybox(cell_sz, sweep_no);
                    void* new_raw = eb ? static_cast<void*>(eb) : _runway->bump_alloc(cell_sz);

                    if (new_raw) {
                        auto* new_obj = new (new_raw) SF_custominiVm_live_header_t();
                        new_obj->relocate_or_clone_from(cur, /*transfer_cold_ownership=*/true);

                        if (use_simd) {
                            sf_critical_clear_memory_core_adaptive(new_obj->body(), fp, sf_is_efficiency_core_active());
                        }
                        memcpy(new_obj->body(), cur->body(), fp);

                        new_obj->forwarding_address.store(nullptr, std::memory_order_relaxed);
                        new_obj->writeTailCanary();

                        sf_sync_store_fence();
                        uint32_t old_fid = cur->function_id;
                        cur->forwarding_address.store(new_obj->body(), std::memory_order_release);

                        region.evict_nolock(cur);
                        region.place_nolock(new_obj);

                        cur->~SF_custominiVm_live_header_t();
                        auto* old_eb = new (cur) SF_custominiVm_emptybox_header_t();
                        old_eb->init(old_fid, fp, cell_sz, new_obj->body(), sweep_no);
                        _falline->register_emptybox(cell_sz, old_eb, sweep_no);

                        ++out_relocated_count;
                        out_relocated_bytes += fp;
                    }
                }
            }
            cur = next_obj;
        }
    }
}

MSIMD_API void RedletGC::compact_and_relocate_motherline_adaptive(uint64_t sweep_no, bool use_simd) {
    set_gc_phase(SF_GC_PHASE_COMPACT, "phase_compact");
    wait_for_safepoint_rendezvous();
    trigger_hook(SF_GC_HOOK_PRE_COMPACT);

    bool is_e = sf_is_efficiency_core_active();
    auto* nexus = sf_get_hardware_nexus();
    bool has_vcache = nexus && (nexus->sram.has_3d_vcache || nexus->sram.has_intel_bllc);
    size_t max_compact_fp = is_e ? (16 * 1024) : (has_vcache ? (256 * 1024) : (64 * 1024));

    size_t count = 0, bytes = 0;
    for (int room_i = 0; room_i < 2; ++room_i) {
        compact_single_room(_motherline->room(room_i), room_i, sweep_no, max_compact_fp, use_simd, count, bytes);
    }

    trigger_hook(SF_GC_HOOK_POST_COMPACT);
    set_gc_phase(SF_GC_PHASE_IDLE, "phase_idle");
}

MSIMD_API void RedletGC::compact_and_relocate_motherline_cycles(uint64_t sweep_no) {
    compact_and_relocate_motherline_adaptive(sweep_no, false);
}

MSIMD_API void RedletGC::compact_and_relocate_motherline_simd(uint64_t sweep_no) {
    compact_and_relocate_motherline_adaptive(sweep_no, true);
}

MSIMD_API void RedletGC::compact_and_relocate_motherline(uint64_t sweep_no) {
    compact_and_relocate_motherline_simd(sweep_no);
}

MSIMD_API bool RedletGC::should_trigger_line_compaction(uint64_t current_sweep, uint64_t fallback_interval) {
    uint64_t now_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last_ns = last_line_compaction_ts_ns.load(std::memory_order_relaxed);
    if (line_compaction_freq_ns > 0) {
        if (last_ns == 0) {
            last_line_compaction_ts_ns.store(now_ns, std::memory_order_relaxed);
            return false;
        }
        if ((now_ns - last_ns) >= line_compaction_freq_ns) {
            last_line_compaction_ts_ns.store(now_ns, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    if (current_sweep > 0 && fallback_interval > 0 && (current_sweep % fallback_interval == 0)) {
        last_line_compaction_ts_ns.store(now_ns, std::memory_order_relaxed);
        return true;
    }
    return false;
}

/* ========================================================================= */
/* 9. TELEMETRY & SNAPSHOTS                                                  */
/* ========================================================================= */

MSIMD_API sf_env_snapshot_t RedletGC::capture_env(void) const {
    sf_env_snapshot_t s{};
    s.runway_bump = _runway->used_bytes();
    s.allocated_bytes = total_heap_bytes.load(std::memory_order_relaxed);
    s.pacing_stalls = _runway->get_pacing_stalls();
    s.total_calls = total_calls_tracked.load(std::memory_order_relaxed);
    s.resurrections = resurrection_count.load(std::memory_order_relaxed);
    s.paged_out_count = pagefile.paged_out_count.load(std::memory_order_relaxed);
    s.total_heap = total_heap_bytes.load(std::memory_order_relaxed);
    auto* coord = RedletGearkit::point_coordinator();
    if (coord) {
        s.cp_bytes_validated = coord->controlpoint_bytes_validated.load(std::memory_order_relaxed);
        s.cp_pacing_triggers = coord->controlpoint_pacing_triggers.load(std::memory_order_relaxed);
        s.hp_canary_faults = coord->hookpoint_canary_faults_caught.load(std::memory_order_relaxed);
        s.sp_rendezvous_stalls = coord->safepoint_rendezvous_stalls.load(std::memory_order_relaxed);
    }
    return s;
}

MSIMD_API bool RedletGC::has_env_changed(const sf_env_snapshot_t& prev, const sf_env_snapshot_t& cur) const {
    return (cur.runway_bump != prev.runway_bump) ||
           (cur.allocated_bytes != prev.allocated_bytes) ||
           (cur.pacing_stalls != prev.pacing_stalls) ||
           (cur.total_calls != prev.total_calls) ||
           (cur.resurrections != prev.resurrections) ||
           (cur.paged_out_count != prev.paged_out_count) ||
           (cur.cp_bytes_validated != prev.cp_bytes_validated) ||
           (cur.cp_pacing_triggers != prev.cp_pacing_triggers) ||
           (cur.hp_canary_faults != prev.hp_canary_faults);
}

MSIMD_API sf_mltc_report_t RedletGC::evaluate_mltc_diagnostics(size_t cur_b, size_t last_b, uint64_t diff_time,
                                                             size_t& peak_b, uint32_t& steady_count, uint64_t sweep_no) {
    sf_mltc_report_t rep;
    rep.heap_bytes = cur_b;
    double diff_sec = diff_time > 0 ? ((double)diff_time / 1e9) : 0.001;
    size_t diff_bytes = (cur_b > last_b) ? (cur_b - last_b) : 0;
    rep.velocity_mbps = ((double)diff_bytes / diff_sec) / (1024.0 * 1024.0);

    if (cur_b > peak_b) {
        peak_b = cur_b;
        steady_count++;
    } else if (cur_b < peak_b) {
        peak_b = cur_b;
        steady_count = 0;
    }
    rep.steady_increase_count = steady_count;

    if (steady_count > 300 && cur_b > 10 * 1024 * 1024) {
        rep.leak_detected = true;
    }
    if (rep.velocity_mbps > 150.0) {
        rep.traffic_congestion_detected = true;
    }

    if (sweep_no % 10 == 0) {
        for (int r = 0; r < 2; ++r) {
            for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
                auto& region = _motherline->room(r).regions[reg];
                sf_ticket_guard_t g(region.get_lock());
                auto* cur = region.get_head();
                size_t sampled = 0;
                while (cur && sampled++ < 16) {
                    rep.total_tensors_scanned++;
                    uintptr_t addr = reinterpret_cast<uintptr_t>(cur->body());
                    if ((addr & (SACFOR_CACHE_LINE - 1)) != 0) rep.sram_misaligned_count++;
                    if ((addr & 255) != 0) rep.dram_misaligned_count++;
                    cur = cur->next;
                }
            }
        }
        if (rep.total_tensors_scanned > 0 &&
            (rep.sram_misaligned_count > 0 || (rep.dram_misaligned_count * 100 / rep.total_tensors_scanned) > 25)) {
            rep.alignment_hazard_detected = true;
        }
    }
    return rep;
}

/* ========================================================================= */
/* 10. MAIN COLLECTION LOOP & LIFECYCLE                                      */
/* ========================================================================= */

MSIMD_API void RedletGC::start(void) {
    if (running.exchange(true, std::memory_order_acq_rel)) return;
    if (gc_thread.joinable()) gc_thread.join();

    gc_thread = std::thread([this]() {
        // Pin background GC thread to E-cores on hybrid architecture
        sf_bind_current_thread(SF_CORE_TIER_EFFICIENCY);

        while (running.load(std::memory_order_acquire)) {
            if (sf_is_focus_scp()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

MSIMD_API void RedletGC::stop(void) {
    running.store(false, std::memory_order_release);
    if (gc_thread.joinable()) gc_thread.join();
    drain_falline();
    _landfill->evacuate(nullptr, sweep_count.load(std::memory_order_relaxed));
    pagefile.destroy();
}

MSIMD_API sf_step_result_t RedletGC::step(void) {
    sf_step_result_t res{};
    res.swept = sweep_motherline();
    res.drained = drain_falline();

    uint64_t current_sweep = sweep_count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (current_sweep % SF_REHAB_SCAN_INTERVAL == 0 ||
        resurrect_read.load(std::memory_order_relaxed) != resurrect_write.load(std::memory_order_acquire)) {
        res.resurrected = scan_rehab_arena();
    }

    maybe_compact_falline();
    if (should_trigger_line_compaction(current_sweep, 50)) {
        compact_and_relocate_motherline(current_sweep);
    }

    set_gc_phase(SF_GC_PHASE_IDLE, "phase_idle");
    return res;
}

MSIMD_API SF_custominiVm_gc_manifest_t RedletGC::shake_bed(void) {
    uint64_t t0 = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    SF_custominiVm_gc_manifest_t m{};
    m.sweep_id = sweep_count.load(std::memory_order_relaxed) + 1;
    m.rooms_inspected = 3;

    size_t heap_before = total_heap_bytes.load(std::memory_order_relaxed);

    set_gc_phase(SF_GC_PHASE_SWEEP, "phase_sweep");
    sf_step_result_t r1 = step();
    m.cells_swept += r1.swept;
    m.cells_drained += r1.drained;

    maybe_compact_falline();
    sf_step_result_t r2 = step();
    m.cells_swept += r2.swept;
    m.cells_drained += r2.drained;
    m.trailing_debris_cleared = 1;

    size_t heap_after = total_heap_bytes.load(std::memory_order_relaxed);
    m.bytes_reclaimed = (heap_before > heap_after) ? (heap_before - heap_after) : 0;
    m.heap_bytes_after = heap_after;
    m.elapsed_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - t0;
    m.quiescent = 1;

    snprintf(m.narrative, sizeof(m.narrative),
        "RedletGC ShakeBed: Swept %zu cells -> Reclaimed %.2f MB -> Debris %s -> Quiescent",
        m.cells_swept, (double)m.bytes_reclaimed / (1024.0 * 1024.0),
        m.trailing_debris_cleared ? "cleared" : "clean");

    set_gc_phase(SF_GC_PHASE_IDLE, "phase_idle");

    std::lock_guard<std::mutex> lk(manifest_lock);
    last_manifest = m;
    return m;
}

MSIMD_API size_t RedletGC::run(SF_custominiVm_gc_run_mode_t mode,
                              bool owp,
                              SF_custominiVm_mtp_predicate_fn mtp_pred,
                              void* user_data) {
    if (mode == SF_GC_RUN_SINGLE_STEP) {
        if (owp) {
            sf_cpu_pause();
            std::this_thread::yield();
        }
        auto m = shake_bed();
        return m.bytes_reclaimed > 0 ? m.bytes_reclaimed : (m.cells_swept > 0 ? m.cells_swept : 1);
    }

    sf_env_snapshot_t prev_env = capture_env();

    if (mode == SF_GC_RUN_CONTINUE_FINISH) {
        size_t steps = 0;
        size_t consecutive_quiescent = 0;
        const size_t MAX_STEPS = 1024;

        while (steps < MAX_STEPS) {
            sf_step_result_t r = step();
            steps++;

            if (owp) {
                sf_cpu_pause();
                std::this_thread::yield();
                sf_env_snapshot_t cur_env = capture_env();
                prev_env = cur_env;
            }

            if (r.swept == 0 && r.drained == 0 && _falline->emptybox_count() == 0 &&
                resurrect_read.load(std::memory_order_relaxed) == resurrect_write.load(std::memory_order_relaxed)) {
                consecutive_quiescent++;
                if (consecutive_quiescent >= 2) break;
            } else {
                consecutive_quiescent = 0;
            }
        }
        return steps;
    }

    if (mode == SF_GC_RUN_STEPPING_MTP) {
        size_t steps = 0;
        const size_t MAX_MTP_STEPS = 50000;

        while (steps < MAX_MTP_STEPS) {
            step();
            steps++;

            sf_env_snapshot_t cur_env = capture_env();
            SF_custominiVm_env_change_t env_delta{};
            env_delta.changed = has_env_changed(prev_env, cur_env) ? 1 : 0;
            env_delta.allocations_detected = (uint32_t)(cur_env.runway_bump > prev_env.runway_bump ? (cur_env.runway_bump - prev_env.runway_bump) : 0);
            env_delta.pacing_stalls_delta = (uint32_t)(cur_env.pacing_stalls > prev_env.pacing_stalls ? (cur_env.pacing_stalls - prev_env.pacing_stalls) : 0);
            env_delta.calls_delta = (uint32_t)(cur_env.total_calls > prev_env.total_calls ? (cur_env.total_calls - prev_env.total_calls) : 0);
            env_delta.resurrections_delta = (uint32_t)(cur_env.resurrections > prev_env.resurrections ? (cur_env.resurrections - prev_env.resurrections) : 0);
            env_delta.paged_out_delta = (uint32_t)(cur_env.paged_out_count > prev_env.paged_out_count ? (cur_env.paged_out_count - prev_env.paged_out_count) : 0);
            prev_env = cur_env;

            if (mtp_pred) {
                SF_custominiVm_anatomy_stat_t stat{};
                SF_custominiVm_anatomy_query(&stat);
                if (mtp_pred(&stat, &env_delta, user_data)) {
                    break;
                }
            } else {
                size_t cur_b = total_heap_bytes.load(std::memory_order_relaxed);
                float ratio = pressure_threshold.load(std::memory_order_relaxed) > 0 ?
                    ((float)cur_b / (float)pressure_threshold.load(std::memory_order_relaxed)) : 0.0f;
                if (ratio < 0.20f) break;
                if (owp && env_delta.allocations_detected > 0) break;
            }

            if (owp) {
                sf_cpu_pause();
                std::this_thread::yield();
            }
        }
        return steps;
    }
    return 0;
}

/* ========================================================================= */
/* 11. TELEMETRY STREAM DAEMON & VM INSTANCE STATE                           */
/* ========================================================================= */

struct sf_telemetry_stream_daemon_t {
    std::atomic<bool>         running{ false };
    std::thread               worker;
    uint32_t                  interval_us{ 10000 };
    sf_telemetry_stream_fn_t  callback{ nullptr };
    void*                     user_data{ nullptr };
    std::mutex                stream_mutex;

    void start(uint32_t flush_interval_us, sf_telemetry_stream_fn_t cb, void* user) {
        std::lock_guard<std::mutex> lk(stream_mutex);
        if (running.load(std::memory_order_relaxed)) return;
        interval_us = flush_interval_us > 0 ? flush_interval_us : 10000;
        callback = cb;
        user_data = user;
        running.store(true, std::memory_order_release);
        worker = std::thread([this]() {
            struct drain_stream_ctx_t {
                sf_telemetry_stream_fn_t cb;
                void* u;
                char buf[512];
            } ctx{ callback, user_data, {0} };

            while (running.load(std::memory_order_relaxed)) {
                if (ctx.cb) {
                    SF_custominiVm_hotlog_drain([](const SF_custominiVm_hotlog_entry_t* e, void* u) {
                        auto* c = static_cast<drain_stream_ctx_t*>(u);
                        c->buf[0] = '\0';
                        snprintf(c->buf, sizeof(c->buf),
                            "[%llu] [EVENT %u] core=%u gen=%u reg=%u sweep=%llu heap=%uKB msg=\"%s\"",
                            (unsigned long long)e->ts_ns, (unsigned int)e->event,
                            (unsigned int)e->current_core, (unsigned int)e->generation, (unsigned int)e->region,
                            (unsigned long long)e->gc_sweep_no, (unsigned int)e->heap_used_kb, e->msg);
                        c->cb(e, c->buf, c->u);
                    }, &ctx);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
            }
        });
    }

    void stop() {
        std::lock_guard<std::mutex> lk(stream_mutex);
        if (!running.load(std::memory_order_relaxed)) return;
        running.store(false, std::memory_order_release);
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool is_running() const {
        return running.load(std::memory_order_relaxed);
    }

    ~sf_telemetry_stream_daemon_t() {
        stop();
    }
};

static sf_telemetry_stream_daemon_t g_sf_telemetry_daemon;

struct sf_vm_instance {
    RedletGC* gc_ptr{ nullptr };
};
static sf_vm_instance g_sf_default_vm_instance{ &g_default_redlet_gc };
static std::atomic<bool> g_sf_rmrp_enabled{ false };
static std::atomic<uint8_t> g_sf_rmrp_method{ static_cast<uint8_t>(sf_rmrp_method_e::PLUMP) };

static char g_anatomy_desc_buf[4096] = { 0 };

/* ========================================================================= */
/* 12. CANONICAL C APIS                                                      */
/* ========================================================================= */

extern "C" {

MSIMD_API RedletGC* sf_get_default_gc(void) {
    return &g_default_redlet_gc;
}

MSIMD_API void SF_custominiVm_register_gc_hook(SF_custominiVm_gc_hook_fn_t hook, void* user_data) {
    g_default_redlet_gc.gc_hook = hook;
    g_default_redlet_gc.gc_hook_user = user_data;
}

MSIMD_API void SF_custominiVm_host_gc_notify(uint32_t host_event_id) {
    (void)host_event_id;
}

MSIMD_API size_t SF_custominiVm_host_loop_tick(uint32_t budget_us) {
    (void)budget_us;
    auto res = g_default_redlet_gc.step();
    return res.swept + res.drained;
}

MSIMD_API void SF_custominiVm_gc_shake_bed(SF_custominiVm_gc_manifest_t* out_manifest) {
    auto m = g_default_redlet_gc.shake_bed();
    if (out_manifest) *out_manifest = m;
}

MSIMD_API void SF_custominiVm_gc_step(sf_step_result_t* out_result) {
    auto r = g_default_redlet_gc.step();
    if (out_result) *out_result = r;
}

MSIMD_API size_t SF_custominiVm_gc_run(SF_custominiVm_gc_run_mode_t mode, int owp) {
    return g_default_redlet_gc.run(mode, owp != 0);
}

MSIMD_API uint64_t SF_custominiVm_gc_sweep_count(void) {
    return g_default_redlet_gc.sweep_count.load(std::memory_order_relaxed);
}

MSIMD_API size_t SF_custominiVm_gc_total_heap_bytes(void) {
    return g_default_redlet_gc.total_heap_bytes.load(std::memory_order_relaxed);
}

MSIMD_API void SF_custominiVm_set_rmrp(int enable, int method) {
    g_sf_rmrp_enabled.store(enable != 0, std::memory_order_release);
    g_sf_rmrp_method.store(static_cast<uint8_t>(method != 0 ? sf_rmrp_method_e::SEQUENTIAL : sf_rmrp_method_e::PLUMP), std::memory_order_release);
}

MSIMD_API int SF_custominiVm_get_rmrp(void) {
    return g_sf_rmrp_enabled.load(std::memory_order_acquire) ? 1 : 0;
}

MSIMD_API size_t SF_custominiVm_run_rmrp(void) {
    auto method = static_cast<sf_rmrp_method_e>(g_sf_rmrp_method.load(std::memory_order_relaxed));
    return g_default_redlet_gc.execute_rmrp(sf_snatch_rebate_mode_e::AUTO, method);
}

MSIMD_API void SF_custominiVm_resurrect_request(uint32_t fn_id) {
    g_default_redlet_gc.request_resurrection(fn_id);
}

MSIMD_API void SF_custominiVm_anatomy_query(SF_custominiVm_anatomy_stat_t* out_stat) {
    if (!out_stat) return;
    memset(out_stat, 0, sizeof(*out_stat));

    auto* gc = sf_get_default_gc();
    if (!gc) return;
    auto* rw = gc->runway();
    auto* ml = gc->motherline();
    auto* fl = gc->falline();
    auto* lf = gc->landfill();

    if (rw && rw->allocator.superblock) {
        auto* sb = rw->allocator.superblock;
        out_stat->onheap_reserved_bytes = sb->total_reserved_bytes;
        out_stat->onheap_committed_bytes = sb->total_committed_bytes;
        out_stat->onheap_allocated_bytes = sb->allocated_bytes.load(std::memory_order_relaxed);
        out_stat->rebated_bytes = sb->rebated_bytes.load(std::memory_order_relaxed);
    }

    /* 1.1 Runway Nursery Domain & Split Zones */
    if (rw) {
        out_stat->runway_total_bytes = rw->allocator.runway_capacity;
        out_stat->runway_used_bytes = rw->allocator.runway_bump_offset.load(std::memory_order_relaxed);
        out_stat->motherline_live_bytes = gc->total_heap_bytes.load(std::memory_order_relaxed);
        out_stat->egg_count = rw->motherline.egg_count();
        out_stat->fledgling_count = rw->motherline.fledgling_count();
        out_stat->pitstop_count = rw->pitstop.count();
        out_stat->pitstop_total_morphed = rw->pitstop.total_morphed();
    }
    if (fl) {
        out_stat->emptybox_count = fl->emptybox_count();
        out_stat->falline_emptybox_bytes = out_stat->emptybox_count * 128;
    }

    /* 1.2 On-Heap Generational Rooms */
    if (ml) {
        out_stat->room_young_bytes = ml->room(SF_ROOM_YOUNG).total_bytes.load(std::memory_order_relaxed);
        out_stat->room_young_objects = ml->room(SF_ROOM_YOUNG).total_objects.load(std::memory_order_relaxed);
        out_stat->room_adolescent_bytes = ml->room(SF_ROOM_ADOLESCENT).total_bytes.load(std::memory_order_relaxed);
        out_stat->room_adolescent_objects = ml->room(SF_ROOM_ADOLESCENT).total_objects.load(std::memory_order_relaxed);
        out_stat->room_senior_bytes = ml->room(SF_ROOM_SENIOR).total_bytes.load(std::memory_order_relaxed);
        out_stat->room_senior_objects = ml->room(SF_ROOM_SENIOR).total_objects.load(std::memory_order_relaxed);

        /* 1.2 Topological Regions aggregated across all rooms */
        size_t dem_bytes = 0, dem_count = 0;
        size_t mid_bytes = 0, mid_count = 0;
        size_t tank_bytes = 0, tank_count = 0;
        size_t hot_bytes = 0, hot_count = 0;

        for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
            auto& rm = ml->room(r);
            dem_bytes += rm.regions[SF_REGION_DEMAND].total_bytes.load(std::memory_order_relaxed);
            dem_count += rm.regions[SF_REGION_DEMAND].count.load(std::memory_order_relaxed);

            mid_bytes += rm.regions[SF_REGION_MIDDLE].total_bytes.load(std::memory_order_relaxed);
            mid_count += rm.regions[SF_REGION_MIDDLE].count.load(std::memory_order_relaxed);

            tank_bytes += rm.regions[SF_REGION_TANK].total_bytes.load(std::memory_order_relaxed);
            tank_count += rm.regions[SF_REGION_TANK].count.load(std::memory_order_relaxed);

            hot_bytes += rm.regions[SF_REGION_HOTFIELD].total_bytes.load(std::memory_order_relaxed);
            hot_count += rm.regions[SF_REGION_HOTFIELD].count.load(std::memory_order_relaxed);
        }

        out_stat->region_demand_bytes = dem_bytes;
        out_stat->region_demand_objects = dem_count;
        out_stat->region_middle_bytes = mid_bytes;
        out_stat->region_middle_objects = mid_count;
        out_stat->tank_regions_bytes = tank_bytes;
        out_stat->tank_regions_count = tank_count;
        out_stat->region_hotfield_bytes = hot_bytes;
        out_stat->region_hotfield_objects = hot_count;
    }

    /* 1.3 Independent Puzzle Arena */
    auto* puzzle = sf_get_default_puzzle_arena();
    if (puzzle) {
        out_stat->puzzle_arena_total_bytes = puzzle->heap_capacity();
        out_stat->puzzle_arena_used_bytes = puzzle->heap_used();
        out_stat->onheap_reserved_bytes += out_stat->puzzle_arena_total_bytes;
        out_stat->onheap_committed_bytes += out_stat->puzzle_arena_total_bytes;
        out_stat->onheap_allocated_bytes += out_stat->puzzle_arena_used_bytes;
    }

    /* 1.4 Landfill Domain */
    if (lf) {
        size_t mat_entries = 0;
        size_t rhb_entries = 0;
        uint32_t pane_cnt = lf->pane().count.load(std::memory_order_relaxed);
        if (lf->pane().entries) {
            for (uint32_t pi = 0; pi < pane_cnt; ++pi) {
                if (lf->pane().entries[pi].rehab_payload_ptr != nullptr ||
                    lf->pane().entries[pi].lasting == 1) {
                    ++rhb_entries;
                } else {
                    ++mat_entries;
                }
            }
        }

        out_stat->landfill_total_bytes = lf->pit().matured_arena.capacity.load(std::memory_order_relaxed) +
            lf->pit().rehab_arena.capacity.load(std::memory_order_relaxed);
        out_stat->landfill_used_bytes = lf->pit().total_used();
        out_stat->matured_arena_bytes = lf->pit().matured_arena.used.load(std::memory_order_relaxed);
        out_stat->matured_entry_count = mat_entries;
        out_stat->rehab_arena_bytes = lf->pit().rehab_arena.used.load(std::memory_order_relaxed);
        out_stat->rehab_entry_count = rhb_entries;
        out_stat->landfill_pane_count = pane_cnt;
        out_stat->matured_flush_count = gc->matured_flush_count.load(std::memory_order_relaxed);
        out_stat->rehab_scan_count = gc->rehab_scan_count.load(std::memory_order_relaxed);
        out_stat->resurrection_count = gc->resurrection_count.load(std::memory_order_relaxed);

        out_stat->onheap_reserved_bytes += out_stat->landfill_total_bytes;
        out_stat->onheap_committed_bytes += out_stat->landfill_total_bytes;
        out_stat->onheap_allocated_bytes += out_stat->landfill_used_bytes;
    }

    /* 2. Off-Heap Domain */
    out_stat->offheap_code_arena_bytes = 64ULL * 1024 * 1024;
    out_stat->offheap_telemetry_bytes = (sizeof(SF_custominiVm_hotlog_entry_t) * SACFOR_HOTLOG_SIZE) +
        (sizeof(SF_custominiVm_coldlog_entry_t) * SACFOR_COLDLOG_SIZE);

    /* 3. Slab Expansion & Paging */
    if (rw) {
        out_stat->chained_chunk_count = rw->allocator.chained_chunk_count.load(std::memory_order_relaxed);
        out_stat->pacing_stall_count = rw->get_pacing_stalls();
    }
    out_stat->paged_out_bytes = gc->pagefile.paged_out_bytes.load(std::memory_order_relaxed);
    out_stat->paged_out_count = gc->pagefile.paged_out_count.load(std::memory_order_relaxed);
}

MSIMD_API const char* SF_custominiVm_anatomy_description(void) {
    SF_custominiVm_anatomy_stat_t s{};
    SF_custominiVm_anatomy_query(&s);
    snprintf(g_anatomy_desc_buf, sizeof(g_anatomy_desc_buf),
        "SFVM Unified Anatomy [Shenandoah-Model Elastic Segments & Complete On-Heap Topography]:\n"
        "  - On-Heap (Total Reserved: %zu MB, Committed: %zu MB, Allocated: %zu MB):\n"
        "    + 1. Runway Segment Total : %zu MB (Bump Offset Used: %zu KB)\n"
        "      * Motherline Live       : %zu KB [Egg (Unoptimized): %zu, Fledgling (Optimized): %zu]\n"
        "      * Pitstop Area          : %zu Active In-Flight Excisions (Lifetime Morphs: %zu)\n"
        "      * Falline Free-Chain    : %zu Emptyboxes [~%zu KB Retained for Snatch]\n"
        "    + 2. Generational Rooms   : Young: %zu KB (%zu objs) | Adolescent: %zu KB (%zu objs) | Senior: %zu KB (%zu objs)\n"
        "    + 3. Topological Regions  :\n"
        "      * Demand Region         : %zu KB (%zu transient buffers)\n"
        "      * Middle Region         : %zu KB (%zu operational tensors)\n"
        "      * Tank Regions          : %zu KB across rooms (%zu humongous SIMD matrices)\n"
        "      * Hotfield Region       : %zu KB (%zu cacheline-sorted hot tensors)\n"
        "    + 4. Puzzle Arena         : %zu MB (3rd On-Heap Sandbox Test Area, Used: %zu KB)\n"
        "    + 5. Landfill Area Total  : %zu MB (Allocated Used: %zu KB, Header Panes: %u slots)\n"
        "      * Matured Arena         : %zu KB (%zu entries, Flushes: %llu)\n"
        "      * Rehabilitation Arena  : %zu KB (%zu candidate entries, Scans: %llu, Resurrected: %llu)\n"
        "  - Off-Heap (GC-Isolated Continuous Exec & Telemetry):\n"
        "    + Code Arena              : %zu MB (JIT Exec Pages)\n"
        "    + Telemetry Rings         : %zu MB (HotLog 64MB + ColdLog 786MB)\n"
        "  - Paging & Backpressure     : %zu Pacing Stalls, %zu KB Swapped to Pagefile (%zu blocks)\n"
        "  - Dynamic Chunks & Rebating : %zu Chained OS Chunks (Rebated / Recycled to OS: %zu KB)",
        s.onheap_reserved_bytes / (1024 * 1024), s.onheap_committed_bytes / (1024 * 1024), s.onheap_allocated_bytes / (1024 * 1024),
        s.runway_total_bytes / (1024 * 1024), s.runway_used_bytes / 1024,
        s.motherline_live_bytes / 1024, s.egg_count, s.fledgling_count,
        s.pitstop_count, s.pitstop_total_morphed,
        s.emptybox_count, s.falline_emptybox_bytes / 1024,
        s.room_young_bytes / 1024, s.room_young_objects,
        s.room_adolescent_bytes / 1024, s.room_adolescent_objects,
        s.room_senior_bytes / 1024, s.room_senior_objects,
        s.region_demand_bytes / 1024, s.region_demand_objects,
        s.region_middle_bytes / 1024, s.region_middle_objects,
        s.tank_regions_bytes / 1024, s.tank_regions_count,
        s.region_hotfield_bytes / 1024, s.region_hotfield_objects,
        s.puzzle_arena_total_bytes / (1024 * 1024), s.puzzle_arena_used_bytes / 1024,
        s.landfill_total_bytes / (1024 * 1024), s.landfill_used_bytes / 1024, s.landfill_pane_count,
        s.matured_arena_bytes / 1024, s.matured_entry_count, (unsigned long long)s.matured_flush_count,
        s.rehab_arena_bytes / 1024, s.rehab_entry_count, (unsigned long long)s.rehab_scan_count, (unsigned long long)s.resurrection_count,
        s.offheap_code_arena_bytes / (1024 * 1024),
        s.offheap_telemetry_bytes / (1024 * 1024),
        s.pacing_stall_count, s.paged_out_bytes / 1024, s.paged_out_count,
        s.chained_chunk_count, s.rebated_bytes / 1024);
    return g_anatomy_desc_buf;
}

MSIMD_API int SF_custominiVm_telemetry_stream_start(uint32_t flush_interval_us,
                                                   sf_telemetry_stream_fn_t callback,
                                                   void* user_data) {
    if (!callback) return -1;
    g_sf_telemetry_daemon.start(flush_interval_us, callback, user_data);
    return 0;
}

MSIMD_API void SF_custominiVm_telemetry_stream_stop(void) {
    g_sf_telemetry_daemon.stop();
}

MSIMD_API int SF_custominiVm_telemetry_stream_is_running(void) {
    return g_sf_telemetry_daemon.is_running() ? 1 : 0;
}

MSIMD_API sf_vm_handle SF_custominiVm_get_default_instance(void) {
    return &g_sf_default_vm_instance;
}

MSIMD_API int SF_custominiVm_instance_is_booted(sf_vm_handle vm) {
    return (vm && vm->gc_ptr) ? 1 : 0;
}

MSIMD_API void SF_custominiVm_instance_shutdown(sf_vm_handle vm) {
    if (vm && vm->gc_ptr) {
        vm->gc_ptr->stop();
    }
}

MSIMD_API int SF_custominiVm_instance_reset(sf_vm_handle vm) {
    if (vm && vm->gc_ptr) {
        vm->gc_ptr->stop();
        vm->gc_ptr->init();
        return 0;
    }
    return -1;
}

MSIMD_API void SF_custominiVm_gc_admit(sfLiveHeader* obj) {
    g_default_redlet_gc.admit(obj);
}

MSIMD_API void SF_custominiVm_gc_touch(sfLiveHeader* obj, uint64_t cycles, uint16_t core_id) {
    if (obj) {
        obj->record_access(cycles, core_id);
    }
}

/* ========================================================================= */
/* SENSOR STREAMING IMPLEMENTATION                                           */
/* ========================================================================= */

struct alignas(64) sf_sensor_attached_entry_t {
    SF_custominiVm_tensor_t        stream_tensor{};
    SF_custominiVm_sensor_config_t config{};
    std::atomic<size_t>            write_cursor{ 0 };
    std::atomic<bool>              active{ false };
};

static std::mutex                 g_sf_sensor_lock;
alignas(64) static sf_sensor_attached_entry_t g_sf_sensor_slots[8];

MSIMD_API int SF_custominiVm_sensor_attach_stream(const SF_custominiVm_sensor_config_t* cfg, SF_custominiVm_tensor_t* out_stream_tensor) {
    if (!cfg || !out_stream_tensor) return -1;
    std::lock_guard<std::mutex> g(g_sf_sensor_lock);

    int free_slot = -1;
    for (int i = 0; i < 8; ++i) {
        if (!g_sf_sensor_slots[i].active.load(std::memory_order_relaxed)) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) return -2;

    size_t samples = cfg->buffer_sample_count > 0 ? cfg->buffer_sample_count : 4096;
    int32_t shape[1] = { static_cast<int32_t>(samples) };

    SF_custominiVm_tensor_t t = SF_custominiVm_tensor_alloc(shape, 1, SF_DTYPE_F32);
    if (!t.data) return -3;

    if (cfg->gc_exempt_pinning) {
        SF_custominiVm_tensor_pin(&t);
    }

    auto& slot = g_sf_sensor_slots[free_slot];
    slot.stream_tensor = t;
    slot.config = *cfg;
    slot.write_cursor.store(0, std::memory_order_relaxed);
    slot.active.store(true, std::memory_order_release);

    *out_stream_tensor = t;
    return 0;
}

MSIMD_API size_t SF_custominiVm_sensor_poll_stream(SF_custominiVm_tensor_t* stream_tensor, size_t max_samples) {
    if (!stream_tensor || !stream_tensor->data) return 0;
    std::lock_guard<std::mutex> g(g_sf_sensor_lock);

    for (int i = 0; i < 8; ++i) {
        auto& slot = g_sf_sensor_slots[i];
        if (slot.active.load(std::memory_order_relaxed) && slot.stream_tensor.data == stream_tensor->data) {
            float* buf = static_cast<float*>(stream_tensor->data);
            size_t cap = slot.stream_tensor.shape[0];
            size_t to_sample = (max_samples > 0) ? max_samples : 64;
            if (to_sample > cap) to_sample = cap;

            size_t cur = slot.write_cursor.load(std::memory_order_relaxed);
            for (size_t s = 0; s < to_sample; ++s) {
                float val = 0.0f;
                if (slot.config.sensor_type == SF_SENSOR_TEMP_HUMIDITY) {
                    val = 24.5f + 0.1f * static_cast<float>(rand() % 10);
                } else if (slot.config.sensor_type == SF_SENSOR_WATER_LEVEL) {
                    val = 680.0f + static_cast<float>(rand() % 20);
                } else if (slot.config.sensor_type == SF_SENSOR_LOGIC_ANALYZER_32CH) {
                    val = static_cast<float>(SF_custominiVm_gpio_fast_read(slot.config.pin_primary));
                } else {
                    val = static_cast<float>(rand() % 1024);
                }
                buf[(cur + s) % cap] = val;
            }
            slot.write_cursor.store((cur + to_sample) % cap, std::memory_order_relaxed);
            return to_sample;
        }
    }
    return 0;
}

MSIMD_API void SF_custominiVm_sensor_detach_stream(SF_custominiVm_tensor_t* stream_tensor) {
    if (!stream_tensor || !stream_tensor->data) return;
    std::lock_guard<std::mutex> g(g_sf_sensor_lock);

    for (int i = 0; i < 8; ++i) {
        auto& slot = g_sf_sensor_slots[i];
        if (slot.active.load(std::memory_order_relaxed) && slot.stream_tensor.data == stream_tensor->data) {
            slot.active.store(false, std::memory_order_release);
            if (slot.config.gc_exempt_pinning) {
                SF_custominiVm_tensor_unpin(&slot.stream_tensor);
            }
            SF_custominiVm_tensor_free(&slot.stream_tensor);
            stream_tensor->data = nullptr;
            stream_tensor->byte_size = 0;
            break;
        }
    }
}

} // extern "C"
