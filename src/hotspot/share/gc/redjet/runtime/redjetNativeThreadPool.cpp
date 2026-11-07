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

#include "gc/redjet/runtime/redjetNativeThreadPool.hpp"
#include <cstdlib>
#include <cstring>

NativeFCS_ThreadPool::NativeFCS_ThreadPool() {
  head_idx.store(0, std::memory_order_relaxed);
  tail_idx.store(0, std::memory_order_relaxed);
}

void NativeFCS_ThreadPool::push_task(void (*func)(void*), void* data, uint32_t affinity) {
  if (func == nullptr) return;

  uint64_t slot_idx = tail_idx.fetch_add(1, std::memory_order_acq_rel);
  NativeFCS_TaskSlot& slot = ring_buffer[slot_idx % RING_SIZE];

  // Wait if slot is currently occupied (ring buffer full backpressure)
  while (slot.task_func.load(std::memory_order_acquire) != nullptr) {
#if defined(_WIN32) && (defined(_M_AMD64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
  }

  slot.task_data.store(data, std::memory_order_relaxed);
  slot.core_affinity_mask = affinity;
  slot.task_func.store(reinterpret_cast<void*>(func), std::memory_order_release);
}

bool NativeFCS_ThreadPool::steal_task_for_core(uint32_t core_id, void** out_func, void** out_data) {
  if (out_func == nullptr || out_data == nullptr) return false;

  uint64_t current_head = head_idx.load(std::memory_order_acquire);
  uint64_t current_tail = tail_idx.load(std::memory_order_acquire);

  for (uint64_t idx = current_head; idx < current_tail; ++idx) {
    NativeFCS_TaskSlot& slot = ring_buffer[idx % RING_SIZE];
    void* func = slot.task_func.load(std::memory_order_acquire);
    if (func != nullptr) {
      if (slot.core_affinity_mask == 0 || (slot.core_affinity_mask & (1U << (core_id % 32)))) {
        // Attempt lock-free claim
        if (slot.task_func.compare_exchange_strong(func, nullptr, std::memory_order_acq_rel)) {
          *out_func = func;
          *out_data = slot.task_data.load(std::memory_order_relaxed);
          // Advance head index if this was the leading task
          if (idx == current_head) {
            head_idx.compare_exchange_strong(current_head, current_head + 1, std::memory_order_release);
          }
          return true;
        }
      }
    }
  }

  return false;
}

size_t NativeFCS_ThreadPool::pending_tasks() const {
  uint64_t head = head_idx.load(std::memory_order_relaxed);
  uint64_t tail = tail_idx.load(std::memory_order_relaxed);
  return tail > head ? (size_t)(tail - head) : 0;
}

// ============================================================================
// NativeRedjet_SlabAllocator Implementation
// ============================================================================

NativeRedjet_SlabAllocator::NativeRedjet_SlabAllocator() {
  _total_allocated_bytes.store(0, std::memory_order_relaxed);
  SlabHeader* first = allocate_new_slab();
  _current_slab.store(first, std::memory_order_release);
}

NativeRedjet_SlabAllocator::~NativeRedjet_SlabAllocator() {
  SlabHeader* slab = _current_slab.load(std::memory_order_relaxed);
  while (slab != nullptr) {
    SlabHeader* next = slab->next_slab;
    free(slab);
    slab = next;
  }
}

NativeRedjet_SlabAllocator::SlabHeader* NativeRedjet_SlabAllocator::allocate_new_slab() {
  SlabHeader* slab = reinterpret_cast<SlabHeader*>(malloc(SLAB_SIZE));
  if (slab != nullptr) {
    slab->bump_offset.store(0, std::memory_order_relaxed);
    slab->next_slab = nullptr;
  }
  return slab;
}

void* NativeRedjet_SlabAllocator::allocate(size_t size_bytes, size_t alignment) {
  if (size_bytes == 0) return nullptr;

  // Align requested size
  size_t aligned_size = (size_bytes + alignment - 1) & ~(alignment - 1);
  const size_t capacity = sizeof(SlabHeader::data);

  SlabHeader* slab = _current_slab.load(std::memory_order_relaxed);
  while (slab != nullptr) {
    size_t prev_offset = slab->bump_offset.fetch_add(aligned_size, std::memory_order_acq_rel);
    if (prev_offset + aligned_size <= capacity) {
      _total_allocated_bytes.fetch_add(aligned_size, std::memory_order_relaxed);
      return reinterpret_cast<void*>(&slab->data[prev_offset]);
    }

    // Slab exhausted: allocate new slab
    SlabHeader* new_slab = allocate_new_slab();
    if (new_slab == nullptr) return nullptr;

    new_slab->next_slab = slab;
    if (_current_slab.compare_exchange_strong(slab, new_slab, std::memory_order_acq_rel)) {
      slab = new_slab;
    } else {
      // Race: another thread already linked a new slab, free ours and retry
      free(new_slab);
      slab = _current_slab.load(std::memory_order_relaxed);
    }
  }

  return nullptr;
}

void NativeRedjet_SlabAllocator::reset() {
  SlabHeader* slab = _current_slab.load(std::memory_order_relaxed);
  while (slab != nullptr) {
    slab->bump_offset.store(0, std::memory_order_release);
    slab = slab->next_slab;
  }
  _total_allocated_bytes.store(0, std::memory_order_relaxed);
}
