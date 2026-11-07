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

#include "gc/redjet/engine/redjetBatchScope.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetThreadLocalData.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/simd/redjetDispatchISA.hpp"
#include "gc/redjet/engine/redjetEntangle.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"
#include "gc/redjet/engine/redjetPitstop.hpp"
#include "gc/redjet/allocator/redjetRunway.hpp"
#include "gc/redjet/runtime/redjetGC.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "runtime/thread.hpp"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

static std::atomic<uint32_t> g_redjet_batch_seq{ 0x2000u };
static std::atomic<uint32_t> g_redjet_group_seq{ 0x1000u };

struct RedjetThreadBatchContext {
  RedjetBatchFrame stack[REDJET_MAX_BATCH_DEPTH]{};
  uint32_t         depth{ 0 };
  uint32_t         saved_entangle_warmup{ 4 };
  bool             entangle_warmup_overridden{ false };
};

static thread_local RedjetThreadBatchContext tl_batch_ctx{};

static inline uint32_t clamp_priority(uint32_t p) noexcept {
  return (p > 10u) ? 10u : p;
}

static inline uint64_t pack_scope_handle(uint32_t slot_idx, uint32_t id_token) noexcept {
  return (static_cast<uint64_t>(slot_idx + 1u) << 32) | static_cast<uint64_t>(id_token);
}

static inline bool unpack_scope_handle(uint64_t handle, uint32_t& out_slot, uint32_t& out_id) noexcept {
  if (handle == 0ULL) return false;
  uint32_t encoded_slot = static_cast<uint32_t>(handle >> 32);
  if (encoded_slot == 0u || encoded_slot > REDJET_MAX_BATCH_DEPTH) return false;
  out_slot = encoded_slot - 1u;
  out_id   = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
  return true;
}

struct BestHardwareSimdTier {
  const char* isa_name;
  uint32_t    ladder_level;
};

static BestHardwareSimdTier detect_highest_simd_tier() noexcept {
  static std::atomic<bool> cached_ready{ false };
  static char              cached_name[32]{ "SCALAR" };
  static uint32_t          cached_level{ 1u };

  if (!cached_ready.load(std::memory_order_acquire)) {
    hs_cpu_isa_inventory_t inv = hs_cpu_isa_inventory_t::discover();
    const char* chosen = "SCALAR";
    uint32_t level = 1u;

    if (inv.has_isa("AVX512F") || inv.has_isa("AVX-512") || inv.has_isa("AVX512BW")) {
      chosen = "AVX-512";
      level  = 4u;
    } else if (inv.has_isa("SVE2")) {
      chosen = "SVE2";
      level  = 4u;
    } else if (inv.has_isa("SVE")) {
      chosen = "SVE";
      level  = 3u;
    } else if (inv.has_isa("RVV") || inv.has_isa("RVV1.0")) {
      chosen = "RVV";
      level  = 3u;
    } else if (inv.has_isa("AVX2")) {
      chosen = "AVX2";
      level  = 3u;
    } else if (inv.has_isa("AVX")) {
      chosen = "AVX";
      level  = 2u;
    } else if (inv.has_isa("NEON") || inv.has_isa("ASIMD")) {
      chosen = "NEON";
      level  = 2u;
    } else if (inv.has_isa("SSE4.2") || inv.has_isa("SSE4_2")) {
      chosen = "SSE4.2";
      level  = 2u;
    }

    std::strncpy(cached_name, chosen, sizeof(cached_name) - 1);
    cached_name[sizeof(cached_name) - 1] = '\0';
    cached_level = level;
    cached_ready.store(true, std::memory_order_release);
  }

  return { cached_name, cached_level };
}

