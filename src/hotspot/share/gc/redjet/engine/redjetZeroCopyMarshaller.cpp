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

#include "gc/redjet/engine/redjetZeroCopyMarshaller.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetZeroCopyMarshaller* RedjetZeroCopyMarshaller::_instance = nullptr;
std::atomic<bool> RedjetZeroCopyMarshaller::_initialized{ false };

RedjetZeroCopyMarshaller::RedjetZeroCopyMarshaller()
  : _park_arena(nullptr),
    _next_lease_id(1),
    _total_leases_granted(0),
    _total_leases_released(0),
    _total_zero_copy_bytes(0),
    _total_kernel_io_completions(0) {
  hs_ticket_lock_init(&_marshaller_lock);
  std::memset(_leases, 0, sizeof(_leases));
}

RedjetZeroCopyMarshaller::~RedjetZeroCopyMarshaller() {
  hs_ticket_lock_acquire(&_marshaller_lock);
  for (size_t i = 0; i < MAX_CONCURRENT_LEASES; ++i) {
    if (_leases[i].buffer_address != nullptr && _leases[i].state == RedjetBufferLeaseState::LEASED) {
#if defined(_WIN32)
      VirtualFree(_leases[i].buffer_address, 0, MEM_RELEASE);
#else
      munmap(_leases[i].buffer_address, _leases[i].capacity_bytes);
#endif
      _leases[i].buffer_address = nullptr;
    }
  }
  hs_ticket_lock_release(&_marshaller_lock);
}

void RedjetZeroCopyMarshaller::initialize(RedjetVFSNode_ParkArena* park_arena) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetZeroCopyMarshaller();
    _instance->set_park_arena(park_arena);
  }
}

void RedjetZeroCopyMarshaller::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

RedjetNativeBufferLease* RedjetZeroCopyMarshaller::acquire_buffer_lease(size_t required_bytes, uint32_t thread_id) {
  if (required_bytes == 0) return nullptr;

  size_t aligned_size = (required_bytes + 4095) & ~static_cast<size_t>(4095);

  hs_ticket_lock_acquire(&_marshaller_lock);

  // Find free lease slot
  RedjetNativeBufferLease* lease = nullptr;
  for (size_t i = 0; i < MAX_CONCURRENT_LEASES; ++i) {
    if (_leases[i].state == RedjetBufferLeaseState::AVAILABLE) {
      lease = &_leases[i];
      break;
    }
  }

  if (lease == nullptr) {
    hs_ticket_lock_release(&_marshaller_lock);
    return nullptr; // Maximum concurrent leases reached
  }

  void* buf = nullptr;
  if (_park_arena != nullptr) {
    buf = _park_arena->allocate_pinned(aligned_size);
  }

  if (buf == nullptr) {
#if defined(_WIN32)
    buf = VirtualAlloc(nullptr, aligned_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    buf = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) buf = nullptr;
#endif
  }

  if (buf == nullptr) {
    hs_ticket_lock_release(&_marshaller_lock);
    return nullptr;
  }

  lease->buffer_address = buf;
  lease->capacity_bytes = aligned_size;
  lease->bytes_written = 0;
  lease->lease_id = _next_lease_id.fetch_add(1, std::memory_order_relaxed);
  lease->thread_id = thread_id;
  lease->state = RedjetBufferLeaseState::LEASED;
  lease->lease_tsc = hs_read_tsc();
  lease->is_kernel_mapped = true;

  _total_leases_granted.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_marshaller_lock);
  return lease;
}

bool RedjetZeroCopyMarshaller::release_buffer_lease(uint32_t lease_id) {
  if (lease_id == 0) return false;

  hs_ticket_lock_acquire(&_marshaller_lock);

  RedjetNativeBufferLease* lease = nullptr;
  for (size_t i = 0; i < MAX_CONCURRENT_LEASES; ++i) {
    if (_leases[i].lease_id == lease_id && _leases[i].state == RedjetBufferLeaseState::LEASED) {
      lease = &_leases[i];
      break;
    }
  }

  if (lease == nullptr) {
    hs_ticket_lock_release(&_marshaller_lock);
    return false;
  }

  void* addr = lease->buffer_address;
  size_t sz = lease->capacity_bytes;

  if (_park_arena != nullptr) {
    _park_arena->release_pinned(addr);
  } else {
#if defined(_WIN32)
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, sz);
#endif
  }

  lease->buffer_address = nullptr;
  lease->capacity_bytes = 0;
  lease->bytes_written = 0;
  lease->state = RedjetBufferLeaseState::AVAILABLE;
  lease->is_kernel_mapped = false;

  _total_leases_released.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_marshaller_lock);
  return true;
}

bool RedjetZeroCopyMarshaller::complete_kernel_io(uint32_t lease_id, size_t bytes_transferred) {
  if (lease_id == 0) return false;

  hs_ticket_lock_acquire(&_marshaller_lock);

  RedjetNativeBufferLease* lease = nullptr;
  for (size_t i = 0; i < MAX_CONCURRENT_LEASES; ++i) {
    if (_leases[i].lease_id == lease_id && _leases[i].state == RedjetBufferLeaseState::LEASED) {
      lease = &_leases[i];
      break;
    }
  }

  if (lease == nullptr) {
    hs_ticket_lock_release(&_marshaller_lock);
    return false;
  }

  lease->bytes_written = std::min(bytes_transferred, lease->capacity_bytes);
  _total_zero_copy_bytes.fetch_add(lease->bytes_written, std::memory_order_relaxed);
  _total_kernel_io_completions.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_marshaller_lock);
  return true;
}

bool RedjetZeroCopyMarshaller::is_buffer_pinned(const void* ptr) const noexcept {
  if (ptr == nullptr) return false;

  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

  for (size_t i = 0; i < MAX_CONCURRENT_LEASES; ++i) {
    if (_leases[i].state == RedjetBufferLeaseState::LEASED && _leases[i].buffer_address != nullptr) {
      uintptr_t base = reinterpret_cast<uintptr_t>(_leases[i].buffer_address);
      if (addr >= base && addr < base + _leases[i].capacity_bytes) {
        return true;
      }
    }
  }

  return false;
}

void RedjetZeroCopyMarshaller::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Zero-Copy Native Marshaller ---");
  st->print_cr("  Max Leases:           %zu", MAX_CONCURRENT_LEASES);
  st->print_cr("  Total Leases Granted: %llu", static_cast<unsigned long long>(_total_leases_granted.load(std::memory_order_relaxed)));
  st->print_cr("  Total Leases Released:%llu", static_cast<unsigned long long>(_total_leases_released.load(std::memory_order_relaxed)));
  st->print_cr("  Zero-Copy Bytes:      %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_zero_copy_bytes.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_zero_copy_bytes.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  Kernel IO Completions:%llu", static_cast<unsigned long long>(_total_kernel_io_completions.load(std::memory_order_relaxed)));
}
