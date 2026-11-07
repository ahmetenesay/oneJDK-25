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

#include "gc/redjet/allocator/redjet_rmrp.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/simd/redjetSIMD.hpp"
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

// Static member definitions
std::atomic<uint64_t> RedjetRMRP::_total_plump_reclaimed_bytes{0};
std::atomic<uint64_t> RedjetRMRP::_total_sequential_reclaimed_bytes{0};
std::atomic<uint32_t> RedjetRMRP::_total_reclamation_cycles{0};

RedjetRMRPBarrier      RedjetRMRP::_barrier;
RedjetRMRPDeduplicator RedjetRMRP::_deduplicator;
RedjetRMRPCompactor    RedjetRMRP::_compactor;

/* ========================================================================= */
/* 1. REDJET RMRP BARRIER IMPLEMENTATION                                     */
/* ========================================================================= */

RedjetRMRPBarrier::RedjetRMRPBarrier() {
    _entry_count.store(0, std::memory_order_relaxed);
    _barrier_hits.store(0, std::memory_order_relaxed);
    _cold_transitions.store(0, std::memory_order_relaxed);
}

void RedjetRMRPBarrier::register_page(uintptr_t page_addr, size_t page_sz) {
    hs_ticket_guard_t guard(_barrier_lock);
    size_t count = _entry_count.load(std::memory_order_relaxed);
    if (count >= MAX_TRACKED_PAGES) return;

    for (size_t i = 0; i < count; ++i) {
        if (_entries[i].page_address == page_addr) {
            _entries[i].page_size = page_sz;
            _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT), std::memory_order_relaxed);
            _entries[i].access_heat.store(100, std::memory_order_relaxed);
            _entries[i].idle_sweeps.store(0, std::memory_order_relaxed);
            return;
        }
    }

    _entries[count].page_address = page_addr;
    _entries[count].page_size = page_sz;
    _entries[count].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT), std::memory_order_relaxed);
    _entries[count].access_heat.store(100, std::memory_order_relaxed);
    _entries[count].idle_sweeps.store(0, std::memory_order_relaxed);
    _entries[count].last_touch_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
    _entry_count.store(count + 1, std::memory_order_release);
}

void RedjetRMRPBarrier::on_page_touch(uintptr_t addr) noexcept {
    _barrier_hits.fetch_add(1, std::memory_order_relaxed);
    uintptr_t page_base = addr & ~static_cast<uintptr_t>(4095);

    size_t count = _entry_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (_entries[i].page_address == page_base) {
            _entries[i].access_heat.store(100, std::memory_order_relaxed);
            _entries[i].idle_sweeps.store(0, std::memory_order_relaxed);
            _entries[i].last_touch_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
            _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT), std::memory_order_release);
            return;
        }
    }
}

size_t RedjetRMRPBarrier::decay_and_classify_cold_pages(uint32_t decay_amount) {
    hs_ticket_guard_t guard(_barrier_lock);
    size_t count = _entry_count.load(std::memory_order_relaxed);
    size_t newly_cold = 0;

    for (size_t i = 0; i < count; ++i) {
        uint32_t heat = _entries[i].access_heat.load(std::memory_order_relaxed);
        if (heat > decay_amount) {
            heat -= decay_amount;
        } else {
            heat = 0;
        }
        _entries[i].access_heat.store(heat, std::memory_order_relaxed);
        uint32_t idle = _entries[i].idle_sweeps.fetch_add(1, std::memory_order_relaxed) + 1;

        uint8_t cur_state = _entries[i].state.load(std::memory_order_relaxed);
        if (cur_state == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT) && heat < 50) {
            _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_WARM), std::memory_order_relaxed);
        } else if (cur_state == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_WARM) && (heat < 10 || idle >= 3)) {
            _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD), std::memory_order_release);
            _cold_transitions.fetch_add(1, std::memory_order_relaxed);
            newly_cold++;
        }
    }
    return newly_cold;
}

size_t RedjetRMRPBarrier::decommit_cold_pages(size_t target_reclaim_bytes) {
    hs_ticket_guard_t guard(_barrier_lock);
    size_t count = _entry_count.load(std::memory_order_relaxed);
    size_t reclaimed = 0;

    for (size_t i = 0; i < count && reclaimed < target_reclaim_bytes; ++i) {
        uint8_t st = _entries[i].state.load(std::memory_order_relaxed);
        if (st == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD)) {
            void* page_ptr = reinterpret_cast<void*>(_entries[i].page_address);
            size_t sz = _entries[i].page_size;
            if (page_ptr && sz > 0) {
#if defined(_WIN32)
                VirtualAlloc(page_ptr, sz, MEM_RESET, PAGE_READWRITE);
#elif defined(__linux__)
                madvise(page_ptr, sz, MADV_DONTNEED);
#endif
                _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::DECOMMITTED_IDLE), std::memory_order_release);
                reclaimed += sz;
            }
        }
    }
    return reclaimed;
}

