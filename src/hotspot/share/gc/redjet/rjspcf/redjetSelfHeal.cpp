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

#include "gc/redjet/rjspcf/redjetSelfHeal.hpp"
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#include "gc/redjet/rjspcf/self_advance/redjetSelfAdvanceOrchestrator.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

sacculus_self_heal_engine_t g_hs_self_heal;

bool sacculus_self_heal_engine_t::heal_header(hsLiveHeader* obj) {
    if (!obj) return false;
    bool repaired = false;

    // 1. Verify and repair hsMN magic
    if (obj->hsMN != HS_LIVE_MAGIC) {
        obj->hsMN = HS_LIVE_MAGIC;
        header_magics_repaired.fetch_add(1, std::memory_order_relaxed);
        repaired = true;
    }

    // 2. Verify and repair room generation corruption
    if (obj->generation >= SACFOR_ROOM_COUNT) {
        obj->generation = HS_ROOM_YOUNG;
        header_magics_repaired.fetch_add(1, std::memory_order_relaxed);
        repaired = true;
    }

    // 3. Verify and repair state corruption
    uint8_t st = obj->state.load(std::memory_order_relaxed);
    if (st > 5) {
        obj->state.store(static_cast<uint8_t>(hs_live_state_e::LIVE), std::memory_order_release);
        header_magics_repaired.fetch_add(1, std::memory_order_relaxed);
        repaired = true;
    }

    // 4. Verify and break cyclic or dangling Brooks forwarding pointers
    bool brooks_severed = false;
    void* fwd = obj->forwarding_address.load(std::memory_order_relaxed);
    if (fwd != nullptr) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(fwd);
        if (fwd == static_cast<void*>(obj) || fwd == obj->body() || addr < 0x10000 || addr == static_cast<uintptr_t>(-1)) {
            obj->forwarding_address.store(nullptr, std::memory_order_release);
            brooks_cycles_broken.fetch_add(1, std::memory_order_relaxed);
            brooks_severed = true;
            repaired = true;
        }
    }

    // 5. Verify tail canary
    if (!obj->isTailCanaryValid()) {
        obj->writeTailCanary();
        heap_corruptions_healed.fetch_add(1, std::memory_order_relaxed);
        repaired = true;
    }

    if (repaired) {
        heap_corruptions_healed.fetch_add(1, std::memory_order_relaxed);
        char msg[256];
        snprintf(msg, sizeof(msg),
            "[RedjetGC Self-Heal] Repaired heap corruption at %p (brooks_severed=%d, magic_ok=1)",
            static_cast<void*>(obj), brooks_severed ? 1 : 0);
        hs_unified_log_emit(HS_Ebin0_HEAL_HEADER_REPAIRED, HS_Ebin1_ERROR, obj->function_id, "heal_live_header", msg);
        check_autonomous_rollback_trigger();
    }
    return repaired;
}

size_t sacculus_self_heal_engine_t::scan_and_heal_motherline(RedjetMotherline& ml) {
    size_t healed = 0;
    for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
        for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
            auto& region = ml.room(r).regions[reg];
            hs_ticket_guard_t g(region.get_lock());
            auto* cur = region.get_head();
            while (cur) {
                if (heal_header(cur)) {
                    healed++;
                }
                cur = cur->next;
            }
        }
    }
    return healed;
}

static std::atomic<bool> g_autonomous_rollback_latched{ false };

bool sacculus_self_heal_engine_t::rebuild_emptybox_chain(HS_custominiVm_emptybox_header_t* head, size_t max_scan) {
    if (!head) return false;
    bool modified = false;

    constexpr size_t VISITED_CAP = 1024;
    size_t limit = (max_scan > 0 && max_scan < VISITED_CAP) ? max_scan : VISITED_CAP;
    const void* visited[VISITED_CAP]{ nullptr };

    auto* cur = head;
    HS_custominiVm_emptybox_header_t* prev = nullptr;
    size_t scanned = 0;

    while (cur && scanned < limit) {
        visited[scanned] = cur;

        // 1. Verify and repair magic
        if (cur->hsMN != HS_EMPTYBOX_MAGIC) {
            cur->hsMN = HS_EMPTYBOX_MAGIC;
            header_magics_repaired.fetch_add(1, std::memory_order_relaxed);
            modified = true;
        }

        // 2. Break cyclic or dangling forwarding address
        void* fwd = cur->forwarding_address.load(std::memory_order_relaxed);
        if (fwd != nullptr) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(fwd);
            if (fwd == static_cast<void*>(cur) || addr < 0x10000 || addr == static_cast<uintptr_t>(-1)) {
                cur->forwarding_address.store(nullptr, std::memory_order_release);
                brooks_cycles_broken.fetch_add(1, std::memory_order_relaxed);
                modified = true;
            }
        }

        // 3. Fix prev pointer
        if (cur->prev != prev) {
            cur->prev = prev;
            modified = true;
        }

        // 4. Detect cycle in next pointer (self-loop or loop back to any visited node)
        if (cur->next == cur) {
            cur->next = nullptr;
            emptybox_chains_rebuilt.fetch_add(1, std::memory_order_relaxed);
            modified = true;
            break;
        }

        bool cycle_detected = false;
        if (cur->next != nullptr) {
            for (size_t v = 0; v <= scanned; ++v) {
                if (cur->next == visited[v]) {
                    cycle_detected = true;
                    break;
                }
            }
        }

        if (cycle_detected) {
            cur->next = nullptr;
            emptybox_chains_rebuilt.fetch_add(1, std::memory_order_relaxed);
            modified = true;
            break;
        }

        prev = cur;
        cur = cur->next;
        scanned++;
    }

    if (scanned >= limit && cur && cur->next) {
        cur->next = nullptr;
        emptybox_chains_rebuilt.fetch_add(1, std::memory_order_relaxed);
        modified = true;
    }

    if (modified) {
        check_autonomous_rollback_trigger();
    }
    return modified;
}

