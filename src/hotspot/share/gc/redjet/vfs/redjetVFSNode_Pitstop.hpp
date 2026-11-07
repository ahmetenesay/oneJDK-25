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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_PITSTOP_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_PITSTOP_HPP

#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <atomic>
#include <cstdint>
#include <cstddef>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// Forward declarations
class RedjetRunway;
class RedjetVFSNode_Hotfield;

/* ========================================================================= */
/* 1. PITSTOP MORPH MODES & ENTRY STATE TYPES                                */
/* ========================================================================= */

enum class RedjetPitstopMorphMode : uint8_t {
  EXCISION_EXCLUSIVE = 0, // Object pulled from live mutators while morphing
  SHADOW_RCU         = 1  // Non-blocking shadow copy executed during duel
};

enum class RedjetPitstopEntryState : uint8_t {
  STAGED_FOR_PUZZLER = 0,
  PROFILING_ACTIVE   = 1,
  DUEL_IN_PROGRESS   = 2,
  MORPHING_APPLIED   = 3,
  HOTSWAP_VERIFYING  = 4,
  READY_TO_RETURN    = 5,
  RETURNED_TO_RUNWAY = 6,
  ABORTED            = 7
};

struct alignas(64) RedjetPitstopMemSlot {
  void*                    original_slot_addr{ nullptr };
  size_t                   slot_bytes{ 0 };
  void*                    shadow_buffer{ nullptr };
  size_t                   shadow_bytes{ 0 };
  uint32_t                 duel_id{ 0 };
  uint32_t                 generation{ 0 };
  uint32_t                 region_id{ 0 };
  RedjetPitstopMorphMode   morph_mode{ RedjetPitstopMorphMode::SHADOW_RCU };
  std::atomic<uint8_t>     state{ static_cast<uint8_t>(RedjetPitstopEntryState::STAGED_FOR_PUZZLER) };

  uint64_t                 entry_tsc{ 0 };
  uint64_t                 baseline_cycles{ 0 };
  uint64_t                 candidate_cycles{ 0 };
  uint64_t                 cycles_gained{ 0 };
  uint16_t                 winning_simd_idx{ 0 };

  RedjetPitstopMemSlot*    next{ nullptr };
  RedjetPitstopMemSlot*    prev{ nullptr };
};

/* ========================================================================= */
/* 2. PITSTOP VFS NODE CONTROLLER (/heap/on_heap/runway/pitstop)             */
/* ========================================================================= */

class RedjetVFSNode_Pitstop : public RedjetVFSNode {
public:
  static constexpr size_t DEFAULT_PITSTOP_QUOTA = 64ULL * 1024 * 1024; // 64 MB

private:
  RedjetPitstopMemSlot* _slots_head{ nullptr };
  RedjetPitstopMemSlot* _slots_tail{ nullptr };
  hs_ticket_lock_t      _slot_lock;

  // Telemetry
  std::atomic<uint64_t> _total_admissions{ 0 };
  std::atomic<uint64_t> _total_duels_completed{ 0 };
  std::atomic<uint64_t> _total_duels_won{ 0 };
  std::atomic<uint64_t> _total_cycles_saved{ 0 };
  std::atomic<uint64_t> _total_returns_dispatched{ 0 };
  std::atomic<uint64_t> _total_aborts{ 0 };
  std::atomic<size_t>   _active_slots_count{ 0 };

public:
  RedjetVFSNode_Pitstop(const char* name = "pitstop",
                        const char* full_path = "/heap/on_heap/runway/pitstop",
                        RedjetVFSNode* parent = nullptr,
                        size_t quota_bytes = DEFAULT_PITSTOP_QUOTA);
  virtual ~RedjetVFSNode_Pitstop();

  // Admits an object slot into pitstop for Puzzler optimization
  RJ_API RedjetPitstopMemSlot* admit_slot(void* original_slot, size_t slot_bytes,
                                          uint32_t duel_id, uint32_t generation,
                                          uint32_t region_id, RedjetPitstopMorphMode mode);

  // Transitions entry state
  RJ_API bool update_slot_state(void* original_slot, RedjetPitstopEntryState new_state);

  // Records completion of a Puzzler duel and updates telemetry
  RJ_API bool record_duel_result(void* original_slot, uint16_t winning_simd_idx,
                                 uint64_t baseline_cyc, uint64_t candidate_cyc);

  // Dispatches return of an optimized object back to its Runway / Hotfield origin
  RJ_API bool return_slot_to_origin(void* original_slot, RedjetRunway* runway = nullptr,
                                    RedjetVFSNode_Hotfield* hotfield = nullptr);

  // Aborts optimization and safely resets slot
  RJ_API bool abort_and_release(void* original_slot);

  // Query entry state
  RJ_API RedjetPitstopEntryState get_slot_state(const void* original_slot) const noexcept;

  // Sizing & Telemetry accessors
  size_t active_slots() const noexcept { return _active_slots_count.load(std::memory_order_relaxed); }
  uint64_t total_admissions() const noexcept { return _total_admissions.load(std::memory_order_relaxed); }
  uint64_t duels_completed()  const noexcept { return _total_duels_completed.load(std::memory_order_relaxed); }
  uint64_t duels_won()        const noexcept { return _total_duels_won.load(std::memory_order_relaxed); }
  uint64_t cycles_saved()     const noexcept { return _total_cycles_saved.load(std::memory_order_relaxed); }
  uint64_t returns_dispatched() const noexcept { return _total_returns_dispatched.load(std::memory_order_relaxed); }
  uint64_t aborts()           const noexcept { return _total_aborts.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_PITSTOP_HPP
