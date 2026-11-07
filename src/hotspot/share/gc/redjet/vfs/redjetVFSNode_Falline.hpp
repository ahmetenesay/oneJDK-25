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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_FALLINE_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_FALLINE_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// Runway Falline: Pretrashing Morgue Queue Node (PLAN.md Section 9.1 & 9.4)
// Two-stage morgue quarantine with 1-epoch retention window
// Prevents premature collection disasters via dynamic resurrection/rehabilitation
// ============================================================================

enum class RedjetFallineSlotState : uint8_t {
  MORGUE_EMPTY          = 0,
  MORGUE_PROBATION      = 1, // Quarantined, awaiting 1-epoch retention
  MORGUE_CONDEMNED      = 2, // 1-epoch elapsed, verified dead, ready for Landfill
  MORGUE_REHABILITATED  = 3, // Resurrected by mutator access; returned to runway
  MORGUE_SWEPT          = 4  // Memory uncommitted or recycled
};

struct alignas(64) RedjetFallineMorgueSlot {
  std::atomic<void*>   dead_oop{ nullptr };
  size_t               object_size{ 0 };
  uint32_t             originating_thread_id{ 0 };
  uint32_t             region_id{ 0 };
  uint64_t             quarantine_timestamp_tsc{ 0 };
  uint64_t             admission_epoch{ 0 };
  std::atomic<uint8_t> state{ static_cast<uint8_t>(RedjetFallineSlotState::MORGUE_EMPTY) };
  std::atomic<bool>    resurrected{ false };
};

class RedjetVFSNode_Falline : public RedjetVFSNode {
public:
  static constexpr size_t MORGUE_CAPACITY = 16384;
  static constexpr size_t MORGUE_MASK = MORGUE_CAPACITY - 1;
  static constexpr uint64_t RETENTION_WINDOW_EPOCHS = 1; // 1-epoch retention window
  static constexpr size_t DEFAULT_QUOTA = 128ULL * 1024 * 1024; // 128 MB

private:
  RedjetFallineMorgueSlot           _morgue_ring[MORGUE_CAPACITY];
  alignas(64) std::atomic<uint64_t> _head_cursor{ 0 };
  alignas(64) std::atomic<uint64_t> _tail_cursor{ 0 };
  alignas(64) std::atomic<uint64_t> _falline_epoch{ 1 };

  std::atomic<uint64_t> _total_quarantined{ 0 };
  std::atomic<uint64_t> _total_condemned{ 0 };
  std::atomic<uint64_t> _total_rehabilitated{ 0 };
  std::atomic<uint64_t> _total_drained_bytes{ 0 };
  std::atomic<uint64_t> _quarantine_drops_on_full{ 0 };

public:
  RedjetVFSNode_Falline(const char* name = "falline",
                        const char* full_path = "/heap/on_heap/runway/falline",
                        RedjetVFSNode* parent = nullptr,
                        size_t quota_bytes = DEFAULT_QUOTA);
  virtual ~RedjetVFSNode_Falline();

  // 1. Quarantines an object received from RPSS or mutator clasp for 1-epoch
  RJ_API bool quarantine_object(void* oop, size_t object_size,
                                uint32_t thread_id, uint32_t region_id);

  // 2. Rehabilitation: Resurrects a quarantined object if accessed during retention window
  RJ_API bool attempt_rehabilitation(void* oop);

  // 3. Advances quarantine epoch: Transitions entries older than 1 epoch to CONDEMNED
  RJ_API size_t advance_quarantine_epoch();

  // 4. Drains condemned objects to Landfill for uncommit or page recycling
  RJ_API size_t drain_condemned_to_landfill(void** out_oops, size_t max_oops, size_t* out_drained_bytes);

  // 5. Purges morgue contents
  RJ_API void purge_morgue();

  // Sizing & Telemetry
  size_t   morgue_capacity() const noexcept { return MORGUE_CAPACITY; }
  size_t   active_quarantine_count() const noexcept;
  uint64_t current_epoch() const noexcept { return _falline_epoch.load(std::memory_order_relaxed); }
  uint64_t total_quarantined() const noexcept { return _total_quarantined.load(std::memory_order_relaxed); }
  uint64_t total_condemned() const noexcept { return _total_condemned.load(std::memory_order_relaxed); }
  uint64_t total_rehabilitated() const noexcept { return _total_rehabilitated.load(std::memory_order_relaxed); }
  uint64_t total_drained_bytes() const noexcept { return _total_drained_bytes.load(std::memory_order_relaxed); }
  uint64_t drops_on_full() const noexcept { return _quarantine_drops_on_full.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_FALLINE_HPP