#ifndef REDJET_STANDALONE_BUILD
template <typename TLD>
static void apply_tld_point_sync(Thread* thread,
                                 bool batch_active,
                                 bool bypass_active,
                                 bool nested_active,
                                 uint32_t depth,
                                 uint32_t group_hex_id,
                                 uint32_t priority) noexcept {
  if (!thread || !UseRedjetGC) return;

  constexpr uint32_t managed_mask =
      static_cast<uint32_t>(REDJET_PT_BATCH_ACTIVE) |
      static_cast<uint32_t>(REDJET_PT_BYPASS_WARMUP) |
      static_cast<uint32_t>(REDJET_PT_NESTED_ACTIVE);

  uint32_t active_bits = 0u;
  if (batch_active)  active_bits |= static_cast<uint32_t>(REDJET_PT_BATCH_ACTIVE);
  if (bypass_active) active_bits |= static_cast<uint32_t>(REDJET_PT_BYPASS_WARMUP);
  if (nested_active) active_bits |= static_cast<uint32_t>(REDJET_PT_NESTED_ACTIVE);

  uint32_t clear_bits = managed_mask & ~active_bits;

  if constexpr (requires { TLD::add_point_flags(thread, active_bits); TLD::clear_point_flags(thread, clear_bits); }) {
    if (clear_bits != 0u) TLD::clear_point_flags(thread, clear_bits);
    if (active_bits != 0u) TLD::add_point_flags(thread, active_bits);
  } else if constexpr (requires { TLD::set_point_flag(thread, active_bits, true); }) {
    TLD::set_point_flag(thread, static_cast<uint32_t>(REDJET_PT_BATCH_ACTIVE), batch_active);
    TLD::set_point_flag(thread, static_cast<uint32_t>(REDJET_PT_BYPASS_WARMUP), bypass_active);
    TLD::set_point_flag(thread, static_cast<uint32_t>(REDJET_PT_NESTED_ACTIVE), nested_active);
  } else if constexpr (requires { TLD::point_flags(thread); TLD::set_point_flags(thread, active_bits); }) {
    uint32_t cur = static_cast<uint32_t>(TLD::point_flags(thread));
    cur = (cur & ~managed_mask) | active_bits;
    TLD::set_point_flags(thread, cur);
  }

  if constexpr (requires { TLD::set_batch_depth(thread, depth); }) {
    TLD::set_batch_depth(thread, depth);
  }
  if constexpr (requires { TLD::set_group_hex_id(thread, group_hex_id); }) {
    TLD::set_group_hex_id(thread, group_hex_id);
  }
  if constexpr (requires { TLD::set_group_priority(thread, static_cast<uint8_t>(priority)); }) {
    TLD::set_group_priority(thread, static_cast<uint8_t>(priority));
  }
}
#endif

static void sync_thread_execution_state() noexcept {
  const uint32_t d = tl_batch_ctx.depth;
  const bool active = (d > 0u);
  const bool bypass = active ? tl_batch_ctx.stack[d - 1u].bypass_warmup : false;
  const bool nested = (d > 1u) && tl_batch_ctx.stack[d - 1u].nested_allowed;
  const uint32_t gid  = active ? tl_batch_ctx.stack[d - 1u].group_hex_id : 0u;
  const uint32_t prio = active ? tl_batch_ctx.stack[d - 1u].priority : 5u;

  if (bypass) {
    if (!tl_batch_ctx.entangle_warmup_overridden) {
      tl_batch_ctx.saved_entangle_warmup = g_hs_entangle_engine.warmup_threshold.load(std::memory_order_relaxed);
      tl_batch_ctx.entangle_warmup_overridden = true;
    }
    g_hs_entangle_engine.warmup_threshold.store(1u, std::memory_order_release);
  } else if (tl_batch_ctx.entangle_warmup_overridden) {
    uint32_t restore = tl_batch_ctx.saved_entangle_warmup > 0u ? tl_batch_ctx.saved_entangle_warmup : 4u;
    g_hs_entangle_engine.warmup_threshold.store(restore, std::memory_order_release);
    tl_batch_ctx.entangle_warmup_overridden = false;
  }

#ifndef REDJET_STANDALONE_BUILD
  Thread* cur_thread = Thread::current_or_null();
  if (cur_thread != nullptr) {
    apply_tld_point_sync<RedjetThreadLocalData>(cur_thread, active, bypass, nested, d, gid, prio);
  }
#else
  (void)gid;
  (void)prio;
#endif
}

