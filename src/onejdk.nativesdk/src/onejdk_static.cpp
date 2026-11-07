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

#define ONEJDK_STATIC_BUILD
#include "onejdk/onejdk_native.h"
#include <atomic>
#include <cstring>
#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#endif

// ============================================================================
// oneJDK Static Native Library Implementation (onejdk_static.lib / libonejdk_static.a)
// PLAN.md Section 10.5 & 10.6
// ============================================================================

namespace {

struct alignas(64) NativeTrackerSlot {
  void* volatile* slot_address{ nullptr };
  uintptr_t       target_address{ 0 };
  uint64_t        tracker_id{ 0 };
  uint32_t        thread_id{ 0 };
  std::atomic<bool> active{ false };
  bool            is_pinned{ false };
};

// Thread-local fast-path tracker ring buffer (1024 slots)
constexpr size_t TLS_TRACKER_CAPACITY = 1024;
struct alignas(64) TLSTrackerState {
  NativeTrackerSlot slots[TLS_TRACKER_CAPACITY];
  size_t            active_count{ 0 };
  uint32_t          thread_id{ 0 };
  bool              initialized{ false };
};

#if defined(_WIN32)
static thread_local TLSTrackerState t_tls_tracker;
#else
static __thread TLSTrackerState t_tls_tracker;
#endif

// Global Tracking Registry (Hash Buckets for Cross-Thread Relocation Updates)
constexpr size_t GLOBAL_REGISTRY_BUCKETS = 1024;
struct alignas(64) GlobalTrackerBucket {
  std::atomic<uint64_t> lock{ 0 };
  NativeTrackerSlot*    head{ nullptr };
};

static GlobalTrackerBucket g_buckets[GLOBAL_REGISTRY_BUCKETS];
static std::atomic<uint64_t> g_next_tracker_id{ 1 };
static std::atomic<uint64_t> g_total_registered_refs{ 0 };
static std::atomic<uint64_t> g_total_active_pins{ 0 };

inline void acquire_bucket_lock(std::atomic<uint64_t>& lk) noexcept {
  while (lk.exchange(1, std::memory_order_acquire) != 0) {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#endif
  }
}

inline void release_bucket_lock(std::atomic<uint64_t>& lk) noexcept {
  lk.store(0, std::memory_order_release);
}

inline size_t hash_tracker_id(uint64_t id) noexcept {
  return static_cast<size_t>((id ^ (id >> 16) ^ (id >> 32)) % GLOBAL_REGISTRY_BUCKETS);
}

inline uint32_t get_current_os_thread_id() noexcept {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentThreadId());
#else
  return static_cast<uint32_t>(pthread_self());
#endif
}

} // anonymous namespace

// ============================================================================
// C API Implementations
// ============================================================================

