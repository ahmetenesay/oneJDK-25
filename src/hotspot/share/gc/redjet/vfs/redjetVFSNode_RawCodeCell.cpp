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

#include "gc/redjet/vfs/redjetVFSNode_RawCodeCell.hpp"
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

RedjetVFSNode_RawCodeCell::RedjetVFSNode_RawCodeCell(const char* name,
                                                     const char* full_path,
                                                     RedjetVFSNode* parent,
                                                     size_t quota_bytes)
  : RedjetVFSNode(name, full_path, parent, quota_bytes),
    _cell_count(0),
    _vac_read_key(0),
    _total_cells_allocated(0),
    _total_bytes_sealed(0),
    _total_seals_verified(0),
    _total_verification_failures(0),
    _total_cells_released(0) {
  hs_ticket_lock_init(&_cell_lock);
  std::memset(_cells, 0, sizeof(_cells));
}

RedjetVFSNode_RawCodeCell::~RedjetVFSNode_RawCodeCell() {
  destroy();
}

void RedjetVFSNode_RawCodeCell::init(uint64_t vac_read_key) {
  hs_ticket_lock_acquire(&_cell_lock);
  _vac_read_key = (vac_read_key != 0) ? vac_read_key : 0x7E3A9F10B24D85C3ULL;
  _cell_count = 0;
  std::memset(_cells, 0, sizeof(_cells));
  set_used_bytes(0);
  hs_ticket_lock_release(&_cell_lock);
}

void RedjetVFSNode_RawCodeCell::destroy() {
  hs_ticket_lock_acquire(&_cell_lock);

  for (size_t i = 0; i < _cell_count; ++i) {
    if (_cells[i] != nullptr) {
      size_t total_size = _cells[i]->allocated_size;
#if defined(_WIN32)
      VirtualFree(_cells[i], 0, MEM_RELEASE);
#else
      munmap(_cells[i], total_size);
#endif
      _cells[i] = nullptr;
    }
  }

  _cell_count = 0;
  set_used_bytes(0);
  hs_ticket_lock_release(&_cell_lock);
}