static RedjetMotherline* resolve_active_motherline() noexcept {
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC) {
    RedjetHeap* h = RedjetHeap::heap();
    if (h != nullptr && h->motherline() != nullptr) {
      return h->motherline();
    }
  }
#endif
  RedjetGC* dgc = hs_get_default_gc();
  return dgc ? dgc->motherline() : nullptr;
}

static RedjetPitstop* resolve_active_pitstop() noexcept {
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC) {
    RedjetHeap* h = RedjetHeap::heap();
    if (h != nullptr && h->pitstop() != nullptr) {
      return h->pitstop();
    }
  }
#endif
  RedjetGC* dgc = hs_get_default_gc();
  return (dgc && dgc->runway()) ? &dgc->runway()->pitstop : nullptr;
}

static void promote_live_hdr_bypass_warmup(HS_hotspot_live_header_t* hdr,
                                           uint32_t group_hex_id,
                                           uint32_t priority) noexcept {
  if (!hdr || hdr->hsMN != HS_LIVE_MAGIC) return;

  RedjetDispatchISA::init();
  BestHardwareSimdTier best = detect_highest_simd_tier();

  const bool was_egg = (hdr->has_optimized.load(std::memory_order_acquire) == 0u);
  hdr->group_hex_id   = group_hex_id;
  hdr->group_priority = static_cast<uint8_t>(priority);
  hdr->priority       = static_cast<uint8_t>(priority);
  hdr->opt_permitted  = 0u; // Lock out slow Puzzler warmup profiling

  hdr->set_active_simd_candidate(best.isa_name, best.ladder_level);
  hdr->opt_success = 1u;
  hdr->optimization_duration_cycles.store(1ULL, std::memory_order_relaxed);
  hdr->last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
  hdr->has_optimized.store(1u, std::memory_order_release);

  if (hdr->memory_footprint.load(std::memory_order_relaxed) > 0 && !hdr->isTailCanaryValid()) {
    hdr->writeTailCanary();
  }

  if (was_egg) {
    if (RedjetMotherline* ml = resolve_active_motherline()) {
      ml->promote_to_fledgling(hdr);
    }
  }
}

} // namespace

uint64_t RedjetBatchScopeManager::enter_batch(uint32_t priority, bool bypass_warmup, bool nested_allowed) {
  const uint32_t clamped_prio = clamp_priority(priority);

  if (tl_batch_ctx.depth > 0u && !tl_batch_ctx.stack[tl_batch_ctx.depth - 1u].nested_allowed) {
    // Parent scope disallowed nesting: coalesce onto top-of-stack handle without pushing a new frame.
    const uint32_t top_idx = tl_batch_ctx.depth - 1u;
    return pack_scope_handle(top_idx, tl_batch_ctx.stack[top_idx].batch_id);
  }

  if (tl_batch_ctx.depth >= REDJET_MAX_BATCH_DEPTH) {
    const uint32_t top_idx = REDJET_MAX_BATCH_DEPTH - 1u;
    hs_hotlog_emit(HS_Ebin0_GROUP_COMPUTE_BEGIN, HS_Ebin1_WARN,
                   tl_batch_ctx.stack[top_idx].batch_id,
                   static_cast<uint16_t>(tl_batch_ctx.depth),
                   0, 0, 0, 0, 0, "batch_depth_clamped_16");
    return pack_scope_handle(top_idx, tl_batch_ctx.stack[top_idx].batch_id);
  }

  const uint32_t slot_idx = tl_batch_ctx.depth;
  const bool inherited_bypass = (slot_idx > 0u) ? tl_batch_ctx.stack[slot_idx - 1u].bypass_warmup : false;
  const bool effective_bypass = bypass_warmup || inherited_bypass;

  const uint32_t batch_id = g_redjet_batch_seq.fetch_add(1u, std::memory_order_relaxed);
  const uint32_t group_id = HS_hotspot_batch_begin(static_cast<uint8_t>(clamped_prio),
                                                       effective_bypass ? 1 : 0);

  RedjetBatchFrame& frame = tl_batch_ctx.stack[slot_idx];
  std::memset(&frame, 0, sizeof(RedjetBatchFrame));
  frame.batch_id        = batch_id;
  frame.group_hex_id    = group_id;
  frame.priority        = clamped_prio;
  frame.bypass_warmup   = effective_bypass;
  frame.nested_allowed  = nested_allowed;
  frame.is_group_scope  = false;
  frame.aborted         = false;
  frame.allocated_bytes = 0;
  frame.egg_count       = 0;
  frame.enter_cycle     = sacculus_rdtsc();

  tl_batch_ctx.depth = slot_idx + 1u;

  HS_hotspot_entangle_begin();
  sync_thread_execution_state();

  return pack_scope_handle(slot_idx, batch_id);
}