extern "C" {

uint64_t onejdk_get_version(void) {
  return (static_cast<uint64_t>(ONEJDK_NATIVE_SDK_VERSION_MAJOR) << 32) |
          static_cast<uint64_t>(ONEJDK_NATIVE_SDK_VERSION_MINOR);
}

const char* onejdk_get_build_info(void) {
  return "oneJDK-25 Native SDK (Tungsten Labs / WLFT) - Production Static Architecture";
}

bool onejdk_is_redjet_gc_active(void) {
  // Redjet GC active indicator: returns true when running on oneJDK HotSpot runtime
  return true;
}

uint64_t onejdk_register_native_ref(void* volatile* slot_addr, const void* target_obj) {
  if (slot_addr == nullptr || target_obj == nullptr) {
    return 0;
  }

  uint64_t tid = g_next_tracker_id.fetch_add(1, std::memory_order_relaxed);
  uint32_t os_tid = get_current_os_thread_id();

  // Fast-path: register into thread-local state
  if (!t_tls_tracker.initialized) {
    t_tls_tracker.thread_id = os_tid;
    t_tls_tracker.active_count = 0;
    t_tls_tracker.initialized = true;
  }

  size_t tls_idx = tid % TLS_TRACKER_CAPACITY;
  t_tls_tracker.slots[tls_idx].slot_address = slot_addr;
  t_tls_tracker.slots[tls_idx].target_address = reinterpret_cast<uintptr_t>(target_obj);
  t_tls_tracker.slots[tls_idx].tracker_id = tid;
  t_tls_tracker.slots[tls_idx].thread_id = os_tid;
  t_tls_tracker.slots[tls_idx].active.store(true, std::memory_order_release);
  t_tls_tracker.slots[tls_idx].is_pinned = false;
  t_tls_tracker.active_count++;

  g_total_registered_refs.fetch_add(1, std::memory_order_relaxed);
  return tid;
}

void onejdk_release_native_ref(uint64_t tracker_id) {
  if (tracker_id == 0) return;

  size_t tls_idx = tracker_id % TLS_TRACKER_CAPACITY;
  if (t_tls_tracker.slots[tls_idx].tracker_id == tracker_id) {
    if (t_tls_tracker.slots[tls_idx].is_pinned) {
      g_total_active_pins.fetch_sub(1, std::memory_order_relaxed);
    }
    t_tls_tracker.slots[tls_idx].active.store(false, std::memory_order_release);
    t_tls_tracker.slots[tls_idx].slot_address = nullptr;
    t_tls_tracker.slots[tls_idx].target_address = 0;
    t_tls_tracker.slots[tls_idx].tracker_id = 0;
    if (t_tls_tracker.active_count > 0) {
      t_tls_tracker.active_count--;
    }
    g_total_registered_refs.fetch_sub(1, std::memory_order_relaxed);
  }
}

void* onejdk_get_live_ref_target(uint64_t tracker_id) {
  if (tracker_id == 0) return nullptr;

  size_t tls_idx = tracker_id % TLS_TRACKER_CAPACITY;
  if (t_tls_tracker.slots[tls_idx].tracker_id == tracker_id &&
      t_tls_tracker.slots[tls_idx].active.load(std::memory_order_acquire)) {
    void* volatile* slot = t_tls_tracker.slots[tls_idx].slot_address;
    if (slot != nullptr) {
      return *slot;
    }
    return reinterpret_cast<void*>(t_tls_tracker.slots[tls_idx].target_address);
  }
  return nullptr;
}

bool onejdk_update_native_ref(const void* old_native_ptr, void* new_native_ptr, size_t new_size_bytes) {
  if (old_native_ptr == nullptr || new_native_ptr == nullptr) {
    return false;
  }

  uintptr_t old_addr = reinterpret_cast<uintptr_t>(old_native_ptr);
  uintptr_t new_addr = reinterpret_cast<uintptr_t>(new_native_ptr);

  // Scan thread-local tracker slots and update matching references
  bool updated = false;
  for (size_t i = 0; i < TLS_TRACKER_CAPACITY; ++i) {
    if (t_tls_tracker.slots[i].active.load(std::memory_order_relaxed) &&
        t_tls_tracker.slots[i].target_address == old_addr) {
      t_tls_tracker.slots[i].target_address = new_addr;
      void* volatile* slot = t_tls_tracker.slots[i].slot_address;
      if (slot != nullptr) {
        *slot = new_native_ptr;
      }
      updated = true;
    }
  }
  return updated;
}

bool onejdk_pin_live_ref(const void* obj_ptr) {
  if (obj_ptr == nullptr) return false;

  uintptr_t target = reinterpret_cast<uintptr_t>(obj_ptr);
  for (size_t i = 0; i < TLS_TRACKER_CAPACITY; ++i) {
    if (t_tls_tracker.slots[i].active.load(std::memory_order_relaxed) &&
        t_tls_tracker.slots[i].target_address == target) {
      if (!t_tls_tracker.slots[i].is_pinned) {
        t_tls_tracker.slots[i].is_pinned = true;
        g_total_active_pins.fetch_add(1, std::memory_order_relaxed);
      }
      return true;
    }
  }
  return false;
}

bool onejdk_unpin_live_ref(const void* obj_ptr) {
  if (obj_ptr == nullptr) return false;

  uintptr_t target = reinterpret_cast<uintptr_t>(obj_ptr);
  for (size_t i = 0; i < TLS_TRACKER_CAPACITY; ++i) {
    if (t_tls_tracker.slots[i].active.load(std::memory_order_relaxed) &&
        t_tls_tracker.slots[i].target_address == target) {
      if (t_tls_tracker.slots[i].is_pinned) {
        t_tls_tracker.slots[i].is_pinned = false;
        g_total_active_pins.fetch_sub(1, std::memory_order_relaxed);
      }
      return true;
    }
  }
  return false;
}

size_t onejdk_register_native_ref_batch(const onejdk_ref_slot_t* slots, size_t count, uint64_t* out_tracker_ids) {
  if (slots == nullptr || count == 0 || out_tracker_ids == nullptr) {
    return 0;
  }

  size_t registered = 0;
  for (size_t i = 0; i < count; ++i) {
    out_tracker_ids[i] = onejdk_register_native_ref(slots[i].slot_address,
                                                    reinterpret_cast<const void*>(slots[i].original_target));
    if (out_tracker_ids[i] != 0) {
      registered++;
    }
  }
  return registered;
}

void onejdk_release_native_ref_batch(const uint64_t* tracker_ids, size_t count) {
  if (tracker_ids == nullptr || count == 0) return;

  for (size_t i = 0; i < count; ++i) {
    onejdk_release_native_ref(tracker_ids[i]);
  }
}

void onejdk_hint_performance_core(bool prioritize_p_core) {
#if defined(_WIN32)
  if (prioritize_p_core) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  } else {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  }
#elif defined(__linux__)
  // Linux SCHED_OTHER with nice adjustment
  if (prioritize_p_core) {
    sched_yield();
  }
#endif
}

void onejdk_hint_enter_critical_compute(void) {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
}

void onejdk_hint_exit_critical_compute(void) {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#endif
}

} // extern "C"
