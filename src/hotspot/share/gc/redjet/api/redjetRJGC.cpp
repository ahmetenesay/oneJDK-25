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

#include "gc/redjet/api/redjetRJGC.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <cstdlib>

RJGCTrackedHandle               RedjetRJGC::_handle_pool[RedjetRJGC::MAX_TRACKED_HANDLES];
std::atomic<RJGCTrackedHandle*> RedjetRJGC::_free_head{nullptr};
std::atomic<bool>               RedjetRJGC::_pool_initialized{false};
std::atomic<RJGCTrackedHandle*> RedjetRJGC::_head_handle{nullptr};
std::atomic<size_t>             RedjetRJGC::_total_registered{0};
std::atomic<size_t>             RedjetRJGC::_total_evicted_bytes{0};

void RedjetRJGC::ensure_pool_initialized() {
  bool expected = false;
  if (_pool_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    for (size_t i = 0; i < MAX_TRACKED_HANDLES - 1; ++i) {
      _handle_pool[i].next = &_handle_pool[i + 1];
      _handle_pool[i].in_pool = true;
    }
    _handle_pool[MAX_TRACKED_HANDLES - 1].next = nullptr;
    _handle_pool[MAX_TRACKED_HANDLES - 1].in_pool = true;
    _free_head.store(&_handle_pool[0], std::memory_order_release);
  }
}

RJGCTrackedHandle* RedjetRJGC::acquire_handle() {
  ensure_pool_initialized();

  // Pop from free list
  RJGCTrackedHandle* h = _free_head.load(std::memory_order_acquire);
  while (h != nullptr) {
    if (_free_head.compare_exchange_weak(h, h->next, std::memory_order_acq_rel)) {
      h->next = nullptr;
      return h;
    }
  }

  // Dynamic fallback if pool is saturated
  RJGCTrackedHandle* dynamic_handle = new RJGCTrackedHandle();
  dynamic_handle->in_pool = false;
  return dynamic_handle;
}

void RedjetRJGC::recycle_handle(RJGCTrackedHandle* handle) {
  if (handle == nullptr) return;

  if (handle->in_pool) {
    handle->off_heap_ptr = nullptr;
    handle->size_bytes = 0;
    handle->custom_destructor = nullptr;
    handle->ref_count.store(0, std::memory_order_relaxed);
    handle->is_evicted.store(false, std::memory_order_relaxed);

    RJGCTrackedHandle* head = _free_head.load(std::memory_order_relaxed);
    do {
      handle->next = head;
    } while (!_free_head.compare_exchange_weak(head, handle, std::memory_order_release, std::memory_order_relaxed));
  } else {
    delete handle;
  }
}

RJGCTrackedHandle* RedjetRJGC::register_off_heap(void* ptr, size_t size, RJGCMode mode, void (*dtor)(void*, size_t)) {
  if (ptr == nullptr || size == 0) return nullptr;

  RJGCTrackedHandle* handle = acquire_handle();
  handle->off_heap_ptr = ptr;
  handle->size_bytes = size;
  handle->mode = mode;
  handle->ref_count.store(1, std::memory_order_relaxed);
  handle->custom_destructor = dtor;
  handle->is_evicted.store(false, std::memory_order_relaxed);

  // Lock-free stack push
  RJGCTrackedHandle* old_head = _head_handle.load(std::memory_order_relaxed);
  do {
    handle->next = old_head;
  } while (!_head_handle.compare_exchange_weak(old_head, handle, std::memory_order_release, std::memory_order_relaxed));

  _total_registered.fetch_add(1, std::memory_order_relaxed);
  return handle;
}

void RedjetRJGC::evict_immediate(RJGCTrackedHandle* handle) {
  if (handle == nullptr) return;

  bool was_evicted = handle->is_evicted.exchange(true, std::memory_order_acq_rel);
  if (!was_evicted) {
    if (handle->off_heap_ptr != nullptr) {
      if (handle->custom_destructor != nullptr) {
        handle->custom_destructor(handle->off_heap_ptr, handle->size_bytes);
      } else {
        free(handle->off_heap_ptr);
      }
      handle->off_heap_ptr = nullptr;
    }
    _total_evicted_bytes.fetch_add(handle->size_bytes, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0, (uint32_t)handle->size_bytes, "@RJGC:ImmediateEviction");
  }
}

void RedjetRJGC::retain_handle(RJGCTrackedHandle* handle) {
  if (handle != nullptr && !handle->is_evicted.load(std::memory_order_acquire)) {
    handle->ref_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void RedjetRJGC::release_handle(RJGCTrackedHandle* handle) {
  if (handle == nullptr) return;

  int32_t remaining = handle->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining <= 0) {
    evict_immediate(handle);
  }
}

size_t RedjetRJGC::poll_and_evict_continuous() {
  size_t reclaimed_bytes = 0;
  RJGCTrackedHandle* curr = _head_handle.load(std::memory_order_acquire);

  while (curr != nullptr) {
    if (curr->mode == RJGCMode::CONTINUOUS_FLOW_WATCH && !curr->is_evicted.load(std::memory_order_relaxed)) {
      if (curr->ref_count.load(std::memory_order_acquire) <= 0) {
        size_t sz = curr->size_bytes;
        evict_immediate(curr);
        reclaimed_bytes += sz;
      }
    }
    curr = curr->next;
  }

  return reclaimed_bytes;
}
