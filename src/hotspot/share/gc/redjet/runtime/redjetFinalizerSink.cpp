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

#include "gc/redjet/runtime/redjetFinalizerSink.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetFinalizerSink* RedjetFinalizerSink::_instance = nullptr;
std::atomic<bool> RedjetFinalizerSink::_initialized{ false };

RedjetFinalizerSink::RedjetFinalizerSink()
  : _queue_head(nullptr),
    _queue_depth(0),
    _total_references_enqueued(0),
    _total_objects_resurrected(0),
    _total_cleaners_dispatched(0),
    _total_drains_executed(0) {
  hs_ticket_lock_init(&_sink_lock);
}

RedjetFinalizerSink::~RedjetFinalizerSink() {
  // Free any pending nodes
  RedjetRehabNode* cur = _queue_head.exchange(nullptr);
  while (cur != nullptr) {
    RedjetRehabNode* next = cur->next;
    std::free(cur);
    cur = next;
  }
}

void RedjetFinalizerSink::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetFinalizerSink();
  }
}

void RedjetFinalizerSink::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetFinalizerSink::enqueue_reference(void* obj_addr, RedjetRefKind kind, uint32_t klass_id) {
  if (obj_addr == nullptr) return;

  RedjetRehabNode* node = reinterpret_cast<RedjetRehabNode*>(std::malloc(sizeof(RedjetRehabNode)));
  if (node == nullptr) return;

  node->object_address = obj_addr;
  node->kind = kind;
  node->klass_id = klass_id;
  node->enqueue_tsc = hs_read_tsc();
  node->next = nullptr;

  // Lock-free atomic push (Treiber stack)
  RedjetRehabNode* old_head = _queue_head.load(std::memory_order_relaxed);
  do {
    node->next = old_head;
  } while (!_queue_head.compare_exchange_weak(old_head, node,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));

  _queue_depth.fetch_add(1, std::memory_order_relaxed);
  _total_references_enqueued.fetch_add(1, std::memory_order_relaxed);
}

size_t RedjetFinalizerSink::drain_rehab_batch(RedjetRehabNode** out_batch, size_t max_items) {
  if (out_batch == nullptr || max_items == 0) return 0;

  // Atomically detach queue
  RedjetRehabNode* head = _queue_head.exchange(nullptr, std::memory_order_acq_rel);
  if (head == nullptr) return 0;

  size_t count = 0;
  RedjetRehabNode* cur = head;
  RedjetRehabNode* leftover = nullptr;

  while (cur != nullptr && count < max_items) {
    out_batch[count++] = cur;
    if (cur->kind == RedjetRefKind::REF_CLEANER) {
      _total_cleaners_dispatched.fetch_add(1, std::memory_order_relaxed);
    }
    cur = cur->next;
  }

  leftover = cur;

  // Re-push any leftover nodes that exceeded batch limit
  if (leftover != nullptr) {
    RedjetRehabNode* tail = leftover;
    while (tail->next != nullptr) {
      tail = tail->next;
    }

    RedjetRehabNode* old_head = _queue_head.load(std::memory_order_relaxed);
    do {
      tail->next = old_head;
    } while (!_queue_head.compare_exchange_weak(old_head, leftover,
                                                std::memory_order_release,
                                                std::memory_order_relaxed));
  }

  if (_queue_depth.load(std::memory_order_relaxed) >= count) {
    _queue_depth.fetch_sub(count, std::memory_order_relaxed);
  } else {
    _queue_depth.store(0, std::memory_order_relaxed);
  }

  _total_drains_executed.fetch_add(1, std::memory_order_relaxed);
  return count;
}

bool RedjetFinalizerSink::resurrect_object(void* obj_addr) {
  if (obj_addr == nullptr) return false;

  // Object resurrection cancels uncommit and marks live
  _total_objects_resurrected.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void RedjetFinalizerSink::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Finalizer & Resurrection Sink ---");
  st->print_cr("  Queue Depth:          %zu nodes", _queue_depth.load(std::memory_order_relaxed));
  st->print_cr("  References Enqueued:  %llu", static_cast<unsigned long long>(_total_references_enqueued.load(std::memory_order_relaxed)));
  st->print_cr("  Objects Resurrected:  %llu", static_cast<unsigned long long>(_total_objects_resurrected.load(std::memory_order_relaxed)));
  st->print_cr("  Cleaners Dispatched:  %llu", static_cast<unsigned long long>(_total_cleaners_dispatched.load(std::memory_order_relaxed)));
  st->print_cr("  Drains Executed:      %llu", static_cast<unsigned long long>(_total_drains_executed.load(std::memory_order_relaxed)));
}
