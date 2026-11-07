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

#include "gc/redjet/allocator/redjetSlabPool.hpp"
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

RedjetSlabPool* RedjetSlabPool::_instance = nullptr;
std::atomic<bool> RedjetSlabPool::_initialized{ false };

RedjetSlabPool::RedjetSlabPool()
  : _pool_backing_memory(nullptr),
    _pool_capacity(0),
    _pool_carved_offset(0),
    _total_allocations(0),
    _total_deallocations(0),
    _total_blocks_carved(0) {
  for (size_t i = 0; i < NUM_CLASSES; ++i) {
    _class_blocks[i] = nullptr;
    hs_ticket_lock_init(&_pool_locks[i]);
  }
}

RedjetSlabPool::~RedjetSlabPool() {
  if (_pool_backing_memory != nullptr) {
#if defined(_WIN32)
    VirtualFree(_pool_backing_memory, 0, MEM_RELEASE);
#else
    munmap(_pool_backing_memory, _pool_capacity);
#endif
    _pool_backing_memory = nullptr;
  }
}

void RedjetSlabPool::initialize(size_t capacity_bytes) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetSlabPool();
    _instance->_pool_capacity = (capacity_bytes > 0) ? capacity_bytes : DEFAULT_POOL_CAPACITY_BYTES;

#if defined(_WIN32)
    _instance->_pool_backing_memory = VirtualAlloc(nullptr, _instance->_pool_capacity,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    _instance->_pool_backing_memory = mmap(nullptr, _instance->_pool_capacity,
                                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (_instance->_pool_backing_memory == MAP_FAILED) {
      _instance->_pool_backing_memory = nullptr;
    }
#endif
  }
}

void RedjetSlabPool::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

size_t RedjetSlabPool::class_to_slot_size(RedjetSlabClass sclass) noexcept {
  switch (sclass) {
    case RedjetSlabClass::CLASS_16B:  return 16;
    case RedjetSlabClass::CLASS_32B:  return 32;
    case RedjetSlabClass::CLASS_64B:  return 64;
    case RedjetSlabClass::CLASS_128B: return 128;
    case RedjetSlabClass::CLASS_256B: return 256;
    case RedjetSlabClass::CLASS_512B: return 512;
    case RedjetSlabClass::CLASS_1KB:  return 1024;
    case RedjetSlabClass::CLASS_2KB:  return 2048;
    case RedjetSlabClass::CLASS_4KB:  return 4096;
    case RedjetSlabClass::CLASS_8KB:  return 8192;
    default: return 64;
  }
}

void* RedjetSlabPool::allocate_slot(RedjetSlabClass sclass) {
  size_t c_idx = static_cast<size_t>(sclass);
  if (c_idx >= NUM_CLASSES) return nullptr;

  hs_ticket_lock_acquire(&_pool_locks[c_idx]);

  // 1. Search existing blocks for available free slots
  RedjetSlabBlock* block = _class_blocks[c_idx];
  while (block != nullptr) {
    if (block->freelist_head != nullptr) {
      RedjetSlabSlot* slot = block->freelist_head;
      block->freelist_head = slot->next;
      block->allocated_slots.fetch_add(1, std::memory_order_relaxed);

      _total_allocations.fetch_add(1, std::memory_order_relaxed);
      hs_ticket_lock_release(&_pool_locks[c_idx]);
      return slot;
    }
    block = block->next;
  }

  // 2. No free slots: carve a new 64KB block from backing pool
  size_t offset = _pool_carved_offset.fetch_add(65536, std::memory_order_relaxed);
  if (offset + 65536 > _pool_capacity || _pool_backing_memory == nullptr) {
    hs_ticket_lock_release(&_pool_locks[c_idx]);
    return nullptr; // Backing pool exhausted
  }

  uint8_t* block_mem = reinterpret_cast<uint8_t*>(_pool_backing_memory) + offset;
  RedjetSlabBlock* new_block = reinterpret_cast<RedjetSlabBlock*>(std::malloc(sizeof(RedjetSlabBlock)));
  if (new_block == nullptr) {
    hs_ticket_lock_release(&_pool_locks[c_idx]);
    return nullptr;
  }

  size_t slot_sz = class_to_slot_size(sclass);
  size_t total_s = 65536 / slot_sz;

  new_block->block_memory = block_mem;
  new_block->block_size = 65536;
  new_block->slot_size = slot_sz;
  new_block->total_slots = total_s;
  new_block->allocated_slots.store(1, std::memory_order_relaxed);

  // Link all slots starting from index 1 (index 0 will be returned directly)
  RedjetSlabSlot* head = nullptr;
  for (size_t i = total_s - 1; i >= 1; --i) {
    RedjetSlabSlot* s = reinterpret_cast<RedjetSlabSlot*>(block_mem + (i * slot_sz));
    s->next = head;
    head = s;
  }

  new_block->freelist_head = head;
  new_block->next = _class_blocks[c_idx];
  _class_blocks[c_idx] = new_block;

  _total_blocks_carved.fetch_add(1, std::memory_order_relaxed);
  _total_allocations.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_pool_locks[c_idx]);
  return block_mem; // Return slot 0
}

void RedjetSlabPool::free_slot(void* ptr, RedjetSlabClass sclass) {
  if (ptr == nullptr) return;

  size_t c_idx = static_cast<size_t>(sclass);
  if (c_idx >= NUM_CLASSES) return;

  hs_ticket_lock_acquire(&_pool_locks[c_idx]);

  RedjetSlabBlock* block = _class_blocks[c_idx];
  while (block != nullptr) {
    uint8_t* b_start = reinterpret_cast<uint8_t*>(block->block_memory);
    uint8_t* p = reinterpret_cast<uint8_t*>(ptr);

    if (p >= b_start && p < b_start + block->block_size) {
      RedjetSlabSlot* slot = reinterpret_cast<RedjetSlabSlot*>(ptr);
      slot->next = block->freelist_head;
      block->freelist_head = slot;

      size_t alloc_s = block->allocated_slots.load(std::memory_order_relaxed);
      if (alloc_s > 0) {
        block->allocated_slots.store(alloc_s - 1, std::memory_order_relaxed);
      }

      _total_deallocations.fetch_add(1, std::memory_order_relaxed);
      hs_ticket_lock_release(&_pool_locks[c_idx]);
      return;
    }
    block = block->next;
  }

  hs_ticket_lock_release(&_pool_locks[c_idx]);
}

void RedjetSlabPool::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Tagged Freelist Slab Pool ---");
  st->print_cr("  Pool Capacity:        %zu MB", _pool_capacity / (1024 * 1024));
  st->print_cr("  Carved Memory:        %zu MB", _pool_carved_offset.load(std::memory_order_relaxed) / (1024 * 1024));
  st->print_cr("  Total Blocks Carved:  %llu (64KB blocks)", static_cast<unsigned long long>(_total_blocks_carved.load(std::memory_order_relaxed)));
  st->print_cr("  Total Allocations:    %llu", static_cast<unsigned long long>(_total_allocations.load(std::memory_order_relaxed)));
  st->print_cr("  Total Deallocations:  %llu", static_cast<unsigned long long>(_total_deallocations.load(std::memory_order_relaxed)));
}