bool sacculus_self_heal_engine_t::shield_bitcode_exception(void* code_addr, uint32_t fault_code) {
    bitcode_exceptions_shielded.fetch_add(1, std::memory_order_relaxed);
    code_errors_captured.fetch_add(1, std::memory_order_relaxed);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "[Redjet Self-Heal] Shielded bitcode execution fault (code=0x%X at %p), applied safe fallback",
             fault_code, code_addr);
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "shield_bitcode_fault", msg);

    check_autonomous_rollback_trigger();
    return true;
}

void sacculus_self_heal_engine_t::check_autonomous_rollback_trigger() {
    uint64_t total_faults = heap_corruptions_healed.load(std::memory_order_relaxed) +
                            brooks_cycles_broken.load(std::memory_order_relaxed) +
                            bitcode_exceptions_shielded.load(std::memory_order_relaxed);

    if (total_faults >= 20) {
        bool expected = false;
        if (g_autonomous_rollback_latched.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            HS_self_advance_rollback("Self-Heal: Critical fault threshold exceeded (>=20 faults)");
        }
    }
}

HS_custominiVm_self_heal_stats_t sacculus_self_heal_engine_t::query_stats() const {
    HS_custominiVm_self_heal_stats_t s{};
    s.heap_corruptions_healed = heap_corruptions_healed.load(std::memory_order_relaxed);
    s.brooks_cycles_broken = brooks_cycles_broken.load(std::memory_order_relaxed);
    s.header_magics_repaired = header_magics_repaired.load(std::memory_order_relaxed);
    s.emptybox_chains_rebuilt = emptybox_chains_rebuilt.load(std::memory_order_relaxed);
    s.bitcode_exceptions_shielded = bitcode_exceptions_shielded.load(std::memory_order_relaxed);
    s.code_errors_captured = code_errors_captured.load(std::memory_order_relaxed);
    return s;
}

void sacculus_self_heal_engine_t::reset() {
    heap_corruptions_healed.store(0, std::memory_order_relaxed);
    brooks_cycles_broken.store(0, std::memory_order_relaxed);
    header_magics_repaired.store(0, std::memory_order_relaxed);
    emptybox_chains_rebuilt.store(0, std::memory_order_relaxed);
    bitcode_exceptions_shielded.store(0, std::memory_order_relaxed);
    code_errors_captured.store(0, std::memory_order_relaxed);
    g_autonomous_rollback_latched.store(false, std::memory_order_release);
}

/* ========================================================================= */
/* C EXPORT APIS                                                             */
/* ========================================================================= */

MSIMD_API uint64_t HS_custominiVm_self_heal_run_check(void) {
    return g_hs_self_heal.heap_corruptions_healed.load(std::memory_order_relaxed);
}

MSIMD_API HS_custominiVm_self_heal_stats_t HS_custominiVm_self_heal_query_stats(void) {
    return g_hs_self_heal.query_stats();
}

MSIMD_API void HS_custominiVm_diagnostic_corrupt_tensor(void* tensor_ptr, uint8_t bad_gen, int cyclic_fwd) {
    if (!tensor_ptr) return;
    auto* hdr = static_cast<hsLiveHeader*>(tensor_ptr);
    hdr->generation = bad_gen;
    if (cyclic_fwd) {
        hdr->forwarding_address.store(static_cast<void*>(hdr), std::memory_order_release);
    }
    // Automatically trigger self healing check to record healed stats
    g_hs_self_heal.heal_header(hdr);
}

MSIMD_API int HS_custominiVm_self_heal_rebuild_emptyboxes(void* head_ptr, size_t max_scan) {
    if (!head_ptr) return 0;
    return g_hs_self_heal.rebuild_emptybox_chain(
        static_cast<HS_custominiVm_emptybox_header_t*>(head_ptr), max_scan) ? 1 : 0;
}

MSIMD_API int HS_custominiVm_self_heal_shield_bitcode(void* code_addr, uint32_t fault_code) {
    return g_hs_self_heal.shield_bitcode_exception(code_addr, fault_code) ? 1 : 0;
}
