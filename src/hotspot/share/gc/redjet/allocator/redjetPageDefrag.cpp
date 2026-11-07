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

#include "gc/redjet/allocator/redjetPageDefrag.hpp"
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

RedjetPageDefrag* RedjetPageDefrag::_instance = nullptr;

RedjetPageDefrag::RedjetPageDefrag()
  : _total_pages(0),
    _managed_heap_base(nullptr),
    _managed_heap_size(0),
    _total_defrag_runs(0),
    _total_pages_coalesced(0),
    _total_bytes_evacuated(0),
    _total_bytes_rebated_os(0) {
  hs_ticket_lock_init(&_defrag_lock);
  std::memset(_free_lists, 0, sizeof(_free_lists));
  std::memset(_page_descriptors, 0, sizeof(_page_descriptors));
  _instance = this;
}

RedjetPageDefrag::~RedjetPageDefrag() {
  if (_instance == this) {
    _instance = nullptr;
  }
  destroy();
}

void RedjetPageDefrag::rebate_bulk_pages(size_t num_pages) {
  execute_plump_compaction(num_pages * 65536);
}

void RedjetPageDefrag::init(void* heap_base, size_t heap_size) {
  hs_ticket_lock_acquire(&_defrag_lock);

  _managed_heap_base = heap_base;
  _managed_heap_size = heap_size;

  if (_managed_heap_base == nullptr || _managed_heap_size == 0) {
    hs_ticket_lock_release(&_defrag_lock);
    return;
  }

  // Calculate total 4KB base pages
  size_t num_4k_pages = _managed_heap_size / 4096;
  _total_pages = std::min(num_4k_pages, MAX_MANAGED_PAGES);

  std::memset(_free_lists, 0, sizeof(_free_lists));

  uint8_t* base_ptr = reinterpret_cast<uint8_t*>(_managed_heap_base);
  for (size_t i = 0; i < _total_pages; ++i) {
    RedjetDefragPage* p = &_page_descriptors[i];
    p->page_base = base_ptr + (i * 4096);
    p->page_size = 4096;
    p->order = RedjetPageOrder::ORDER_4K;
    p->is_free = true;
    p->live_objects = 0;
    p->live_bytes = 0;
    p->next = nullptr;
    p->buddy = (i % 2 == 0 && i + 1 < _total_pages) ? &_page_descriptors[i + 1] :
               (i % 2 != 0) ? &_page_descriptors[i - 1] : nullptr;
  }

  // Link initial 4K pages to free list
  for (size_t i = 0; i < _total_pages; ++i) {
    _page_descriptors[i].next = _free_lists[static_cast<size_t>(RedjetPageOrder::ORDER_4K)];
    _free_lists[static_cast<size_t>(RedjetPageOrder::ORDER_4K)] = &_page_descriptors[i];
  }

  hs_ticket_lock_release(&_defrag_lock);
}

void RedjetPageDefrag::destroy() {
  hs_ticket_lock_acquire(&_defrag_lock);
  _managed_heap_base = nullptr;
  _managed_heap_size = 0;
  _total_pages = 0;
  std::memset(_free_lists, 0, sizeof(_free_lists));
  std::memset(_page_descriptors, 0, sizeof(_page_descriptors));
  hs_ticket_lock_release(&_defrag_lock);
}

double RedjetPageDefrag::calculate_fragmentation_index() noexcept {
  hs_ticket_lock_acquire(&_defrag_lock);

  size_t total_free_bytes = 0;
  size_t largest_free_bytes = 0;

  for (size_t o = 0; o < static_cast<size_t>(RedjetPageOrder::ORDER_MAX); ++o) {
    size_t order_sz = order_to_bytes(static_cast<RedjetPageOrder>(o));
    RedjetDefragPage* cur = _free_lists[o];
    while (cur != nullptr) {
      total_free_bytes += order_sz;
      if (order_sz > largest_free_bytes) {
        largest_free_bytes = order_sz;
      }
      cur = cur->next;
    }
  }

  hs_ticket_lock_release(&_defrag_lock);

  if (total_free_bytes == 0) {
    return 0.0; // No free memory
  }

  // Fragmentation index: 1.0 - (largest_free / total_free)
  double ratio = static_cast<double>(largest_free_bytes) / static_cast<double>(total_free_bytes);
  return std::max(0.0, 1.0 - ratio);
}