RedjetRawCodeCellHeader* RedjetVFSNode_RawCodeCell::allocate_cell(uint32_t class_id,
                                                                 uint32_t method_id,
                                                                 const uint8_t* bytecode,
                                                                 size_t size) {
  if (bytecode == nullptr || size == 0) {
    return nullptr;
  }

  size_t total_needed = sizeof(RedjetRawCodeCellHeader) + size;
  // Align up to 4KB page boundary
  size_t page_aligned = (total_needed + 4095) & ~static_cast<size_t>(4095);

  hs_ticket_lock_acquire(&_cell_lock);

  if (_cell_count >= MAX_CELLS_TRACKED) {
    hs_ticket_lock_release(&_cell_lock);
    return nullptr;
  }

  if (used_bytes() + page_aligned > quota_bytes() && quota_bytes() > 0) {
    hs_ticket_lock_release(&_cell_lock);
    return nullptr; // Quota exceeded
  }

  void* mem = nullptr;
#if defined(_WIN32)
  mem = VirtualAlloc(nullptr, page_aligned, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  mem = mmap(nullptr, page_aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    mem = nullptr;
  }
#endif

  if (mem == nullptr) {
    hs_ticket_lock_release(&_cell_lock);
    return nullptr;
  }

  RedjetRawCodeCellHeader* header = reinterpret_cast<RedjetRawCodeCellHeader*>(mem);
  header->method_id = method_id;
  header->class_id = class_id;
  header->bytecode_size = size;
  header->allocated_size = page_aligned;
  header->crypto_seal = 0;
  header->timestamp_tsc = hs_read_tsc();
  header->is_sealed = false;
  std::memset(header->padding, 0, sizeof(header->padding));

  // Copy bytecode payload
  std::memcpy(header->bytecode_data(), bytecode, size);

  _cells[_cell_count++] = header;
  _total_cells_allocated.fetch_add(1, std::memory_order_relaxed);
  set_used_bytes(used_bytes() + page_aligned);

  hs_ticket_lock_release(&_cell_lock);
  return header;
}

bool RedjetVFSNode_RawCodeCell::seal_cell(RedjetRawCodeCellHeader* cell, uint64_t vac_key) {
  if (cell == nullptr || cell->is_sealed) {
    return false;
  }

  uint64_t effective_key = (vac_key != 0) ? vac_key : _vac_read_key;

  // Compute 7-key cryptographic digest over header metadata and bytecode data
  uint64_t digest = 0xC1A2B3C4D5E6F708ULL;
  digest = RedjetVFSCrypto::poly_eval_step(digest, cell->method_id);
  digest = RedjetVFSCrypto::poly_eval_step(digest, cell->class_id);
  digest = RedjetVFSCrypto::poly_eval_step(digest, cell->bytecode_size);
  digest = RedjetVFSCrypto::poly_eval_step(digest, effective_key);

  const uint8_t* bc = reinterpret_cast<const uint8_t*>(cell->bytecode_data());
  for (size_t i = 0; i < cell->bytecode_size; ++i) {
    digest = (digest * 6364136223846793005ULL) + bc[i];
  }

  cell->crypto_seal = digest;
  cell->is_sealed = true;

  // Protect memory as Read-Only (W^X enforcement)
#if defined(_WIN32)
  DWORD old_protect;
  if (!VirtualProtect(cell, cell->allocated_size, PAGE_READONLY, &old_protect)) {
    return false;
  }
#else
  if (mprotect(cell, cell->allocated_size, PROT_READ) != 0) {
    return false;
  }
#endif

  _total_bytes_sealed.fetch_add(cell->bytecode_size, std::memory_order_relaxed);
  return true;
}

bool RedjetVFSNode_RawCodeCell::verify_cell_seal(const RedjetRawCodeCellHeader* cell, uint64_t vac_key) noexcept {
  if (cell == nullptr || !cell->is_sealed) {
    _total_verification_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint64_t effective_key = (vac_key != 0) ? vac_key : _vac_read_key;

  uint64_t digest = 0xC1A2B3C4D5E6F708ULL;
  digest = RedjetVFSCrypto::poly_eval_step(digest, cell->method_id);
  digest = RedjetVFSCrypto::poly_eval_step(digest, cell->class_id);
  digest = RedjetVFSCrypto::poly_eval_step(digest, cell->bytecode_size);
  digest = RedjetVFSCrypto::poly_eval_step(digest, effective_key);

  const uint8_t* bc = reinterpret_cast<const uint8_t*>(cell->bytecode_data());
  for (size_t i = 0; i < cell->bytecode_size; ++i) {
    digest = (digest * 6364136223846793005ULL) + bc[i];
  }

  bool match = (digest == cell->crypto_seal);
  if (match) {
    _total_seals_verified.fetch_add(1, std::memory_order_relaxed);
  } else {
    _total_verification_failures.fetch_add(1, std::memory_order_relaxed);
  }
  return match;
}

bool RedjetVFSNode_RawCodeCell::release_cell(RedjetRawCodeCellHeader* cell) {
  if (cell == nullptr) {
    return false;
  }

  hs_ticket_lock_acquire(&_cell_lock);

  size_t target_idx = MAX_CELLS_TRACKED;
  for (size_t i = 0; i < _cell_count; ++i) {
    if (_cells[i] == cell) {
      target_idx = i;
      break;
    }
  }

  if (target_idx == MAX_CELLS_TRACKED) {
    hs_ticket_lock_release(&_cell_lock);
    return false; // Not found
  }

  size_t bytes_freed = cell->allocated_size;

#if defined(_WIN32)
  VirtualFree(cell, 0, MEM_RELEASE);
#else
  munmap(cell, bytes_freed);
#endif

  // Shift remaining cells
  for (size_t i = target_idx; i + 1 < _cell_count; ++i) {
    _cells[i] = _cells[i + 1];
  }
  _cells[--_cell_count] = nullptr;

  if (used_bytes() >= bytes_freed) {
    set_used_bytes(used_bytes() - bytes_freed);
  } else {
    set_used_bytes(0);
  }

  _total_cells_released.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_cell_lock);
  return true;
}

RedjetRawCodeCellHeader* RedjetVFSNode_RawCodeCell::find_cell_by_method(uint32_t method_id) noexcept {
  hs_ticket_lock_acquire(&_cell_lock);

  for (size_t i = 0; i < _cell_count; ++i) {
    if (_cells[i] != nullptr && _cells[i]->method_id == method_id) {
      RedjetRawCodeCellHeader* found = _cells[i];
      hs_ticket_lock_release(&_cell_lock);
      return found;
    }
  }

  hs_ticket_lock_release(&_cell_lock);
  return nullptr;
}

void RedjetVFSNode_RawCodeCell::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet VFS Node: Raw Code Cell (/heap/off_heap/code_boot/rawcode_cell) ---");
  st->print_cr("  Tracked Cells:        %zu / %zu", _cell_count, MAX_CELLS_TRACKED);
  st->print_cr("  Used Bytes:           %zu bytes (Quota: %zu bytes)", used_bytes(), quota_bytes());
  st->print_cr("  Total Cells Allocated:%llu", static_cast<unsigned long long>(_total_cells_allocated.load(std::memory_order_relaxed)));
  st->print_cr("  Total Bytes Sealed:   %llu bytes", static_cast<unsigned long long>(_total_bytes_sealed.load(std::memory_order_relaxed)));
  st->print_cr("  Total Seals Verified: %llu", static_cast<unsigned long long>(_total_seals_verified.load(std::memory_order_relaxed)));
  st->print_cr("  Verification Failures:%llu", static_cast<unsigned long long>(_total_verification_failures.load(std::memory_order_relaxed)));
  st->print_cr("  Total Cells Released: %llu", static_cast<unsigned long long>(_total_cells_released.load(std::memory_order_relaxed)));
}
