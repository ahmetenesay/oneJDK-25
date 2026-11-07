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

#include "gc/redjet/api/redjetExplicitGC.hpp"
#include "gc/redjet/api/redjetTailTraces.hpp"
#include "gc/redjet/api/redjetRJGC.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"

std::atomic<uint64_t> RedjetExplicitGC::_explicit_hints_invoked{0};
std::atomic<uint64_t> RedjetExplicitGC::_explicit_hints_preflighted{0};
std::atomic<uint64_t> RedjetExplicitGC::_bytes_reclaimed_by_hints{0};

void RedjetExplicitGC::preflight_gc_hint(uint32_t method_id, uint32_t bci) {
  _explicit_hints_preflighted.fetch_add(1, std::memory_order_relaxed);
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, method_id, bci, 0, 0, 0, 0, 0, "ExplicitGC:PreflightHint");
}

static void explicit_reclaim_sink(void* obj, uint32_t region_id) {
  (void)obj;
  (void)region_id;
}

void RedjetExplicitGC::invoke_explicit_clean_hint() {
  _explicit_hints_invoked.fetch_add(1, std::memory_order_relaxed);

  if (RedjetGCInvokesExplicitCleanHints) {
    // 1. Drain all pending dead reference signals in RPSS ring
    size_t dead_drained = g_redjet_tail_traces.drain_signals(explicit_reclaim_sink);

    // 2. Poll and evict zero-ref continuous off-heap handles
    size_t off_heap_reclaimed = RedjetRJGC::poll_and_evict_continuous();

    // 3. Lightweight sequential resident set reclamation (Zero-STW)
    size_t rmrp_reclaimed = RedjetRMRP::execute_reclamation(hs_rmrp_method_e::SEQUENTIAL, 64 * 1024 * 1024);

    size_t total_reclaimed = off_heap_reclaimed + rmrp_reclaimed;
    _bytes_reclaimed_by_hints.fetch_add(total_reclaimed, std::memory_order_relaxed);

    hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, (uint32_t)dead_drained,
                   (uint32_t)(total_reclaimed / 1024), 0, 0, 0, 0,
                   (uint32_t)total_reclaimed, "ExplicitGC:CleanHintExecuted");
  }
}
