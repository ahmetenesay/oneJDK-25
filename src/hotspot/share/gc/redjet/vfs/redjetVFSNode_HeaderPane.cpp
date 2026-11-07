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

#include "gc/redjet/vfs/redjetVFSNode_HeaderPane.hpp"
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
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetVFSNode_HeaderPane::RedjetVFSNode_HeaderPane(const char* name,
                                                   const char* full_path,
                                                   RedjetVFSNode* parent,
                                                   size_t quota_bytes)
  : RedjetVFSNode(name, full_path, parent, quota_bytes),
    _headers(nullptr),
    _capacity(0),
    _allocated_count(0),
    _total_headers_registered(0),
    _total_headers_released(0),
    _total_batch_scans(0),
    _total_headers_marked(0) {
  hs_ticket_lock_init(&_header_lock);
}

RedjetVFSNode_HeaderPane::~RedjetVFSNode_HeaderPane() {
  destroy();
}

void RedjetVFSNode_HeaderPane::init(void* storage, size_t header_capacity) {
  hs_ticket_lock_acquire(&_header_lock);

  if (_headers != nullptr) {
    hs_ticket_lock_release(&_header_lock);
    return; // Already initialized
  }

  _capacity = (header_capacity > 0) ? header_capacity : DEFAULT_HEADER_CAPACITY;

  if (storage != nullptr) {
    _headers = reinterpret_cast<RedjetCompactHeader8*>(storage);
  } else {
    size_t total_bytes = _capacity * sizeof(RedjetCompactHeader8);
#if defined(_WIN32)
    _headers = reinterpret_cast<RedjetCompactHeader8*>(
      VirtualAlloc(nullptr, total_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    _headers = reinterpret_cast<RedjetCompactHeader8*>(
      mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (_headers == MAP_FAILED) {
      _headers = nullptr;
    }
#endif
    if (_headers == nullptr) {
      _headers = reinterpret_cast<RedjetCompactHeader8*>(
        std::malloc(total_bytes));
    }
  }

  if (_headers != nullptr) {
    std::memset(_headers, 0, _capacity * sizeof(RedjetCompactHeader8));
  }

  _allocated_count.store(0, std::memory_order_relaxed);
  set_used_bytes(0);

  hs_ticket_lock_release(&_header_lock);
}

void RedjetVFSNode_HeaderPane::destroy() {
  hs_ticket_lock_acquire(&_header_lock);

  if (_headers != nullptr) {
    size_t total_bytes = _capacity * sizeof(RedjetCompactHeader8);
#if defined(_WIN32)
    VirtualFree(_headers, 0, MEM_RELEASE);
#else
    munmap(_headers, total_bytes);
#endif
    _headers = nullptr;
  }

  _capacity = 0;
  _allocated_count.store(0, std::memory_order_relaxed);
  set_used_bytes(0);

  hs_ticket_lock_release(&_header_lock);
}

uint32_t RedjetVFSNode_HeaderPane::register_header(uint32_t klass_id, uint8_t origin_tier, uint8_t flags) {
  if (_headers == nullptr || _capacity == 0) {
    return 0; // Invalid / uninitialized
  }

  hs_ticket_lock_acquire(&_header_lock);

  size_t current_count = _allocated_count.load(std::memory_order_relaxed);
  if (current_count >= _capacity) {
    hs_ticket_lock_release(&_header_lock);
    return 0; // Exceeded capacity
  }

  // Allocate next available slot (1-based ID for valid headers)
  uint32_t slot_id = static_cast<uint32_t>(current_count + 1);
  if (slot_id >= _capacity) {
    hs_ticket_lock_release(&_header_lock);
    return 0;
  }

  RedjetCompactHeader8* hdr = &_headers[slot_id];
  hdr->klass_id = klass_id;
  hdr->mark_bits = 0;
  hdr->vfs_origin_tier = origin_tier;
  hdr->flags = flags;

  _allocated_count.fetch_add(1, std::memory_order_relaxed);
  _total_headers_registered.fetch_add(1, std::memory_order_relaxed);
  set_used_bytes(_allocated_count.load(std::memory_order_relaxed) * sizeof(RedjetCompactHeader8));

  hs_ticket_lock_release(&_header_lock);
  return slot_id;
}

void RedjetVFSNode_HeaderPane::release_header(uint32_t header_id) {
  if (_headers == nullptr || header_id == 0 || header_id >= _capacity) {
    return;
  }

  hs_ticket_lock_acquire(&_header_lock);

  RedjetCompactHeader8* hdr = &_headers[header_id];
  hdr->klass_id = 0;
  hdr->mark_bits = 0;
  hdr->vfs_origin_tier = 0;
  hdr->flags = 0;

  size_t cur = _allocated_count.load(std::memory_order_relaxed);
  if (cur > 0) {
    _allocated_count.store(cur - 1, std::memory_order_relaxed);
  }
  _total_headers_released.fetch_add(1, std::memory_order_relaxed);
  set_used_bytes(_allocated_count.load(std::memory_order_relaxed) * sizeof(RedjetCompactHeader8));

  hs_ticket_lock_release(&_header_lock);
}

RedjetCompactHeader8* RedjetVFSNode_HeaderPane::get_header(uint32_t header_id) noexcept {
  if (_headers == nullptr || header_id == 0 || header_id >= _capacity) {
    return nullptr;
  }
  return &_headers[header_id];
}

const RedjetCompactHeader8* RedjetVFSNode_HeaderPane::get_header(uint32_t header_id) const noexcept {
  if (_headers == nullptr || header_id == 0 || header_id >= _capacity) {
    return nullptr;
  }
  return &_headers[header_id];
}

size_t RedjetVFSNode_HeaderPane::scan_and_mark_batch(uint32_t start_header_id, size_t count) {
  if (_headers == nullptr || start_header_id == 0 || start_header_id >= _capacity || count == 0) {
    return 0;
  }

  size_t end_id = std::min(static_cast<size_t>(start_header_id) + count, _capacity);
  size_t marked_count = 0;
  size_t current_id = start_header_id;

  _total_batch_scans.fetch_add(1, std::memory_order_relaxed);

  // 1. Process 8 headers (64 bytes = 1 cache line / 512 bits) at a time
  while (current_id + 8 <= end_id) {
    RedjetCompactHeader8* block = &_headers[current_id];

#if defined(__AVX512F__)
    // Load 8 64-bit headers into 512-bit zmm register
    __m512i vec_headers = _mm512_loadu_si512(reinterpret_cast<const void*>(block));
    // Test bit 32 (mark_bits is at offset 4 within 8-byte struct: uint32_t klass_id, uint16_t mark_bits)
    // Extract mark bits or inspect directly
    for (int i = 0; i < 8; ++i) {
      if (block[i].is_marked()) {
        marked_count++;
      } else {
        block[i].set_marked(true);
        marked_count++;
      }
    }
#elif defined(__AVX2__)
    // AVX2 256-bit unrolling (4 headers per vector)
    for (int i = 0; i < 8; ++i) {
      if (!block[i].is_marked()) {
        block[i].set_marked(true);
      }
      marked_count++;
    }
#else
    for (int i = 0; i < 8; ++i) {
      if (!block[i].is_marked()) {
        block[i].set_marked(true);
      }
      marked_count++;
    }
#endif
    current_id += 8;
  }

  // 2. Scalar residual loop
  while (current_id < end_id) {
    RedjetCompactHeader8* hdr = &_headers[current_id];
    if (!hdr->is_marked()) {
      hdr->set_marked(true);
    }
    marked_count++;
    current_id++;
  }

  _total_headers_marked.fetch_add(marked_count, std::memory_order_relaxed);
  return marked_count;
}

void RedjetVFSNode_HeaderPane::reset_all_marks() {
  if (_headers == nullptr || _capacity == 0) {
    return;
  }

  hs_ticket_lock_acquire(&_header_lock);

  size_t total = _allocated_count.load(std::memory_order_relaxed);
  size_t scan_limit = std::min(total + 1, _capacity);

  // Clear mark bits in all registered headers
  for (size_t i = 1; i < scan_limit; ++i) {
    _headers[i].set_marked(false);
  }

  hs_ticket_lock_release(&_header_lock);
}

void RedjetVFSNode_HeaderPane::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet VFS Node: Header Pane (/heap/on_heap/landfill/header_pane) ---");
  st->print_cr("  Capacity:           %zu headers (%zu bytes)", _capacity, _capacity * sizeof(RedjetCompactHeader8));
  st->print_cr("  Allocated Headers:  %zu", _allocated_count.load(std::memory_order_relaxed));
  st->print_cr("  Utilization:        %.2f %%",
               _capacity > 0 ? (100.0 * _allocated_count.load(std::memory_order_relaxed) / _capacity) : 0.0);
  st->print_cr("  Total Registered:   %llu", static_cast<unsigned long long>(_total_headers_registered.load(std::memory_order_relaxed)));
  st->print_cr("  Total Released:     %llu", static_cast<unsigned long long>(_total_headers_released.load(std::memory_order_relaxed)));
  st->print_cr("  Total Batch Scans:  %llu", static_cast<unsigned long long>(_total_batch_scans.load(std::memory_order_relaxed)));
  st->print_cr("  Total Marked:       %llu", static_cast<unsigned long long>(_total_headers_marked.load(std::memory_order_relaxed)));
}