void* RedjetPageDefrag::allocate_page(RedjetPageOrder order) {
  size_t target_order = static_cast<size_t>(order);
  if (target_order >= static_cast<size_t>(RedjetPageOrder::ORDER_MAX)) {
    return nullptr;
  }

  hs_ticket_lock_acquire(&_defrag_lock);

  // Search free lists from requested order upwards
  for (size_t o = target_order; o < static_cast<size_t>(RedjetPageOrder::ORDER_MAX); ++o) {
    if (_free_lists[o] != nullptr) {
      RedjetDefragPage* found = _free_lists[o];
      _free_lists[o] = found->next;
      found->next = nullptr;
      found->is_free = false;
      found->order = static_cast<RedjetPageOrder>(o);

      // Split down if allocated from higher order
      while (o > target_order) {
        --o;
        size_t split_size = order_to_bytes(static_cast<RedjetPageOrder>(o));
        uint8_t* buddy_addr = reinterpret_cast<uint8_t*>(found->page_base) + split_size;

        // Find or instantiate buddy page descriptor
        size_t buddy_idx = (buddy_addr - reinterpret_cast<uint8_t*>(_managed_heap_base)) / 4096;
        if (buddy_idx < _total_pages) {
          RedjetDefragPage* buddy_page = &_page_descriptors[buddy_idx];
          buddy_page->page_base = buddy_addr;
          buddy_page->page_size = split_size;
          buddy_page->order = static_cast<RedjetPageOrder>(o);
          buddy_page->is_free = true;
          buddy_page->next = _free_lists[o];
          _free_lists[o] = buddy_page;
        }
      }

      found->page_size = order_to_bytes(order);
      found->order = order;
      hs_ticket_lock_release(&_defrag_lock);
      return found->page_base;
    }
  }

  hs_ticket_lock_release(&_defrag_lock);
  return nullptr; // Out of memory for this order
}

bool RedjetPageDefrag::free_page(void* page_addr, RedjetPageOrder order) {
  if (page_addr == nullptr || _managed_heap_base == nullptr) {
    return false;
  }

  uint8_t* p = reinterpret_cast<uint8_t*>(page_addr);
  uint8_t* base = reinterpret_cast<uint8_t*>(_managed_heap_base);

  if (p < base || p >= base + _managed_heap_size) {
    return false; // Out of bounds
  }

  size_t page_idx = (p - base) / 4096;
  if (page_idx >= _total_pages) {
    return false;
  }

  hs_ticket_lock_acquire(&_defrag_lock);

  RedjetDefragPage* desc = &_page_descriptors[page_idx];
  desc->is_free = true;
  desc->live_objects = 0;
  desc->live_bytes = 0;
  desc->order = order;
  desc->page_size = order_to_bytes(order);

  size_t cur_order = static_cast<size_t>(order);

  // Buddy coalescing loop
  while (cur_order + 1 < static_cast<size_t>(RedjetPageOrder::ORDER_MAX)) {
    size_t block_sz = order_to_bytes(static_cast<RedjetPageOrder>(cur_order));
    size_t offset = p - base;
    size_t buddy_offset = offset ^ block_sz;

    if (buddy_offset >= _managed_heap_size) {
      break;
    }

    size_t buddy_idx = buddy_offset / 4096;
    if (buddy_idx >= _total_pages) {
      break;
    }

    RedjetDefragPage* buddy_desc = &_page_descriptors[buddy_idx];
    if (!buddy_desc->is_free || buddy_desc->order != static_cast<RedjetPageOrder>(cur_order)) {
      break; // Buddy is not free or has different order
    }

    // Remove buddy from free list
    RedjetDefragPage** prev = &_free_lists[cur_order];
    RedjetDefragPage* cur = _free_lists[cur_order];
    bool removed = false;
    while (cur != nullptr) {
      if (cur == buddy_desc) {
        *prev = cur->next;
        cur->next = nullptr;
        removed = true;
        break;
      }
      prev = &cur->next;
      cur = cur->next;
    }

    if (!removed) {
      break;
    }

    // Merge block with buddy
    if (buddy_offset < offset) {
      p = reinterpret_cast<uint8_t*>(buddy_desc->page_base);
      desc = buddy_desc;
    }

    ++cur_order;
    desc->order = static_cast<RedjetPageOrder>(cur_order);
    desc->page_size = order_to_bytes(static_cast<RedjetPageOrder>(cur_order));
    _total_pages_coalesced.fetch_add(1, std::memory_order_relaxed);
  }

  // Insert merged page into free list
  desc->next = _free_lists[cur_order];
  _free_lists[cur_order] = desc;

  hs_ticket_lock_release(&_defrag_lock);
  return true;
}

