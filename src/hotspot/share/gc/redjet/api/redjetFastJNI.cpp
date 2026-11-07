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

#include "precompiled.hpp"
#include "gc/redjet/api/redjetFastJNI.hpp"
#include "gc/redjet/api/redjetNativeSDK.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "gc/redjet/vfs/redjetVFSNode_ParkArena.hpp"
#include "gc/shared/gc_globals.hpp"
#include "runtime/javaThread.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace {

// Tag bit 0x1 marks a Redjet TLS fast handle vs standard HotSpot jobject pointer
constexpr uintptr_t FAST_HANDLE_TAG = 0x1ULL;

struct alignas(64) TLSFastHandleFrame {
  void*    oop_slots[RedjetFastJNI::TLS_FAST_HANDLE_CAPACITY]{ nullptr };
  uint64_t tracker_ids[RedjetFastJNI::TLS_FAST_HANDLE_CAPACITY]{ 0 };
  uint32_t top{ 0 };
};

#if defined(_WIN32)
static thread_local TLSFastHandleFrame t_fast_handles;
#else
static __thread TLSFastHandleFrame t_fast_handles;
#endif

inline uint64_t read_fast_tsc() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  return static_cast<uint64_t>(__rdtsc());
#elif defined(__aarch64__)
  uint64_t v = 0;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  return 0;
#endif
}

inline uint32_t current_tid_u32(JavaThread* jt) noexcept {
  if (jt != nullptr) {
    uintptr_t p = reinterpret_cast<uintptr_t>(jt);
    return static_cast<uint32_t>((p >> 4) ^ (p >> 20));
  }
  return 1U;
}

} // anonymous namespace

std::atomic<uint64_t> RedjetFastJNI::_total_fast_downcalls{ 0 };
std::atomic<uint64_t> RedjetFastJNI::_total_parkpoint_slowpaths{ 0 };
std::atomic<uint64_t> RedjetFastJNI::_total_critical_pins{ 0 };
std::atomic<uint64_t> RedjetFastJNI::_total_critical_unpins{ 0 };
std::atomic<uint64_t> RedjetFastJNI::_total_emptybox_returns{ 0 };
std::atomic<uint64_t> RedjetFastJNI::_total_tls_fast_handles{ 0 };
std::atomic<uint64_t> RedjetFastJNI::_last_downcall_cycles{ 0 };

void RedjetFastJNI::initialize() {
  _total_fast_downcalls.store(0, std::memory_order_relaxed);
  _total_parkpoint_slowpaths.store(0, std::memory_order_relaxed);
  _total_critical_pins.store(0, std::memory_order_relaxed);
  _total_critical_unpins.store(0, std::memory_order_relaxed);
  _total_emptybox_returns.store(0, std::memory_order_relaxed);
  _total_tls_fast_handles.store(0, std::memory_order_relaxed);
  _last_downcall_cycles.store(0, std::memory_order_relaxed);
}

void RedjetFastJNI::shutdown() {
  rewind_tls_fast_handles(0);
}

bool RedjetFastJNI::is_enabled() noexcept {
#if INCLUDE_REDJETGC
  return UseRedjetGC;
#else
  return false;
#endif
}

RedjetFastJNIScopeToken RedjetFastJNI::enter_fast_downcall(JavaThread* current_thread) noexcept {
  RedjetFastJNIScopeToken token{};
  if (!is_enabled()) {
    return token;
  }

  token.thread = current_thread;
  token.enter_tsc = read_fast_tsc();
  token.tls_handle_watermark = t_fast_handles.top;
  token.redjet_fast_active = true;

  if (current_thread != nullptr) {
    RedjetPointCoordinator::on_jni_park_enter(current_thread);
  }
  _total_fast_downcalls.fetch_add(1, std::memory_order_relaxed);
  return token;
}