void RedjetBatchScopeManager::exit_batch(uint64_t scope_handle) {
  uint32_t target_slot = 0u;
  uint32_t token_id    = 0u;
  if (!unpack_scope_handle(scope_handle, target_slot, token_id)) {
    return;
  }
  if (tl_batch_ctx.depth == 0u || target_slot >= tl_batch_ctx.depth) {
    return;
  }
  if (tl_batch_ctx.stack[target_slot].batch_id != token_id) {
    return;
  }

  // Unwind from current top down to target_slot cleanly
  while (tl_batch_ctx.depth > target_slot) {
    const uint32_t cur_idx = tl_batch_ctx.depth - 1u;
    RedjetBatchFrame& frame = tl_batch_ctx.stack[cur_idx];

    if (!frame.is_group_scope) {
      if (!frame.aborted) {
        if (!frame.bypass_warmup && frame.egg_count > 0u) {
          const size_t count = (frame.egg_count <= REDJET_MAX_STAGED_EGGS)
                                 ? static_cast<size_t>(frame.egg_count)
                                 : static_cast<size_t>(REDJET_MAX_STAGED_EGGS);
          (void)batch_optimize_egg_objects(frame.staged_eggs, count);
        }
        HS_hotspot_entangle_commit();
      } else {
        HS_hotspot_entangle_cancel();
      }
      HS_hotspot_batch_end();
    } else {
      if (!frame.aborted && !frame.bypass_warmup && frame.egg_count > 0u) {
        const size_t count = (frame.egg_count <= REDJET_MAX_STAGED_EGGS)
                               ? static_cast<size_t>(frame.egg_count)
                               : static_cast<size_t>(REDJET_MAX_STAGED_EGGS);
        (void)batch_optimize_egg_objects(frame.staged_eggs, count);
      }
      HS_hotspot_batch_end();
    }

    std::memset(&frame, 0, sizeof(RedjetBatchFrame));
    tl_batch_ctx.depth = cur_idx;
  }

  sync_thread_execution_state();
}

void RedjetBatchScopeManager::abort_batch(uint64_t scope_handle) {
  uint32_t target_slot = 0u;
  uint32_t token_id    = 0u;
  if (!unpack_scope_handle(scope_handle, target_slot, token_id)) {
    return;
  }
  if (tl_batch_ctx.depth == 0u || target_slot >= tl_batch_ctx.depth) {
    return;
  }
  if (tl_batch_ctx.stack[target_slot].batch_id != token_id) {
    return;
  }

  for (uint32_t i = target_slot; i < tl_batch_ctx.depth; ++i) {
    tl_batch_ctx.stack[i].aborted   = true;
    tl_batch_ctx.stack[i].egg_count = 0u;
  }

  exit_batch(scope_handle);
}

