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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_RESERVEDBOOT_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_RESERVEDBOOT_HPP

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
// Reserved Boot VFS Node Controller (/heap/off_heap/reserved_boot) (PLAN.md Sec 9.1 & 8.5)
// 64 MB locked emergency margin preventing hard JVM crashes during OOM spikes
// Physical memory locking (VirtualLock / mlock) with emergency headroom borrowing
// ============================================================================

enum class RedjetReserveState : uint8_t {
  RESERVE_LOCKED_PRISTINE  = 0, // 64 MB fully locked in physical RAM, ready
  RESERVE_EMERGENCY_ACTIVE = 1, // Headroom borrowed by mutator/GC emergency
  RESERVE_EXHAUSTED        = 2, // Full 64 MB margin consumed
  RESERVE_UNLOCKED         = 3
};

class RedjetVFSNode_ReservedBoot : public RedjetVFSNode {
public:
  static constexpr size_t RESERVED_BOOT_SIZE = 64ULL * 1024 * 1024; // 64 MB emergency buffer

private:
  uint8_t*                          _reserve_base{ nullptr };
  alignas(64) std::atomic<uint8_t*> _borrow_cursor{ nullptr };
  uint8_t*                          _reserve_end{ nullptr };
  std::atomic<bool>                 _is_memory_locked{ false };
  std::atomic<uint8_t>              _reserve_state{ static_cast<uint8_t>(RedjetReserveState::RESERVE_UNLOCKED) };
  hs_ticket_lock_t                  _reserve_lock;

  // Telemetry
  std::atomic<uint64_t> _emergency_invocations{ 0 };
  std::atomic<uint64_t> _total_bytes_borrowed{ 0 };
  std::atomic<uint64_t> _reserve_restorations{ 0 };

public:
  RedjetVFSNode_ReservedBoot(const char* name = "reserved_boot",
                             const char* full_path = "/heap/off_heap/reserved_boot",
                             RedjetVFSNode* parent = nullptr,
                             size_t quota_bytes = RESERVED_BOOT_SIZE);
  virtual ~RedjetVFSNode_ReservedBoot();

  // 1. Locks the 64MB emergency buffer into physical RAM (VirtualLock / mlock)
  RJ_API bool lock_physical_reserve();

  // 2. Unlocks the reserve (VirtualUnlock / munlock)
  RJ_API bool unlock_physical_reserve();

  // 3. Emergency Borrow: allocates slice from 64MB reserve during OOM panic
  RJ_API void* borrow_emergency_headroom(size_t requested_bytes);

  // 4. Restores 64MB reserve to pristine state after GC reclamation completes
  RJ_API void restore_emergency_headroom();

  // Sizing & Status
  bool     is_locked()             const noexcept { return _is_memory_locked.load(std::memory_order_relaxed); }
  uint64_t emergency_invocations() const noexcept { return _emergency_invocations.load(std::memory_order_relaxed); }
  uint64_t total_borrowed_bytes()  const noexcept { return _total_bytes_borrowed.load(std::memory_order_relaxed); }
  uint64_t reserve_restorations()  const noexcept { return _reserve_restorations.load(std::memory_order_relaxed); }
  size_t   remaining_headroom()    const noexcept;

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_RESERVEDBOOT_HPP