void RedjetFastJNI::exit_fast_downcall(const RedjetFastJNIScopeToken& token) noexcept {
  if (!token.redjet_fast_active) {
    return;
  }

  // Rewind any ephemeral TLS handles opened during this native call
  if (t_fast_handles.top > token.tls_handle_watermark) {
    rewind_tls_fast_handles(token.tls_handle_watermark);
  }

  // 0.5-cycle L1-D Parkpoint check instead of serializing OrderAccess::fence()
  if (RedjetPointCoordinator::is_parkpoint_active()) {
    _total_parkpoint_slowpaths.fetch_add(1, std::memory_order_relaxed);
    if (token.thread != nullptr) {
      RedjetPointCoordinator::parkpoint_poll(token.thread, nullptr);
    }
  }

  if (token.thread != nullptr) {
    RedjetPointCoordinator::on_jni_park_exit(token.thread);
  }

  uint64_t end_tsc = read_fast_tsc();
  if (end_tsc > token.enter_tsc) {
    _last_downcall_cycles.store(end_tsc - token.enter_tsc, std::memory_order_relaxed);
  }
}

void* RedjetFastJNI::pin_onheap_critical_region(JavaThread* current_thread,
                                                void* java_obj_base,
                                                size_t payload_offset_bytes,
                                                size_t byte_size,
                                                jboolean* is_copy_out) noexcept {
  if (is_copy_out != nullptr) {
    *is_copy_out = JNI_FALSE; // Always zero-copy under RedjetGC!
  }
  if (!is_enabled() || java_obj_base == nullptr) {
    return nullptr;
  }

  RedjetHeap* heap = RedjetHeap::heap();
  uint32_t tid = current_tid_u32(current_thread);
  size_t total_obj_bytes = payload_offset_bytes + byte_size;

  if (heap != nullptr && heap->vfs_tree() != nullptr) {
    RedjetVFSNode_ParkArena* park_arena = heap->vfs_tree()->park_arena_node();
    if (park_arena != nullptr) {
      RedjetParkProvenanceHeader prov{};
      prov.magic = 0x5041524BU; // "PARK"
      prov.origin_branch = RedjetParkOriginBranch::ORIGIN_BRANCH_HOTFIELD;
      prov.entry_tsc = read_fast_tsc();
      prov.native_thread_id = tid;
      prov.original_slot_addr = java_obj_base;
      prov.parked_object_ptr = java_obj_base;
      prov.object_size = total_obj_bytes;

      park_arena->park_object_with_provenance(java_obj_base, total_obj_bytes, tid, prov, 0);
    }
  }

  RedjetPointCoordinator::request_object_park(java_obj_base);
  _total_critical_pins.fetch_add(1, std::memory_order_relaxed);

  return reinterpret_cast<uint8_t*>(java_obj_base) + payload_offset_bytes;
}

