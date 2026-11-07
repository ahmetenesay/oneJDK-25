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

#include "gc/redjet/vfs/redjetVFSNode_ReservedBoot.hpp"
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

RedjetVFSNode_ReservedBoot::RedjetVFSNode_ReservedBoot(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  // Allocate 64MB dedicated emergency buffer
#if defined(_WIN32)
  _reserve_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, RESERVED_BOOT_SIZE,
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
  _reserve_base = static_cast<uint8_t*>(mmap(nullptr, RESERVED_BOOT_SIZE,
                                             PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (_reserve_base == MAP_FAILED) _reserve_base = nullptr;
#endif

  if (_reserve_base != nullptr) {
    _borrow_cursor.store(_reserve_base, std::memory_order_relaxed);
    _reserve_end = _reserve_base + RESERVED_BOOT_SIZE;
    add_used(RESERVED_BOOT_SIZE);
    lock_physical_reserve();
  }
}

RedjetVFSNode_ReservedBoot::~RedjetVFSNode_ReservedBoot() {
  unlock_physical_reserve();

  if (_reserve_base != nullptr) {
#if defined(_WIN32)
    VirtualFree(_reserve_base, 0, MEM_RELEASE);
#else
    munmap(_reserve_base, RESERVED_BOOT_SIZE);
#endif
    _reserve_base = nullptr;
    _borrow_cursor.store(nullptr, std::memory_order_relaxed);
    _reserve_end = nullptr;
  }
}

bool RedjetVFSNode_ReservedBoot::lock_physical_reserve() {
  if (_reserve_base == nullptr) return false;

#if defined(_WIN32)
  BOOL res = VirtualLock(_reserve_base, RESERVED_BOOT_SIZE);
  _is_memory_locked.store(res != FALSE, std::memory_order_release);
#else
  int res = mlock(_reserve_base, RESERVED_BOOT_SIZE);
  _is_memory_locked.store(res == 0, std::memory_order_release);
#endif

  _reserve_state.store(static_cast<uint8_t>(RedjetReserveState::RESERVE_LOCKED_PRISTINE), std::memory_order_release);
  return _is_memory_locked.load(std::memory_order_relaxed);
}

bool RedjetVFSNode_ReservedBoot::unlock_physical_reserve() {
  if (_reserve_base == nullptr || !_is_memory_locked.load(std::memory_order_relaxed)) {
    return false;
  }

#if defined(_WIN32)
  VirtualUnlock(_reserve_base, RESERVED_BOOT_SIZE);
#else
  munlock(_reserve_base, RESERVED_BOOT_SIZE);
#endif

  _is_memory_locked.store(false, std::memory_order_release);
  _reserve_state.store(static_cast<uint8_t>(RedjetReserveState::RESERVE_UNLOCKED), std::memory_order_release);
  return true;
}

void* RedjetVFSNode_ReservedBoot::borrow_emergency_headroom(size_t requested_bytes) {
  if (_reserve_base == nullptr || requested_bytes == 0) return nullptr;

  size_t aligned = (requested_bytes + 7) & ~static_cast<size_t>(7);
  uint8_t* cur = _borrow_cursor.load(std::memory_order_relaxed);

  while (cur + aligned <= _reserve_end) {
    if (_borrow_cursor.compare_exchange_weak(cur, cur + aligned,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      _emergency_invocations.fetch_add(1, std::memory_order_relaxed);
      _total_bytes_borrowed.fetch_add(aligned, std::memory_order_relaxed);
      _reserve_state.store(static_cast<uint8_t>(RedjetReserveState::RESERVE_EMERGENCY_ACTIVE), std::memory_order_release);
      return cur;
    }
  }

  _reserve_state.store(static_cast<uint8_t>(RedjetReserveState::RESERVE_EXHAUSTED), std::memory_order_release);
  return nullptr; // Reserve exhausted
}

void RedjetVFSNode_ReservedBoot::restore_emergency_headroom() {
  if (_reserve_base == nullptr) return;

  hs_ticket_guard_t guard(_reserve_lock);
  _borrow_cursor.store(_reserve_base, std::memory_order_release);
  _reserve_restorations.fetch_add(1, std::memory_order_relaxed);
  _reserve_state.store(static_cast<uint8_t>(RedjetReserveState::RESERVE_LOCKED_PRISTINE), std::memory_order_release);
}

size_t RedjetVFSNode_ReservedBoot::remaining_headroom() const noexcept {
  if (_reserve_base == nullptr || _reserve_end == nullptr) return 0;
  uint8_t* cur = _borrow_cursor.load(std::memory_order_relaxed);
  return (cur < _reserve_end) ? static_cast<size_t>(_reserve_end - cur) : 0;
}

void RedjetVFSNode_ReservedBoot::print_on(outputStream* st) const {
  st->print_cr("Redjet Reserved Boot VFS Node [%s]:", full_path());
  st->print_cr("  Physical RAM Lock: %s, Remaining Headroom: %zu KB / 65536 KB",
               is_locked() ? "LOCKED" : "UNLOCKED", remaining_headroom() / 1024);
  st->print_cr("  Emergency Invocations: " UINT64_FORMAT ", Total Borrowed: " UINT64_FORMAT " bytes",
               emergency_invocations(), total_borrowed_bytes());
  st->print_cr("  Reserve Restorations: " UINT64_FORMAT, reserve_restorations());
}