uint64_t RedjetBatchScopeManager::enter_group(uint32_t group_hex_id, uint32_t priority, bool bypass_warmup) {
  const uint32_t clamped_prio = clamp_priority(priority);

  if (tl_batch_ctx.depth >= REDJET_MAX_BATCH_DEPTH) {
    const uint32_t top_idx = REDJET_MAX_BATCH_DEPTH - 1u;
    return pack_scope_handle(top_idx, tl_batch_ctx.stack[top_idx].group_hex_id);
  }

  const uint32_t slot_idx = tl_batch_ctx.depth;
  const bool inherited_bypass = (slot_idx > 0u) ? tl_batch_ctx.stack[slot_idx - 1u].bypass_warmup : false;
  const bool effective_bypass = bypass_warmup || inherited_bypass;

  const uint32_t assigned_gid = (group_hex_id != 0u)
      ? group_hex_id
      : g_redjet_group_seq.fetch_add(1u, std::memory_order_relaxed);

  // Push onto underlying HSVM 16-deep dispatch stack for C-API interoperability
  (void)HS_hotspot_batch_begin(static_cast<uint8_t>(clamped_prio), effective_bypass ? 1 : 0);

  RedjetBatchFrame& frame = tl_batch_ctx.stack[slot_idx];
  std::memset(&frame, 0, sizeof(RedjetBatchFrame));
  frame.batch_id        = assigned_gid;
  frame.group_hex_id    = assigned_gid;
  frame.priority        = clamped_prio;
  frame.bypass_warmup   = effective_bypass;
  frame.nested_allowed  = true;
  frame.is_group_scope  = true;
  frame.aborted         = false;
  frame.allocated_bytes = 0;
  frame.egg_count       = 0;
  frame.enter_cycle     = sacculus_rdtsc();

  tl_batch_ctx.depth = slot_idx + 1u;
  sync_thread_execution_state();

  return pack_scope_handle(slot_idx, assigned_gid);
}

void RedjetBatchScopeManager::exit_group(uint64_t scope_handle) {
  uint32_t target_slot = 0u;
  uint32_t token_id    = 0u;
  if (!unpack_scope_handle(scope_handle, target_slot, token_id)) {
    return;
  }
  if (tl_batch_ctx.depth == 0u || target_slot >= tl_batch_ctx.depth) {
    return;
  }
  if (tl_batch_ctx.stack[target_slot].group_hex_id != token_id) {
    return;
  }

  exit_batch(pack_scope_handle(target_slot, tl_batch_ctx.stack[target_slot].batch_id));
}

uint32_t RedjetBatchScopeManager::current_depth() {
  return tl_batch_ctx.depth;
}

uint32_t RedjetBatchScopeManager::current_priority() {
  if (tl_batch_ctx.depth == 0u) {
    return static_cast<uint32_t>(hs_get_thread_group_priority());
  }
  return tl_batch_ctx.stack[tl_batch_ctx.depth - 1u].priority;
}

uint32_t RedjetBatchScopeManager::current_group_hex_id() {
  if (tl_batch_ctx.depth == 0u) {
    return hs_get_thread_group_hex_id();
  }
  return tl_batch_ctx.stack[tl_batch_ctx.depth - 1u].group_hex_id;
}

uint32_t RedjetBatchScopeManager::current_batch_id() {
  if (tl_batch_ctx.depth == 0u) {
    return HS_hotspot_get_current_batch_id();
  }
  return tl_batch_ctx.stack[tl_batch_ctx.depth - 1u].batch_id;
}

bool RedjetBatchScopeManager::is_batch_active() {
  return tl_batch_ctx.depth > 0u;
}

bool RedjetBatchScopeManager::is_bypass_warmup_active() {
  if (tl_batch_ctx.depth == 0u) {
    return hs_get_thread_group_bypass_warmup();
  }
  return tl_batch_ctx.stack[tl_batch_ctx.depth - 1u].bypass_warmup;
}

bool RedjetBatchScopeManager::is_nested_active() {
  return (tl_batch_ctx.depth > 1u) && tl_batch_ctx.stack[tl_batch_ctx.depth - 1u].nested_allowed;
}

