/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:4700-4950.
 */

#include "gc/redlet/redletPitstop.hpp"
#include <new>

RedletPitstop::RedletPitstop() = default;

RedletPitstop::~RedletPitstop() {
    std::lock_guard<std::mutex> g(_lock);
    auto* cur = _head;
    while (cur) {
        auto* next = cur->next;
        cur->release();
        cur = next;
    }
    _head = nullptr;
    _count = 0;
}

sacculus_pitstop_entry_t* RedletPitstop::admit_excision(sfLiveHeader* obj) {
    if (!obj) return nullptr;
    std::lock_guard<std::mutex> g(_lock);
    auto* entry = new (std::nothrow) sacculus_pitstop_entry_t();
    if (!entry) return nullptr;

    entry->saved_header = *obj;
    entry->slot_addr = static_cast<void*>(obj);
    entry->slot_bytes = obj->total_cell_bytes;
    entry->mode = sf_puzzler_morph_e::EXCISION_EXCLUSIVE;
    entry->is_busy.store(true, std::memory_order_release);

    entry->next = _head;
    entry->prev = nullptr;
    if (_head) _head->prev = entry;
    _head = entry;
    ++_count;
    _total_morphed.fetch_add(1, std::memory_order_relaxed);

    sf_hotlog_emit(SF_Ebin0_PITSTOP_EXCISE, SF_Ebin1_INFO, obj->function_id, 0,
                   obj->generation, obj->region, 0, 0, 0, "pitstop_excision_exclusive");
    return entry;
}

sacculus_pitstop_entry_t* RedletPitstop::admit_shadow(sfLiveHeader* obj) {
    if (!obj) return nullptr;
    std::lock_guard<std::mutex> g(_lock);
    auto* entry = new (std::nothrow) sacculus_pitstop_entry_t();
    if (!entry) return nullptr;

    entry->saved_header = *obj;
    entry->slot_addr = static_cast<void*>(obj);
    entry->slot_bytes = obj->total_cell_bytes;
    entry->mode = sf_puzzler_morph_e::SHADOW_RCU;
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

bool RedletPitstop::restore_return(sacculus_pitstop_entry_t* entry, sfLiveHeader** out_restored) {
    if (!entry || !out_restored) return false;
    std::lock_guard<std::mutex> g(_lock);

    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    if (_head == entry) _head = entry->next;
    --_count;

    sfLiveHeader* restored = static_cast<sfLiveHeader*>(entry->slot_addr);
    *restored = entry->saved_header;
    *out_restored = restored;

    sf_hotlog_emit(SF_Ebin0_PITSTOP_RETURN, SF_Ebin1_INFO, restored->function_id, 0,
                   restored->generation, restored->region, 0, 0, 0, "pitstop_return");

    entry->release();
    return true;
}

sacculus_pitstop_entry_t* RedletPitstop::find_by_slot(void* slot) {
    if (!slot) return nullptr;
    std::lock_guard<std::mutex> g(_lock);
    auto* cur = _head;
    while (cur) {
        if (cur->slot_addr == slot) return cur;
        cur = cur->next;
    }
    return nullptr;
}
