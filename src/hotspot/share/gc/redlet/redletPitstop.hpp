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

#ifndef SHARE_GC_REDLET_REDLETPITSTOP_HPP
#define SHARE_GC_REDLET_REDLETPITSTOP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletRingLog.hpp"
#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletEmptyBox.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletIntentCLI.hpp"

#ifndef SF_PUZZLER_MORPH_E_DEFINED
#define SF_PUZZLER_MORPH_E_DEFINED
enum class sf_puzzler_morph_e : uint8_t {
    EXCISION_EXCLUSIVE = 0,
    SHADOW_RCU         = 1
};
#endif

using sf_object_ready_callback_t = void (*)(void* obj_ptr, void* user_data);

/* ========================================================================= */
/* 1. PITSTOP ENTRY (ISA MORPHING & REPLACEMENT BAY)                         */
/* ========================================================================= */

struct sacculus_pitstop_entry_t {
    alignas(64) SF_custominiVm_live_header_t saved_header;
    void*                      slot_addr{ nullptr };
    size_t                     slot_bytes{ 0 };
    sf_puzzler_morph_e         mode{ sf_puzzler_morph_e::SHADOW_RCU };
    std::atomic<bool>          is_busy{ false };
    std::atomic<uint32_t>      ref_count{ 1 };
    std::mutex                 wait_mtx;
    std::condition_variable    wait_cv;
    sf_object_ready_callback_t ready_callback{ nullptr };
    void*                      callback_user_data{ nullptr };

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
    sf_ticket_lock_t          _lock;
    std::atomic<size_t>       _total_morphed{ 0 };

public:
    RedletPitstop();
    ~RedletPitstop();

    sacculus_pitstop_entry_t* admit_excision(sfLiveHeader* obj);
    sacculus_pitstop_entry_t* admit_shadow(sfLiveHeader* obj);
    sacculus_pitstop_entry_t* admit(sfLiveHeader* obj);
    void                      evict(sfLiveHeader* obj);
    bool restore_return(sacculus_pitstop_entry_t* entry, sfLiveHeader** out_restored = nullptr);
    sacculus_pitstop_entry_t* find_by_slot(void* slot_or_body, bool retain_ref = false);
    bool is_busy(const void* ptr);
    int await_ready(void* ptr, uint32_t timeout_ms = 0);
    int register_callback(void* ptr, sf_object_ready_callback_t callback, void* user_data);

    void destroy();

    size_t count() const noexcept { return _count; }
    size_t total_morphed() const noexcept { return _total_morphed.load(std::memory_order_relaxed); }
};

using sacculus_pitstop_t = RedletPitstop;

#endif // SHARE_GC_REDLET_REDLETPITSTOP_HPP
