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

#include "gc/redjet/api/redjetOffHeapReclaimer.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/api/redjetRJGC.hpp"
#include "gc/redjet/api/redjetNativeSDK.hpp"

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

RedjetOffHeapDescriptor                     RedjetOffHeapReclaimer::_descriptor_pool[RedjetOffHeapReclaimer::DESCRIPTOR_POOL_CAPACITY];
std::atomic<RedjetOffHeapDescriptor*>       RedjetOffHeapReclaimer::_pool_free_head{ nullptr };
hs_ticket_lock_t                            RedjetOffHeapReclaimer::_pool_lock;
std::atomic<bool>                           RedjetOffHeapReclaimer::_initialized{ false };
std::atomic<RedjetOffHeapDescriptor*>       RedjetOffHeapReclaimer::_buckets[RedjetOffHeapReclaimer::BUCKET_COUNT];
hs_ticket_lock_t                            RedjetOffHeapReclaimer::_bucket_locks[RedjetOffHeapReclaimer::BUCKET_COUNT];

std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_total_immediate_evictions{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_total_continuous_registrations{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_total_continuous_reclamations{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_total_bytes_reclaimed{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_total_sweep_cycles{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_last_sweep_duration_ns{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_active_descriptors_count{ 0 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_canary_faults_detected{ 0 };

std::atomic<uint8_t>                        RedjetOffHeapReclaimer::_scanner_state{ static_cast<uint8_t>(RedjetOffHeapScannerState::STOPPED) };
std::atomic<uint32_t>                       RedjetOffHeapReclaimer::_sweep_interval_ms{ 25 };
std::atomic<uint64_t>                       RedjetOffHeapReclaimer::_global_reclaimer_epoch{ 1 };

static inline uint64_t hs_get_current_time_ns() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void RedjetOffHeapReclaimer::ensure_pool_initialized() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    for (size_t i = 0; i < DESCRIPTOR_POOL_CAPACITY - 1; ++i) {
      _descriptor_pool[i].next = &_descriptor_pool[i + 1];
      _descriptor_pool[i].in_pool = true;
    }
    _descriptor_pool[DESCRIPTOR_POOL_CAPACITY - 1].next = nullptr;
    _descriptor_pool[DESCRIPTOR_POOL_CAPACITY - 1].in_pool = true;
    _pool_free_head.store(&_descriptor_pool[0], std::memory_order_release);

    for (size_t b = 0; b < BUCKET_COUNT; ++b) {
      _buckets[b].store(nullptr, std::memory_order_relaxed);
    }
  }
}

RedjetOffHeapDescriptor* RedjetOffHeapReclaimer::acquire_descriptor() {
  ensure_pool_initialized();

  {
    hs_ticket_guard_t guard(_pool_lock);
    RedjetOffHeapDescriptor* desc = _pool_free_head.load(std::memory_order_relaxed);
    if (desc != nullptr) {
      _pool_free_head.store(desc->next, std::memory_order_relaxed);
      desc->next = nullptr;
      return desc;
    }
  }

  // Fallback allocation if static descriptor pool is saturated
  RedjetOffHeapDescriptor* dynamic_desc = new RedjetOffHeapDescriptor();
  dynamic_desc->in_pool = false;
  return dynamic_desc;
}

void RedjetOffHeapReclaimer::release_descriptor(RedjetOffHeapDescriptor* desc) {
  if (desc == nullptr) return;

  if (desc->in_pool) {
    desc->buffer_address = nullptr;
    desc->size_bytes = 0;
    desc->alignment = 0;
    desc->owner_thread_id = 0;
    desc->registered_epoch = 0;
    desc->last_accessed_timestamp_ns.store(0, std::memory_order_relaxed);
    desc->flow_ref_count.store(0, std::memory_order_relaxed);
    desc->is_evicted.store(false, std::memory_order_relaxed);
    desc->canary_magic = CANARY_EXPECTED;
    desc->custom_free_func = nullptr;
    desc->rjgc_handle = nullptr;

    hs_ticket_guard_t guard(_pool_lock);
    desc->next = _pool_free_head.load(std::memory_order_relaxed);
    _pool_free_head.store(desc, std::memory_order_relaxed);
  } else {
    delete desc;
  }
}

void RedjetOffHeapReclaimer::initialize() {
  ensure_pool_initialized();
  hs_hotlog_emit(HS_Ebin0_METASPACE_BOOT, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0,
                 static_cast<uint32_t>(DESCRIPTOR_POOL_CAPACITY), "RedjetOffHeapReclaimer::initialize");
}

void RedjetOffHeapReclaimer::shutdown() {
  stop_scanner_worker();

  // Sweep and force-evict all remaining active descriptors
  for (size_t b = 0; b < BUCKET_COUNT; ++b) {
    hs_ticket_guard_t guard(_bucket_locks[b]);
    RedjetOffHeapDescriptor* curr = _buckets[b].exchange(nullptr, std::memory_order_acq_rel);
    while (curr != nullptr) {
      RedjetOffHeapDescriptor* nxt = curr->next;
      evict_descriptor(curr);
      release_descriptor(curr);
      curr = nxt;
    }
  }

  _active_descriptors_count.store(0, std::memory_order_relaxed);
}

RedjetOffHeapDescriptor* RedjetOffHeapReclaimer::register_allocation(
    void* ptr,
    size_t size,
    RedjetOffHeapMarkPolicy policy,
    void (*dtor)(void*, size_t),
    uint32_t owner_thread_id,
    uint32_t alignment) {
  if (ptr == nullptr || size == 0) return nullptr;

  ensure_pool_initialized();

  RedjetOffHeapDescriptor* desc = acquire_descriptor();
  desc->buffer_address = ptr;
  desc->size_bytes = size;
  desc->alignment = alignment;
  desc->mark_policy = policy;
  desc->owner_thread_id = owner_thread_id;
  desc->registered_epoch = _global_reclaimer_epoch.load(std::memory_order_relaxed);
  desc->last_accessed_timestamp_ns.store(hs_get_current_time_ns(), std::memory_order_relaxed);
  desc->flow_ref_count.store(1, std::memory_order_relaxed);
  desc->is_evicted.store(false, std::memory_order_relaxed);
  desc->canary_magic = CANARY_EXPECTED;
  desc->custom_free_func = dtor;

  // Cross-link with RedjetRJGC core subsystem
  RJGCMode rjgc_mode = (policy == RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION)
      ? RJGCMode::IMMEDIATE_EVICTION
      : RJGCMode::CONTINUOUS_FLOW_WATCH;
  desc->rjgc_handle = RedjetRJGC::register_off_heap(ptr, size, rjgc_mode, dtor);

  // Insert into hash bucket under bucket lock
  size_t bucket_idx = hash_address(ptr);
  {
    hs_ticket_guard_t guard(_bucket_locks[bucket_idx]);
    desc->next = _buckets[bucket_idx].load(std::memory_order_relaxed);
    _buckets[bucket_idx].store(desc, std::memory_order_release);
  }

  _active_descriptors_count.fetch_add(1, std::memory_order_relaxed);
  if (policy == RedjetOffHeapMarkPolicy::CONTINUOUS_FLOW_WATCH) {
    _total_continuous_registrations.fetch_add(1, std::memory_order_relaxed);
  }

  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_TRACE,
                 owner_thread_id, static_cast<uint32_t>(bucket_idx),
                 static_cast<uint32_t>(policy), 0, 0, 0,
                 static_cast<uint32_t>(size), "RedjetOffHeapReclaimer::registered");

  return desc;
}

RedjetOffHeapDescriptor* RedjetOffHeapReclaimer::register_by_mark_string(
    void* ptr,
    size_t size,
    const char* mark_str,
    void (*dtor)(void*, size_t),
    uint32_t owner_thread_id) {
  if (ptr == nullptr || size == 0) return nullptr;

  RedjetOffHeapMarkPolicy policy = RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION;
  if (mark_str != nullptr) {
    if (std::strcmp(mark_str, "ContinuousFlowWatch") == 0 ||
        std::strcmp(mark_str, "CONTINUOUS_FLOW_WATCH") == 0 ||
        std::strcmp(mark_str, "FlowWatch") == 0) {
      policy = RedjetOffHeapMarkPolicy::CONTINUOUS_FLOW_WATCH;
    }
  }

  return register_allocation(ptr, size, policy, dtor, owner_thread_id);
}

bool RedjetOffHeapReclaimer::evict_descriptor(RedjetOffHeapDescriptor* desc) {
  if (desc == nullptr) return false;

  bool was_evicted = desc->is_evicted.exchange(true, std::memory_order_acq_rel);
  if (was_evicted) return false;

  // Validate canary integrity
  if (desc->canary_magic != CANARY_EXPECTED) {
    _canary_faults_detected.fetch_add(1, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_MEMORY_PRESSURE, HS_Ebin1_FATAL,
                   0, 0, 0, 0, 0, 0,
                   static_cast<uint32_t>(desc->size_bytes), "RedjetOffHeapReclaimer::CANARY_FAULT");
  }

  void* addr = desc->buffer_address;
  size_t bytes = desc->size_bytes;

  if (addr != nullptr) {
    if (desc->custom_free_func != nullptr) {
      desc->custom_free_func(addr, bytes);
    } else {
      std::free(addr);
    }
    desc->buffer_address = nullptr;
  }

  // Synchronize with RJGC handle if present
  if (desc->rjgc_handle != nullptr) {
    RedjetRJGC::evict_immediate(desc->rjgc_handle);
    desc->rjgc_handle = nullptr;
  }

  _total_bytes_reclaimed.fetch_add(bytes, std::memory_order_relaxed);
  if (desc->mark_policy == RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION) {
    _total_immediate_evictions.fetch_add(1, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO,
                   0, 0, 0, 0, 0, 0,
                   static_cast<uint32_t>(bytes), "@RJGC(Mark = ImmediateEviction) evicted");
  } else {
    _total_continuous_reclamations.fetch_add(1, std::memory_order_relaxed);
    hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO,
                   0, 0, 1, 0, 0, 0,
                   static_cast<uint32_t>(bytes), "@RJGC(Mark = ContinuousFlowWatch) reclaimed");
  }

  return true;
}

bool RedjetOffHeapReclaimer::evict_immediate(void* ptr) {
  if (ptr == nullptr) return false;

  size_t bucket_idx = hash_address(ptr);
  hs_ticket_guard_t guard(_bucket_locks[bucket_idx]);
  RedjetOffHeapDescriptor* prev = nullptr;
  RedjetOffHeapDescriptor* curr = _buckets[bucket_idx].load(std::memory_order_relaxed);

  while (curr != nullptr) {
    if (curr->buffer_address == ptr) {
      bool evicted = evict_descriptor(curr);

      // Unlink from bucket chain
      if (prev == nullptr) {
        _buckets[bucket_idx].store(curr->next, std::memory_order_release);
      } else {
        prev->next = curr->next;
      }

      if (_active_descriptors_count.load(std::memory_order_relaxed) > 0) {
        _active_descriptors_count.fetch_sub(1, std::memory_order_relaxed);
      }
      release_descriptor(curr);
      return evicted;
    }
    prev = curr;
    curr = curr->next;
  }

  return false;
}

void RedjetOffHeapReclaimer::retain_flow_ref(void* ptr) {
  if (ptr == nullptr) return;

  size_t bucket_idx = hash_address(ptr);
  hs_ticket_guard_t guard(_bucket_locks[bucket_idx]);
  RedjetOffHeapDescriptor* curr = _buckets[bucket_idx].load(std::memory_order_relaxed);
  while (curr != nullptr) {
    if (curr->buffer_address == ptr && !curr->is_evicted.load(std::memory_order_relaxed)) {
      curr->flow_ref_count.fetch_add(1, std::memory_order_relaxed);
      curr->last_accessed_timestamp_ns.store(hs_get_current_time_ns(), std::memory_order_relaxed);
      if (curr->rjgc_handle != nullptr) {
        RedjetRJGC::retain_handle(curr->rjgc_handle);
      }
      return;
    }
    curr = curr->next;
  }
}

void RedjetOffHeapReclaimer::release_flow_ref(void* ptr) {
  if (ptr == nullptr) return;

  size_t bucket_idx = hash_address(ptr);
  bool do_evict = false;
  {
    hs_ticket_guard_t guard(_bucket_locks[bucket_idx]);
    RedjetOffHeapDescriptor* curr = _buckets[bucket_idx].load(std::memory_order_relaxed);
    while (curr != nullptr) {
      if (curr->buffer_address == ptr && !curr->is_evicted.load(std::memory_order_relaxed)) {
        int32_t rem = curr->flow_ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        curr->last_accessed_timestamp_ns.store(hs_get_current_time_ns(), std::memory_order_relaxed);
        if (curr->rjgc_handle != nullptr) {
          RedjetRJGC::release_handle(curr->rjgc_handle);
        }
        if (rem <= 0) {
          do_evict = true;
        }
        break;
      }
      curr = curr->next;
    }
  }
  if (do_evict) {
    evict_immediate(ptr);
  }
}

void RedjetOffHeapReclaimer::touch_flow_ref(void* ptr) {
  if (ptr == nullptr) return;

  size_t bucket_idx = hash_address(ptr);
  hs_ticket_guard_t guard(_bucket_locks[bucket_idx]);
  RedjetOffHeapDescriptor* curr = _buckets[bucket_idx].load(std::memory_order_relaxed);
  while (curr != nullptr) {
    if (curr->buffer_address == ptr) {
      curr->last_accessed_timestamp_ns.store(hs_get_current_time_ns(), std::memory_order_relaxed);
      return;
    }
    curr = curr->next;
  }
}

int32_t RedjetOffHeapReclaimer::get_flow_ref_count(const void* ptr) {
  if (ptr == nullptr) return 0;

  size_t bucket_idx = hash_address(ptr);
  hs_ticket_guard_t guard(_bucket_locks[bucket_idx]);
  RedjetOffHeapDescriptor* curr = _buckets[bucket_idx].load(std::memory_order_relaxed);
  while (curr != nullptr) {
    if (curr->buffer_address == ptr) {
      return curr->flow_ref_count.load(std::memory_order_relaxed);
    }
    curr = curr->next;
  }
  return 0;
}

size_t RedjetOffHeapReclaimer::run_continuous_sweep_cycle() {
  uint64_t start_ns = hs_get_current_time_ns();
  _scanner_state.store(static_cast<uint8_t>(RedjetOffHeapScannerState::SWEEPING), std::memory_order_relaxed);
  _global_reclaimer_epoch.fetch_add(1, std::memory_order_relaxed);

  size_t reclaimed_bytes = 0;
  size_t reclaimed_count = 0;

  // Sweep through all hash buckets under bucket lock
  for (size_t b = 0; b < BUCKET_COUNT; ++b) {
    hs_ticket_guard_t guard(_bucket_locks[b]);
    RedjetOffHeapDescriptor* prev = nullptr;
    RedjetOffHeapDescriptor* curr = _buckets[b].load(std::memory_order_relaxed);

    while (curr != nullptr) {
      RedjetOffHeapDescriptor* next = curr->next;
      bool should_reclaim = false;

      if (curr->mark_policy == RedjetOffHeapMarkPolicy::CONTINUOUS_FLOW_WATCH &&
          !curr->is_evicted.load(std::memory_order_relaxed)) {

        int32_t refs = curr->flow_ref_count.load(std::memory_order_acquire);

        // Check cross-subsystem native registry references
        bool has_native_root = g_redjet_native_registry.is_referenced(curr->buffer_address);

        if (refs <= 0 && !has_native_root) {
          should_reclaim = true;
        }
      }

      if (should_reclaim) {
        size_t sz = curr->size_bytes;
        evict_descriptor(curr);
        reclaimed_bytes += sz;
        reclaimed_count++;

        // Unlink from bucket list
        if (prev == nullptr) {
          _buckets[b].store(next, std::memory_order_release);
        } else {
          prev->next = next;
        }

        if (_active_descriptors_count.load(std::memory_order_relaxed) > 0) {
          _active_descriptors_count.fetch_sub(1, std::memory_order_relaxed);
        }
        release_descriptor(curr);
        curr = next;
      } else {
        prev = curr;
        curr = next;
      }
    }
  }

  // Also trigger RedjetRJGC core continuous polling
  size_t rjgc_bytes = RedjetRJGC::poll_and_evict_continuous();
  reclaimed_bytes += rjgc_bytes;

  uint64_t end_ns = hs_get_current_time_ns();
  _last_sweep_duration_ns.store(end_ns - start_ns, std::memory_order_relaxed);
  _total_sweep_cycles.fetch_add(1, std::memory_order_relaxed);

  _scanner_state.store(static_cast<uint8_t>(RedjetOffHeapScannerState::RUNNING), std::memory_order_relaxed);

  if (reclaimed_bytes > 0) {
    hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO,
                   static_cast<uint32_t>(reclaimed_count),
                   static_cast<uint32_t>(end_ns - start_ns),
                   0, 0, 0, 0,
                   static_cast<uint32_t>(reclaimed_bytes), "RedjetOffHeapReclaimer::sweep_cycle_reclaimed");
  }

  return reclaimed_bytes;
}

void RedjetOffHeapReclaimer::start_scanner_worker() {
  uint8_t expected = static_cast<uint8_t>(RedjetOffHeapScannerState::STOPPED);
  if (_scanner_state.compare_exchange_strong(expected, static_cast<uint8_t>(RedjetOffHeapScannerState::RUNNING), std::memory_order_acq_rel)) {
    hs_hotlog_emit(HS_Ebin0_METASPACE_BOOT, HS_Ebin1_INFO,
                   0, 0, 0, 0, 0, 0,
                   _sweep_interval_ms.load(std::memory_order_relaxed),
                   "RedjetOffHeapReclaimer::scanner_started");
  }
}

void RedjetOffHeapReclaimer::stop_scanner_worker() {
  _scanner_state.store(static_cast<uint8_t>(RedjetOffHeapScannerState::STOPPED), std::memory_order_release);
  hs_hotlog_emit(HS_Ebin0_METASPACE_BOOT, HS_Ebin1_INFO,
                 0, 0, 0, 0, 0, 0, 0,
                 "RedjetOffHeapReclaimer::scanner_stopped");
}

bool RedjetOffHeapReclaimer::is_scanner_running() {
  uint8_t state = _scanner_state.load(std::memory_order_acquire);
  return state == static_cast<uint8_t>(RedjetOffHeapScannerState::RUNNING) ||
         state == static_cast<uint8_t>(RedjetOffHeapScannerState::SWEEPING);
}

void RedjetOffHeapReclaimer::set_sweep_interval_ms(uint32_t interval_ms) {
  _sweep_interval_ms.store((interval_ms > 0) ? interval_ms : 1, std::memory_order_relaxed);
}

uint32_t RedjetOffHeapReclaimer::get_sweep_interval_ms() {
  return _sweep_interval_ms.load(std::memory_order_relaxed);
}

RedjetOffHeapReclaimerStats RedjetOffHeapReclaimer::get_stats() {
  RedjetOffHeapReclaimerStats stats;
  stats.total_immediate_evictions       = _total_immediate_evictions.load(std::memory_order_relaxed);
  stats.total_continuous_registrations  = _total_continuous_registrations.load(std::memory_order_relaxed);
  stats.total_continuous_reclamations   = _total_continuous_reclamations.load(std::memory_order_relaxed);
  stats.total_bytes_reclaimed           = _total_bytes_reclaimed.load(std::memory_order_relaxed);
  stats.total_sweep_cycles              = _total_sweep_cycles.load(std::memory_order_relaxed);
  stats.last_sweep_duration_ns          = _last_sweep_duration_ns.load(std::memory_order_relaxed);
  stats.active_descriptors_count        = _active_descriptors_count.load(std::memory_order_relaxed);
  stats.canary_faults_detected          = _canary_faults_detected.load(std::memory_order_relaxed);
  return stats;
}

void RedjetOffHeapReclaimer::reset_stats() {
  _total_immediate_evictions.store(0, std::memory_order_relaxed);
  _total_continuous_registrations.store(0, std::memory_order_relaxed);
  _total_continuous_reclamations.store(0, std::memory_order_relaxed);
  _total_bytes_reclaimed.store(0, std::memory_order_relaxed);
  _total_sweep_cycles.store(0, std::memory_order_relaxed);
  _last_sweep_duration_ns.store(0, std::memory_order_relaxed);
  _canary_faults_detected.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* C ABI EXPORTS FOR REDJET OFF-HEAP LIFECYCLE MANAGEMENT                    */
/* ========================================================================= */

extern "C" {

RJ_API void* hs_offheap_register(void* ptr, size_t size, const char* mark_policy_str) {
  return RedjetOffHeapReclaimer::register_by_mark_string(ptr, size, mark_policy_str);
}

RJ_API bool hs_offheap_evict_immediate(void* ptr) {
  return RedjetOffHeapReclaimer::evict_immediate(ptr);
}

RJ_API void hs_offheap_retain(void* ptr) {
  RedjetOffHeapReclaimer::retain_flow_ref(ptr);
}

RJ_API void hs_offheap_release(void* ptr) {
  RedjetOffHeapReclaimer::release_flow_ref(ptr);
}

RJ_API size_t hs_offheap_sweep_cycle(void) {
  return RedjetOffHeapReclaimer::run_continuous_sweep_cycle();
}

RJ_API void hs_offheap_scanner_start(void) {
  RedjetOffHeapReclaimer::start_scanner_worker();
}

RJ_API void hs_offheap_scanner_stop(void) {
  RedjetOffHeapReclaimer::stop_scanner_worker();
}

RJ_API size_t hs_offheap_get_reclaimed_bytes(void) {
  return RedjetOffHeapReclaimer::get_stats().total_bytes_reclaimed;
}

} // extern "C"