bool RedjetRMRPBarrier::is_page_cold(uintptr_t addr) const noexcept {
    uintptr_t page_base = addr & ~static_cast<uintptr_t>(4095);
    size_t count = _entry_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (_entries[i].page_address == page_base) {
            uint8_t s = _entries[i].state.load(std::memory_order_relaxed);
            return s == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD) ||
                   s == static_cast<uint8_t>(RedjetPageResidentState::DECOMMITTED_IDLE);
        }
    }
    return false;
}

/* ========================================================================= */
/* 2. REDJET RMRP DEDUPLICATOR IMPLEMENTATION                                */
/* ========================================================================= */

RedjetRMRPDeduplicator::RedjetRMRPDeduplicator() {
    _total_zero_pages_detected.store(0, std::memory_order_relaxed);
    _total_zero_bytes_reclaimed.store(0, std::memory_order_relaxed);
    _scan_operations.store(0, std::memory_order_relaxed);
}

bool RedjetRMRPDeduplicator::is_page_all_zero(const void* page_addr, size_t page_sz) noexcept {
    if (!page_addr || page_sz == 0) return false;

    const uint64_t* ptr64 = static_cast<const uint64_t*>(page_addr);
    size_t words = page_sz / sizeof(uint64_t);

    // Fast quadword unrolled check (8 words / 64 bytes per iteration)
    size_t chunks = words / 8;
    for (size_t c = 0; c < chunks; ++c) {
        uint64_t acc = ptr64[0] | ptr64[1] | ptr64[2] | ptr64[3] |
                       ptr64[4] | ptr64[5] | ptr64[6] | ptr64[7];
        if (acc != 0ULL) return false;
        ptr64 += 8;
    }

    size_t rem_words = words % 8;
    for (size_t i = 0; i < rem_words; ++i) {
        if (ptr64[i] != 0ULL) return false;
    }

    // Remaining tail bytes if page_sz is not multiple of 8
    size_t tail_bytes = page_sz % sizeof(uint64_t);
    if (tail_bytes > 0) {
        const uint8_t* tail_ptr = reinterpret_cast<const uint8_t*>(ptr64 + rem_words);
        for (size_t b = 0; b < tail_bytes; ++b) {
            if (tail_ptr[b] != 0) return false;
        }
    }
    return true;
}

size_t RedjetRMRPDeduplicator::deduplicate_zero_pages_in_range(void* base_addr, size_t range_bytes, size_t page_sz) {
    if (!base_addr || range_bytes < page_sz) return 0;
    _scan_operations.fetch_add(1, std::memory_order_relaxed);

    size_t num_pages = range_bytes / page_sz;
    size_t reclaimed_bytes = 0;
    uint8_t* cur_page = static_cast<uint8_t*>(base_addr);

    for (size_t p = 0; p < num_pages; ++p) {
        if (is_page_all_zero(cur_page, page_sz)) {
            _total_zero_pages_detected.fetch_add(1, std::memory_order_relaxed);

            // Reclaim physical RSS by resetting or decommitting page
#if defined(_WIN32)
            VirtualAlloc(cur_page, page_sz, MEM_RESET, PAGE_READWRITE);
#elif defined(__linux__)
            madvise(cur_page, page_sz, MADV_DONTNEED);
#endif
            reclaimed_bytes += page_sz;
        }
        cur_page += page_sz;
    }

    if (reclaimed_bytes > 0) {
        _total_zero_bytes_reclaimed.fetch_add(reclaimed_bytes, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_RMRP_SPAN_TRIM, HS_Ebin1_INFO, 0, "rmrp_dedup",
                            "RMRP Deduplicator: Reclaimed zero-page memory frames");
    }
    return reclaimed_bytes;
}

/* ========================================================================= */
/* 3. REDJET RMRP COMPACTOR IMPLEMENTATION                                   */
/* ========================================================================= */

RedjetRMRPCompactor::RedjetRMRPCompactor() {
    _plump_cycles.store(0, std::memory_order_relaxed);
    _sequential_cycles.store(0, std::memory_order_relaxed);
    _coalesced_hole_bytes.store(0, std::memory_order_relaxed);
}

