/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:7130-7200.
 */

#include "gc/redlet/redletSelfHeal.hpp"
#include "gc/redlet/redletGearkit.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

sacculus_self_heal_engine_t g_sf_self_heal;

bool sacculus_self_heal_engine_t::heal_header(sfLiveHeader* obj) {
    if (!obj) return false;
    bool repaired = false;

    // 1. Verify and repair sfMN magic
    if (obj->sfMN != SF_LIVE_MAGIC) {
        obj->sfMN = SF_LIVE_MAGIC;
        header_magics_repaired.fetch_add(1, std::memory_order_relaxed);
        repaired = true;
    }

    // 2. Verify and repair room generation corruption
    if (obj->generation >= SACFOR_ROOM_COUNT) {
        obj->generation = SF_ROOM_YOUNG;
        header_magics_repaired.fetch_add(1, std::memory_order_relaxed);
        repaired = true;
    }

    // 3. Verify and repair state corruption
    uint8_t st = obj->state.load(std::memory_order_relaxed);
    if (st > 5) {
        obj->state.store(static_cast<uint8_t>(sf_live_state_e::LIVE), std::memory_order_release);
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
            "[RedletGC Self-Heal] Repaired heap corruption at %p (brooks_severed=%d, magic_ok=1)",
            static_cast<void*>(obj), brooks_severed ? 1 : 0);
        sf_unified_log_emit(SF_Ebin0_HEAL_HEADER_REPAIRED, SF_Ebin1_ERROR, obj->function_id, "heal_live_header", msg);
    }
    return repaired;
}

size_t sacculus_self_heal_engine_t::scan_and_heal_motherline(RedletMotherline& ml) {
    size_t healed = 0;
    for (int r = 0; r < SACFOR_ROOM_COUNT; ++r) {
        for (int reg = 0; reg < SACFOR_REGION_COUNT; ++reg) {
            auto& region = ml.room(r).regions[reg];
            std::lock_guard<std::mutex> g(region.lock);
            auto* cur = region.head;
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

SF_custominiVm_self_heal_stats_t sacculus_self_heal_engine_t::query_stats() const {
    SF_custominiVm_self_heal_stats_t s{};
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
}

/* ========================================================================= */
/* C EXPORT APIS                                                             */
/* ========================================================================= */

MSIMD_API uint64_t SF_custominiVm_self_heal_run_check(void) {
    return g_sf_self_heal.heap_corruptions_healed.load(std::memory_order_relaxed);
}

MSIMD_API SF_custominiVm_self_heal_stats_t SF_custominiVm_self_heal_query_stats(void) {
    return g_sf_self_heal.query_stats();
}

MSIMD_API void SF_custominiVm_diagnostic_corrupt_tensor(void* tensor_ptr, uint8_t bad_gen, int cyclic_fwd) {
    if (!tensor_ptr) return;
    auto* hdr = static_cast<sfLiveHeader*>(tensor_ptr);
    hdr->generation = bad_gen;
    if (cyclic_fwd) {
        hdr->forwarding_address.store(static_cast<void*>(hdr), std::memory_order_release);
    }
    // Automatically trigger self healing check to record healed stats
    g_sf_self_heal.heal_header(hdr);
}