const RedjetBatchFrame* RedjetBatchScopeManager::current_frame() {
  if (tl_batch_ctx.depth == 0u) return nullptr;
  return &tl_batch_ctx.stack[tl_batch_ctx.depth - 1u];
}

void RedjetBatchScopeManager::record_allocation(size_t bytes, void* live_hdr) {
#ifndef REDJET_STANDALONE_BUILD
  Thread* cur_thread = Thread::current_or_null();
  (void)RedjetPointCoordinator::controlpoint_check(cur_thread, bytes, live_hdr);
#endif

  if (tl_batch_ctx.depth == 0u) {
    if (live_hdr != nullptr && hs_get_thread_group_bypass_warmup()) {
      auto* hdr = static_cast<HS_hotspot_live_header_t*>(live_hdr);
      promote_live_hdr_bypass_warmup(hdr, hs_get_thread_group_hex_id(), hs_get_thread_group_priority());
    }
    return;
  }

  RedjetBatchFrame& frame = tl_batch_ctx.stack[tl_batch_ctx.depth - 1u];
  frame.allocated_bytes += bytes;

  if (live_hdr == nullptr) {
    return;
  }

  auto* hdr = static_cast<HS_hotspot_live_header_t*>(live_hdr);
  if (hdr->hsMN != HS_LIVE_MAGIC) {
    // Check if caller passed body pointer instead of header pointer
    hdr = HS_hotspot_live_header_t::from_body(live_hdr);
    if (!hdr) return;
  }

  hdr->group_hex_id   = frame.group_hex_id;
  hdr->group_priority = static_cast<uint8_t>(frame.priority);

  if (frame.bypass_warmup) {
    promote_live_hdr_bypass_warmup(hdr, frame.group_hex_id, frame.priority);
  } else {
    hdr->opt_permitted = 1u;
    if (hdr->has_optimized.load(std::memory_order_relaxed) == 0u) {
      if (frame.egg_count < REDJET_MAX_STAGED_EGGS) {
        // Deduplicate within current frame staging buffer
        bool already_staged = false;
        for (uint32_t i = 0u; i < frame.egg_count; ++i) {
          if (frame.staged_eggs[i] == static_cast<void*>(hdr)) {
            already_staged = true;
            break;
          }
        }
        if (!already_staged) {
          frame.staged_eggs[frame.egg_count++] = static_cast<void*>(hdr);
        }
      } else {
        // Staging buffer saturated: flush current 32 staged Egg objects immediately
        (void)batch_optimize_egg_objects(frame.staged_eggs, REDJET_MAX_STAGED_EGGS);
        frame.egg_count = 0u;
        frame.staged_eggs[frame.egg_count++] = static_cast<void*>(hdr);
      }
    }
  }
}