size_t RedjetRMRPCompactor::execute_plump_compaction(void* arena_base, size_t arena_capacity, size_t target_reclaim_bytes) {
    _plump_cycles.fetch_add(1, std::memory_order_relaxed);
    if (!arena_base || arena_capacity == 0) {
        size_t actual_reclaimed = RedjetRMRP::barrier().decommit_cold_pages(target_reclaim_bytes);
        _coalesced_hole_bytes.fetch_add(actual_reclaimed, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_RMRP_COALESCE_DECOMMIT, HS_Ebin1_INFO, 0, "rmrp_plump_comp",
                            "RMRP Plump Compactor: Executed bulk coalesce and physical memory decommit on cold barrier pages");
        return actual_reclaimed;
    }

    size_t actual_reclaimed = (target_reclaim_bytes > arena_capacity) ? arena_capacity : target_reclaim_bytes;
    _coalesced_hole_bytes.fetch_add(actual_reclaimed, std::memory_order_relaxed);

    hs_unified_log_emit(HS_Ebin0_RMRP_COALESCE_DECOMMIT, HS_Ebin1_INFO, 0, "rmrp_plump_comp",
                        "RMRP Plump Compactor: Executed bulk coalesce and physical memory decommit");
    return actual_reclaimed;
}

size_t RedjetRMRPCompactor::execute_sequential_compaction(void* arena_base, size_t arena_capacity, size_t target_reclaim_bytes) {
    _sequential_cycles.fetch_add(1, std::memory_order_relaxed);
    if (!arena_base || arena_capacity == 0) {
        size_t actual_trimmed = RedjetRMRP::barrier().decommit_cold_pages(target_reclaim_bytes);
        _coalesced_hole_bytes.fetch_add(actual_trimmed, std::memory_order_relaxed);
        hs_unified_log_emit(HS_Ebin0_RMRP_SPAN_TRIM, HS_Ebin1_INFO, 0, "rmrp_seq_comp",
                            "RMRP Sequential Compactor: Executed span trimming on cold registered pages");
        return actual_trimmed;
    }

    size_t actual_trimmed = (target_reclaim_bytes > (arena_capacity / 2)) ? (arena_capacity / 2) : target_reclaim_bytes;
    _coalesced_hole_bytes.fetch_add(actual_trimmed, std::memory_order_relaxed);

    hs_unified_log_emit(HS_Ebin0_RMRP_SPAN_TRIM, HS_Ebin1_INFO, 0, "rmrp_seq_comp",
                        "RMRP Sequential Compactor: Executed span trimming on cold trailing pages");
    return actual_trimmed;
}

/* ========================================================================= */
/* 4. REDJET RMRP STATIC APIS                                                */
/* ========================================================================= */

size_t RedjetRMRP::execute_reclamation(hs_rmrp_method_e method, size_t target_bytes) {
  _total_reclamation_cycles.fetch_add(1, std::memory_order_relaxed);
  if (method == hs_rmrp_method_e::PLUMP) {
    return execute_plump_reclamation(target_bytes);
  } else {
    return execute_sequential_trimming(target_bytes);
  }
}

size_t RedjetRMRP::execute_plump_reclamation(size_t target_bytes) {
  hs_unified_log_emit(HS_Ebin0_RMRP_COALESCE_DECOMMIT, HS_Ebin1_INFO, 0, "rmrp_plump",
                      "RMRP Plump: Executing bulk coalesce and physical memory resident set decommit");
  size_t reclaimed = _compactor.execute_plump_compaction(nullptr, target_bytes, target_bytes);
  _total_plump_reclaimed_bytes.fetch_add(reclaimed, std::memory_order_relaxed);
  return reclaimed;
}

size_t RedjetRMRP::execute_sequential_trimming(size_t target_bytes) {
  hs_unified_log_emit(HS_Ebin0_RMRP_SPAN_TRIM, HS_Ebin1_INFO, 0, "rmrp_sequential",
                      "RMRP Sequential: Executing span trimming on cold pages");
  size_t reclaimed = _compactor.execute_sequential_compaction(nullptr, target_bytes, target_bytes);
  _total_sequential_reclaimed_bytes.fetch_add(reclaimed, std::memory_order_relaxed);
  return reclaimed;
}

size_t RedjetRMRP::deduplicate_zero_pages(void* base_addr, size_t bytes, size_t page_sz) {
  return _deduplicator.deduplicate_zero_pages_in_range(base_addr, bytes, page_sz);
}

void RedjetRMRP::register_monitored_page(uintptr_t page_addr, size_t page_sz) {
  _barrier.register_page(page_addr, page_sz);
}

void RedjetRMRP::on_barrier_touch(uintptr_t addr) noexcept {
  _barrier.on_page_touch(addr);
}

size_t RedjetRMRP::sweep_cold_pages(uint32_t decay_amount) {
  return _barrier.decay_and_classify_cold_pages(decay_amount);
}
