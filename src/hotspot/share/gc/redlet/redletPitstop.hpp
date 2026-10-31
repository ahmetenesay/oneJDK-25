/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:4700-4950.
 */

#ifndef SHARE_GC_REDLET_REDLETPITSTOP_HPP
#define SHARE_GC_REDLET_REDLETPITSTOP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletMotherline.hpp"

enum class sf_puzzler_morph_e : uint8_t {
    EXCISION_EXCLUSIVE = 0,
    SHADOW_RCU        = 1
};

/* ========================================================================= */
/* 1. PITSTOP ENTRY (ISA MORPHING & REPLACEMENT BAY)                         */
/* ========================================================================= */

struct sacculus_pitstop_entry_t {
    void*                slot_addr{ nullptr };
    size_t               slot_bytes{ 0 };
    sfLiveHeader         saved_header;
    sf_puzzler_morph_e   mode{ sf_puzzler_morph_e::SHADOW_RCU };
    std::atomic<bool>    is_busy{ false };
    std::atomic<int32_t> ref_count{ 1 };

    sacculus_pitstop_entry_t* next{ nullptr };
    sacculus_pitstop_entry_t* prev{ nullptr };

    void retain() noexcept {
        ref_count.fetch_add(1, std::memory_order_relaxed);
    }
    void release() noexcept {
        if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

/* ========================================================================= */
/* 2. REDLET PITSTOP ENGINE (sacculus_pitstop_t)                             */
/* ========================================================================= */

class RedletPitstop {
private:
    sacculus_pitstop_entry_t* _head{ nullptr };
    size_t                    _count{ 0 };
    std::mutex                _lock;
    std::atomic<size_t>       _total_morphed{ 0 };

public:
    RedletPitstop();
    ~RedletPitstop();

    sacculus_pitstop_entry_t* admit_excision(sfLiveHeader* obj);
    sacculus_pitstop_entry_t* admit_shadow(sfLiveHeader* obj);
    bool restore_return(sacculus_pitstop_entry_t* entry, sfLiveHeader** out_restored);
    sacculus_pitstop_entry_t* find_by_slot(void* slot);

    size_t count() const { return _count; }
    size_t total_morphed() const { return _total_morphed.load(std::memory_order_relaxed); }
};

using sacculus_pitstop_t = RedletPitstop;

#endif // SHARE_GC_REDLET_REDLETPITSTOP_HPP