bool RedjetFastJNI::unpin_onheap_critical_region(JavaThread* current_thread,
                                                 void* java_obj_base,
                                                 void* pinned_payload_ptr,
                                                 jint release_mode) noexcept {
  (void)pinned_payload_ptr;
  if (!is_enabled() || java_obj_base == nullptr) {
    return false;
  }

  // JNI_COMMIT copies back without unpinning; since RedjetGC is zero-copy,
  // mutations are already live in memory, so JNI_COMMIT keeps the pin active.
  if (release_mode == JNI_COMMIT) {
    return true;
  }

  RedjetHeap* heap = RedjetHeap::heap();
  uint32_t tid = current_tid_u32(current_thread);
  bool returned_via_emptybox = false;

  if (heap != nullptr && heap->vfs_tree() != nullptr) {
    RedjetVFSNode_ParkArena* park_arena = heap->vfs_tree()->park_arena_node();
    if (park_arena != nullptr) {
      returned_via_emptybox = park_arena->unpin_and_return_object(
          java_obj_base,
          tid,
          heap->vfs_tree()->hotfield_node(),
          heap->runway());
    }
  }

  RedjetPointCoordinator::release_object_park(java_obj_base);
  _total_critical_unpins.fetch_add(1, std::memory_order_relaxed);
  if (returned_via_emptybox) {
    _total_emptybox_returns.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

jobject RedjetFastJNI::acquire_tls_fast_handle(void* raw_oop_ptr) noexcept {
  if (!is_enabled() || raw_oop_ptr == nullptr) {
    return nullptr;
  }
  if (t_fast_handles.top >= TLS_FAST_HANDLE_CAPACITY) {
    return nullptr; // Caller falls back to standard JNIHandles::make_local
  }

  uint32_t idx = t_fast_handles.top++;
  t_fast_handles.oop_slots[idx] = raw_oop_ptr;
  t_fast_handles.tracker_ids[idx] = RedjetNativeTracker::thread_tracker().register_live_slot(
      &t_fast_handles.oop_slots[idx], raw_oop_ptr);

  _total_tls_fast_handles.fetch_add(1, std::memory_order_relaxed);

  uintptr_t tagged = reinterpret_cast<uintptr_t>(&t_fast_handles.oop_slots[idx]) | FAST_HANDLE_TAG;
  return reinterpret_cast<jobject>(tagged);
}

void* RedjetFastJNI::resolve_tls_fast_handle(jobject handle) noexcept {
  if (handle == nullptr) {
    return nullptr;
  }
  uintptr_t raw = reinterpret_cast<uintptr_t>(handle);
  if ((raw & FAST_HANDLE_TAG) != 0) {
    void* const* slot_addr = reinterpret_cast<void* const*>(raw & ~FAST_HANDLE_TAG);
    return *slot_addr;
  }
  return *reinterpret_cast<void* const*>(handle);
}

void RedjetFastJNI::rewind_tls_fast_handles(uint32_t watermark) noexcept {
  RedjetNativeTracker& tracker = RedjetNativeTracker::thread_tracker();
  while (t_fast_handles.top > watermark) {
    uint32_t idx = --t_fast_handles.top;
    if (t_fast_handles.tracker_ids[idx] != 0) {
      tracker.unregister_live_slot(t_fast_handles.tracker_ids[idx], &t_fast_handles.oop_slots[idx]);
      t_fast_handles.tracker_ids[idx] = 0;
    }
    t_fast_handles.oop_slots[idx] = nullptr;
  }
}

RedjetFastJNITelemetry RedjetFastJNI::snapshot_telemetry() noexcept {
  RedjetFastJNITelemetry out{};
  out.total_fast_downcalls = _total_fast_downcalls.load(std::memory_order_relaxed);
  out.total_parkpoint_slowpaths = _total_parkpoint_slowpaths.load(std::memory_order_relaxed);
  out.total_critical_pins = _total_critical_pins.load(std::memory_order_relaxed);
  out.total_critical_unpins = _total_critical_unpins.load(std::memory_order_relaxed);
  out.total_emptybox_returns = _total_emptybox_returns.load(std::memory_order_relaxed);
  out.total_tls_fast_handles = _total_tls_fast_handles.load(std::memory_order_relaxed);
  out.last_downcall_cycles = _last_downcall_cycles.load(std::memory_order_relaxed);

  RedjetHeap* heap = is_enabled() ? RedjetHeap::heap() : nullptr;
  if (heap != nullptr && heap->vfs_tree() != nullptr && heap->vfs_tree()->park_arena_node() != nullptr) {
    out.active_pinned_objects = heap->vfs_tree()->park_arena_node()->pinned_object_count();
  }
  return out;
}

extern "C" {

RJ_API void* hs_redjet_fast_jni_pin_critical(void* java_obj, size_t payload_offset, size_t bytes) {
  return RedjetFastJNI::pin_onheap_critical_region(nullptr, java_obj, payload_offset, bytes, nullptr);
}

RJ_API bool hs_redjet_fast_jni_unpin_critical(void* java_obj, void* payload_ptr, int mode) {
  return RedjetFastJNI::unpin_onheap_critical_region(nullptr, java_obj, payload_ptr, static_cast<jint>(mode));
}

} // extern "C"