size_t RedjetBatchScopeManager::batch_optimize_egg_objects(void** egg_hdrs, size_t count) {
  if (!egg_hdrs || count == 0u) return 0u;

  RedjetDispatchISA::init();
  const BestHardwareSimdTier best = detect_highest_simd_tier();
  RedjetPitstop* pitstop          = resolve_active_pitstop();
  RedjetMotherline* motherline    = resolve_active_motherline();

  size_t promoted = 0u;
  for (size_t i = 0u; i < count; ++i) {
    void* raw = egg_hdrs[i];
    if (!raw) continue;

    auto* hdr = static_cast<HS_hotspot_live_header_t*>(raw);
    if (hdr->hsMN != HS_LIVE_MAGIC) {
      hdr = HS_hotspot_live_header_t::from_body(raw);
      if (!hdr || hdr->hsMN != HS_LIVE_MAGIC) continue;
    }

    const uint8_t st = hdr->state.load(std::memory_order_acquire);
    if (st != static_cast<uint8_t>(hs_live_state_e::LIVE)) continue;
    if (hdr->opt_permitted == 0u) continue;
    if (hdr->has_optimized.load(std::memory_order_acquire) != 0u) continue;

    uint64_t best_cycles = 0ULL;
    (void)RedjetDispatchISA::table().read_fn_seqlock(hdr->function_id, &best_cycles);
    const uint64_t est_gain = (best_cycles != UINT64_MAX && best_cycles > 0ULL) ? best_cycles : 128ULL;

    // Seed highest verified hardware SIMD tier slot on the Egg before Pitstop morph
    hdr->set_active_simd_candidate(best.isa_name, best.ladder_level);
    const uint16_t target_idx = hdr->active_simd_idx;
    // Revert has_optimized to 0 briefly so Pitstop performs full Egg -> Pitstop -> Fledgling transition
    hdr->has_optimized.store(0u, std::memory_order_relaxed);

    bool morphed = false;
    if (pitstop != nullptr) {
      morphed = pitstop->morph_isa_to_fledgling(hdr, target_idx, est_gain);
    }

    if (!morphed) {
      // Direct fallback promotion when Pitstop instance is detached
      RedjetPointCoordinator::request_object_pause(hdr);
      hdr->active_simd_idx = target_idx;
      for (uint16_t s = 0u; s < hdr->simd_candidate_count && s < 17u; ++s) {
        hdr->simd_table[s].is_active = (s == target_idx) ? 1u : 0u;
      }
      hdr->cycles_gained.store(est_gain, std::memory_order_relaxed);
      hdr->optimization_duration_cycles.store(250ULL, std::memory_order_relaxed);
      hdr->last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
      hdr->opt_success = 1u;
      hdr->has_optimized.store(1u, std::memory_order_release);
      if (hdr->memory_footprint.load(std::memory_order_relaxed) > 0 && !hdr->isTailCanaryValid()) {
        hdr->writeTailCanary();
      }
      RedjetPointCoordinator::release_object_pause(hdr);
      morphed = true;
    }

    if (morphed) {
      if (motherline != nullptr) {
        motherline->promote_to_fledgling(hdr);
        (void)motherline->batch_optimize_egg_objects(hdr->function_id, best.isa_name, target_idx, est_gain);
      }
      ++promoted;
    }
  }

  return promoted;
}

extern "C" {

uint64_t redjet_rt_batch_enter(uint32_t priority, bool bypass_warmup) {
  return RedjetBatchScopeManager::enter_batch(priority, bypass_warmup, true);
}

void redjet_rt_batch_exit(uint64_t handle) {
  RedjetBatchScopeManager::exit_batch(handle);
}

void redjet_rt_batch_abort(uint64_t handle) {
  RedjetBatchScopeManager::abort_batch(handle);
}

uint64_t redjet_rt_group_enter(uint32_t group_hex_id, uint32_t priority, bool bypass_warmup) {
  return RedjetBatchScopeManager::enter_group(group_hex_id, priority, bypass_warmup);
}

void redjet_rt_group_exit(uint64_t handle) {
  RedjetBatchScopeManager::exit_group(handle);
}

JNIEXPORT jlong JNICALL Java_jdk_gc_redjet_BatchScope_enterBatch0(JNIEnv* env, jclass clazz, jint priority, jboolean bypass_warmup) {
  (void)env;
  (void)clazz;
  return static_cast<jlong>(RedjetBatchScopeManager::enter_batch(static_cast<uint32_t>(priority), bypass_warmup != 0, true));
}

JNIEXPORT void JNICALL Java_jdk_gc_redjet_BatchScope_exitBatch0(JNIEnv* env, jclass clazz, jlong handle) {
  (void)env;
  (void)clazz;
  RedjetBatchScopeManager::exit_batch(static_cast<uint64_t>(handle));
}

JNIEXPORT void JNICALL Java_jdk_gc_redjet_BatchScope_abortBatch0(JNIEnv* env, jclass clazz, jlong handle) {
  (void)env;
  (void)clazz;
  RedjetBatchScopeManager::abort_batch(static_cast<uint64_t>(handle));
}

} // extern "C"
