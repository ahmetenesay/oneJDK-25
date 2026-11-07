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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_RPSS_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_RPSS_HPP

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

/* ========================================================================= */
/* 1. RPSS SIGNAL & DEAD REFERENCE QUEUE DATA STRUCTURES                     */
/* ========================================================================= */

enum class RedjetRPSSSignalState : uint8_t {
  RPSS_SLOT_EMPTY       = 0,
  RPSS_SIGNAL_PROBATION = 1,
  RPSS_SIGNAL_CONDEMNED = 2,
  RPSS_SIGNAL_CANCELLED = 3,
  RPSS_SIGNAL_RECLAIMED = 4
};

struct alignas(64) RedjetRPSSHarbingerEntry {
  std::atomic<uintptr_t> dead_oop{ 0 };
  uint32_t               thread_id{ 0 };
  uint32_t               region_id{ 0 };
  size_t                 object_size{ 0 };
  uint64_t               enqueue_timestamp_ns{ 0 };
  uint64_t               probation_epoch{ 0 };
  std::atomic<uint8_t>   state{ static_cast<uint8_t>(RedjetRPSSSignalState::RPSS_SLOT_EMPTY) };
  std::atomic<bool>      resurrected{ false };
};

struct RedjetRPSSDrainStats {
  size_t   signals_drained{ 0 };
  size_t   objects_condemned{ 0 };
  size_t   false_positives_cancelled{ 0 };
  size_t   reclaimed_bytes{ 0 };
};

/* ========================================================================= */
/* 2. RPSS VFS NODE CONTROLLER (/heap/on_heap/rpss)                          */
/* ========================================================================= */

class RedjetVFSNode_RPSS : public RedjetVFSNode {
public:
  static constexpr size_t RING_CAPACITY = 8192;
  static constexpr size_t RING_MASK = RING_CAPACITY - 1;
  static constexpr uint64_t PROBATION_DELAY_EPOCHS = 2;

private:
  RedjetRPSSHarbingerEntry            _ring[RING_CAPACITY];
  alignas(64) std::atomic<uint64_t>   _head{ 0 };
  alignas(64) std::atomic<uint64_t>   _tail{ 0 };
  alignas(64) std::atomic<uint64_t>   _current_epoch{ 1 };

  // Telemetry
  std::atomic<uint64_t> _total_enqueued{ 0 };
  std::atomic<uint64_t> _total_committed{ 0 };
  std::atomic<uint64_t> _total_cancelled{ 0 };
  std::atomic<uint64_t> _total_reclaimed_bytes{ 0 };
  std::atomic<uint64_t> _total_drops_on_full{ 0 };

public:
  RedjetVFSNode_RPSS(const char* name = "rpss",
                     const char* full_path = "/heap/on_heap/rpss",
                     RedjetVFSNode* parent = nullptr,
                     size_t quota_bytes = 64 * 1024 * 1024);
  virtual ~RedjetVFSNode_RPSS();

  // Enqueue a dead reference harbinger signal from mutator or barrier
  RJ_API bool enqueue_dead_ref(uintptr_t dead_oop, uint32_t thread_id, uint32_t region_id, size_t object_size);

  // Check and cancel harbinger signal if object was accessed or escaped
  RJ_API bool notify_object_escaped(uintptr_t oop_addr);

  // Advance probation epoch and evaluate signals ready for irreversible reclamation
  RJ_API void advance_epoch();

  // Drain condemned signals ready to be transitioned to Landfill / swept
  RJ_API RedjetRPSSDrainStats drain_condemned_batch(uintptr_t* out_dead_oops, size_t max_count);

  // Clear and purge all slots
  RJ_API void purge();

  // Sizing & Telemetry
  size_t ring_capacity() const noexcept { return RING_CAPACITY; }
  size_t active_signals() const noexcept;
  uint64_t current_epoch() const noexcept { return _current_epoch.load(std::memory_order_relaxed); }
  uint64_t total_enqueued() const noexcept { return _total_enqueued.load(std::memory_order_relaxed); }
  uint64_t total_committed() const noexcept { return _total_committed.load(std::memory_order_relaxed); }
  uint64_t total_cancelled() const noexcept { return _total_cancelled.load(std::memory_order_relaxed); }
  uint64_t total_reclaimed_bytes() const noexcept { return _total_reclaimed_bytes.load(std::memory_order_relaxed); }
  uint64_t total_drops_on_full() const noexcept { return _total_drops_on_full.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_RPSS_HPP
