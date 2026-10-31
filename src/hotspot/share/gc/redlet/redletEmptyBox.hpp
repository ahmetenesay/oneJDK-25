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

#ifndef SHARE_GC_REDLET_REDLETEMPTYBOX_HPP
#define SHARE_GC_REDLET_REDLETEMPTYBOX_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "gc/redlet/redletGearkit.hpp"

#ifndef SACFOR_CACHE_LINE
#define SACFOR_CACHE_LINE 64
#endif

#define SF_EMPTYBOX_MAGIC 0x53464542U

struct SACFOR_ALIGN_CL SF_custominiVm_emptybox_header_t {
    uint32_t           sfMN{ SF_EMPTYBOX_MAGIC };
    uint32_t           prev_function_id{ 0 };
    size_t             prev_occupant_size{ 0 };
    size_t             aligned_hole_size{ 0 };

    uint8_t            _magic{ 0xEB };
    uint8_t            is_decommitted{ 0 };
    uint8_t            _pad[2]{ 0, 0 };
    uint32_t           decommitted_kb{ 0 };

    std::atomic<void*> forwarding_address{ nullptr };
    uint64_t           created_sweep_no{ 0 };

    SF_custominiVm_emptybox_header_t* next{ nullptr };
    SF_custominiVm_emptybox_header_t* prev{ nullptr };

    void init(uint32_t prev_fn, size_t prev_size, size_t aligned_size, void* fwd = nullptr, uint64_t sweep_no = 0) {
        sfMN = SF_EMPTYBOX_MAGIC;
        prev_function_id = prev_fn;
        prev_occupant_size = prev_size;
        aligned_hole_size = aligned_size;
        _magic = 0xEB;
        is_decommitted = 0;
        _pad[0] = _pad[1] = 0;
        decommitted_kb = 0;
        forwarding_address.store(fwd, std::memory_order_release);
        created_sweep_no = sweep_no;
        next = prev = nullptr;
    }

    inline bool isValid() const noexcept {
        return sfMN == SF_EMPTYBOX_MAGIC || _magic == 0xEB;
    }

    inline uint8_t* hole_body() noexcept {
        size_t hdr_sz = (sizeof(SF_custominiVm_emptybox_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        return reinterpret_cast<uint8_t*>(this) + hdr_sz;
    }

    static inline SF_custominiVm_emptybox_header_t* from_body(void* body_ptr) noexcept {
        if (!body_ptr) return nullptr;
        size_t hdr_sz = (sizeof(SF_custominiVm_emptybox_header_t) + SACFOR_CACHE_LINE - 1) & ~(SACFOR_CACHE_LINE - 1);
        auto* eb = reinterpret_cast<SF_custominiVm_emptybox_header_t*>(static_cast<uint8_t*>(body_ptr) - hdr_sz);
        const volatile uint32_t* magic32 = &eb->sfMN;
        const volatile uint8_t* magic8 = &eb->_magic;
        if (*magic32 != SF_EMPTYBOX_MAGIC && *magic8 != 0xEB) return nullptr;
        return eb;
    }

    inline void scrub_coalesced_tombstone() noexcept {
        *const_cast<volatile uint32_t*>(&sfMN) = 0;
        *const_cast<volatile uint8_t*>(&_magic) = 0;
        SF_COMPILER_BARRIER(this);
    }
};

using sfEmptyboxHeader = SF_custominiVm_emptybox_header_t;
using sacculus_emptybox_header_t = sfEmptyboxHeader;
using RedletEmptyBox = SF_custominiVm_emptybox_header_t;

#endif // SHARE_GC_REDLET_REDLETEMPTYBOX_HPP
