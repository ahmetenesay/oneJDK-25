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

#include "gc/redjet/vfs/redjetVFSNode_Landfill.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

RedjetVFSNode_Landfill::RedjetVFSNode_Landfill(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
    : RedjetVFSNode(name, full_path, parent, quota_bytes) {

  _header_blocks_head = allocate_header_block();
}

RedjetVFSNode_Landfill::~RedjetVFSNode_Landfill() {
  hs_ticket_guard_t guard(_landfill_lock);
  RedjetHeaderPaneBlock* cur = _header_blocks_head;
  while (cur != nullptr) {
    RedjetHeaderPaneBlock* next = cur->next_block;
    delete cur;
    cur = next;
  }
  _header_blocks_head = nullptr;
}

RedjetHeaderPaneBlock* RedjetVFSNode_Landfill::allocate_header_block() {
  auto* block = new (std::nothrow) RedjetHeaderPaneBlock();
  if (block != nullptr) {
    block->init();
    block->next_block = nullptr;
    _header_block_count.fetch_add(1, std::memory_order_relaxed);
  }
  return block;
}

size_t RedjetVFSNode_Landfill::uncommit_matured_pages(void* base_addr, size_t num_bytes) {
  if (base_addr == nullptr || num_bytes == 0) return 0;

  // Align to 4KB OS page boundary
  uintptr_t addr = reinterpret_cast<uintptr_t>(base_addr);
  uintptr_t aligned_base = (addr + 4095) & ~static_cast<uintptr_t>(4095);
  uintptr_t end_addr = addr + num_bytes;
  uintptr_t aligned_end = end_addr & ~static_cast<uintptr_t>(4095);

  if (aligned_end <= aligned_base) {
    return 0;
  }

  size_t uncommit_len = static_cast<size_t>(aligned_end - aligned_base);

#if defined(_WIN32)
  BOOL res = VirtualFree(reinterpret_cast<void*>(aligned_base), uncommit_len, MEM_DECOMMIT);
  if (!res) {
    return 0;
  }
#else
  int res = madvise(reinterpret_cast<void*>(aligned_base), uncommit_len, MADV_DONTNEED);
  if (res != 0) {
    return 0;
  }
#endif

  _total_uncommit_operations.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_uncommitted.fetch_add(uncommit_len, std::memory_order_relaxed);
  sub_used(uncommit_len);
  return uncommit_len;
}

void RedjetVFSNode_Landfill::stage_rehab_reference(void* oop, size_t sz) {
  if (oop == nullptr) return;
  _total_rehab_admissions.fetch_add(1, std::memory_order_relaxed);
  add_used(sz);
}

bool RedjetVFSNode_Landfill::dispatch_rehab_return(void* oop, void** out_destination_slot) {
  if (oop == nullptr) return false;
  if (out_destination_slot != nullptr) {
    *out_destination_slot = oop;
  }
  _total_rehab_returns.fetch_add(1, std::memory_order_relaxed);
  return true;
}

RedjetCompactHeaderEntry* RedjetVFSNode_Landfill::allocate_compact_header(
    void* oop, uint32_t klass_id, uint64_t mark_word) {

  hs_ticket_guard_t guard(_landfill_lock);
  RedjetHeaderPaneBlock* block = _header_blocks_head;

  while (block != nullptr) {
    RedjetCompactHeaderEntry* e = block->allocate();
    if (e != nullptr) {
      e->associated_oop = oop;
      e->compressed_klass_id = klass_id;
      e->compact_mark_word = mark_word;
      e->flags = 1;
      _total_header_allocations.fetch_add(1, std::memory_order_relaxed);
      return e;
    }
    if (block->next_block == nullptr) {
      // Allocate fresh block and chain it
      RedjetHeaderPaneBlock* new_block = allocate_header_block();
      if (new_block != nullptr) {
        block->next_block = new_block;
      }
      break;
    }
    block = block->next_block;
  }

  // Allocate from newly added block
  if (block != nullptr && block->next_block != nullptr) {
    RedjetCompactHeaderEntry* e = block->next_block->allocate();
    if (e != nullptr) {
      e->associated_oop = oop;
      e->compressed_klass_id = klass_id;
      e->compact_mark_word = mark_word;
      e->flags = 1;
      _total_header_allocations.fetch_add(1, std::memory_order_relaxed);
      return e;
    }
  }

  return nullptr;
}

void RedjetVFSNode_Landfill::free_compact_header(RedjetCompactHeaderEntry* header) {
  if (header == nullptr) return;

  hs_ticket_guard_t guard(_landfill_lock);
  RedjetHeaderPaneBlock* block = _header_blocks_head;

  while (block != nullptr) {
    if (header >= &block->entries[0] && header < &block->entries[RedjetHeaderPaneBlock::ENTRIES_PER_BLOCK]) {
      block->free_entry(header);
      _total_header_frees.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    block = block->next_block;
  }
}

void RedjetVFSNode_Landfill::print_on(outputStream* st) const {
  st->print_cr("Redjet Landfill VFS Node [%s]:", full_path());
  st->print_cr("  Matured Uncommits: " UINT64_FORMAT ", Total Bytes Uncommitted: " UINT64_FORMAT,
               total_uncommits(), total_bytes_uncommitted());
  st->print_cr("  Rehab Admissions: " UINT64_FORMAT ", Rehab Returns: " UINT64_FORMAT,
               total_rehab_admissions(), total_rehab_returns());
  st->print_cr("  Header Blocks: %zu, Active Compact Headers: " UINT64_FORMAT,
               _header_block_count.load(std::memory_order_relaxed), total_headers_active());
}