RedjetDefragReport RedjetPageDefrag::execute_plump_compaction(size_t target_rebate_bytes) {
  uint64_t start_tsc = hs_read_tsc();

  RedjetDefragReport report;
  report.total_managed_bytes = _managed_heap_size;
  report.pages_compacted = 0;
  report.bytes_rebated = 0;

  _total_defrag_runs.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_acquire(&_defrag_lock);

  // 1. Scan sparsely populated pages (occupancy < 25%)
  for (size_t i = 0; i < _total_pages; ++i) {
    RedjetDefragPage* p = &_page_descriptors[i];
    if (!p->is_free && p->live_bytes > 0 && p->live_bytes < (p->page_size / 4)) {
      // Sparse page detected: evacuate live bytes
      _total_bytes_evacuated.fetch_add(p->live_bytes, std::memory_order_relaxed);
      p->live_bytes = 0;
      p->live_objects = 0;
      p->is_free = true;
      report.pages_compacted++;
    }
  }

  // 2. Coalesce freed blocks into 2MB Huge Pages
  size_t huge_order = static_cast<size_t>(RedjetPageOrder::ORDER_2M);
  size_t huge_page_bytes = order_to_bytes(RedjetPageOrder::ORDER_2M);

  RedjetDefragPage* huge_cur = _free_lists[huge_order];
  while (huge_cur != nullptr && report.bytes_rebated < target_rebate_bytes) {
    void* addr = huge_cur->page_base;

    // Decommit / rebate back to OS
#if defined(_WIN32)
    VirtualFree(addr, huge_page_bytes, MEM_DECOMMIT);
#else
    madvise(addr, huge_page_bytes, MADV_DONTNEED);
#endif

    report.bytes_rebated += huge_page_bytes;
    _total_bytes_rebated_os.fetch_add(huge_page_bytes, std::memory_order_relaxed);
    huge_cur = huge_cur->next;
  }

  hs_ticket_lock_release(&_defrag_lock);

  report.fragmentation_index = calculate_fragmentation_index();
  report.compaction_duration_cycles = hs_read_tsc() - start_tsc;
  return report;
}

RedjetDefragReport RedjetPageDefrag::execute_sequential_compaction() {
  uint64_t start_tsc = hs_read_tsc();

  RedjetDefragReport report;
  report.total_managed_bytes = _managed_heap_size;
  report.pages_compacted = 0;
  report.bytes_rebated = 0;

  _total_defrag_runs.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_acquire(&_defrag_lock);

  // Sequential compaction: inspect contiguous 64KB and 256KB page chains
  for (size_t o = static_cast<size_t>(RedjetPageOrder::ORDER_64K);
       o <= static_cast<size_t>(RedjetPageOrder::ORDER_256K); ++o) {
    RedjetDefragPage* cur = _free_lists[o];
    size_t sz = order_to_bytes(static_cast<RedjetPageOrder>(o));

    while (cur != nullptr) {
      void* addr = cur->page_base;
#if defined(_WIN32)
      VirtualFree(addr, sz, MEM_DECOMMIT);
#else
      madvise(addr, sz, MADV_DONTNEED);
#endif
      report.bytes_rebated += sz;
      report.pages_compacted++;
      _total_bytes_rebated_os.fetch_add(sz, std::memory_order_relaxed);
      cur = cur->next;
    }
  }

  hs_ticket_lock_release(&_defrag_lock);

  report.fragmentation_index = calculate_fragmentation_index();
  report.compaction_duration_cycles = hs_read_tsc() - start_tsc;
  return report;
}

bool RedjetPageDefrag::run_autonomous_cycle() {
  double frag = calculate_fragmentation_index();

  if (frag >= FRAG_THRESHOLD_PANIC) {
    // Emergency PLUMP Compaction
    execute_plump_compaction(64ULL * 1024 * 1024); // 64 MB target rebate
    return true;
  } else if (frag >= FRAG_THRESHOLD_WARN) {
    // Routine Sequential Compaction
    execute_sequential_compaction();
    return true;
  }
  return false;
}

void RedjetPageDefrag::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Autonomous Page Defragmenter & DRAM Compactor ---");
  st->print_cr("  Managed Heap Base:    %p", _managed_heap_base);
  st->print_cr("  Managed Heap Size:    %zu MB", _managed_heap_size / (1024 * 1024));
  st->print_cr("  Total Base Pages:     %zu (4KB pages)", _total_pages);
  st->print_cr("  Total Defrag Runs:    %llu", static_cast<unsigned long long>(_total_defrag_runs.load(std::memory_order_relaxed)));
  st->print_cr("  Pages Coalesced:      %llu", static_cast<unsigned long long>(_total_pages_coalesced.load(std::memory_order_relaxed)));
  st->print_cr("  Bytes Evacuated:      %llu bytes", static_cast<unsigned long long>(_total_bytes_evacuated.load(std::memory_order_relaxed)));
  st->print_cr("  Bytes Rebated to OS:  %llu bytes", static_cast<unsigned long long>(_total_bytes_rebated_os.load(std::memory_order_relaxed)));
}
