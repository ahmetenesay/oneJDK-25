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
#include "gc/redjet/api/redjetFastFFM.hpp"
#include "gc/redjet/api/redjetNativeSDK.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "gc/redjet/vfs/redjetVFSNode_ReservedBoot.hpp"
#include "gc/shared/gc_globals.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include <cstdlib>
#include <cstring>

std::atomic<uint64_t> RedjetFastFFM::_total_segments_allocated{ 0 };
std::atomic<uint64_t> RedjetFastFFM::_total_segments_freed_immediate{ 0 };
std::atomic<uint64_t> RedjetFastFFM::_total_segments_freed_flow_watch{ 0 };
std::atomic<uint64_t> RedjetFastFFM::_total_reserved_boot_borrows{ 0 };
std::atomic<uint64_t> RedjetFastFFM::_total_zero_copy_downcalls{ 0 };
std::atomic<uint64_t> RedjetFastFFM::_total_bidirectional_realloc_syncs{ 0 };
std::atomic<uint64_t> RedjetFastFFM::_active_offheap_java_bytes{ 0 };

namespace {

inline size_t align_up_pow2(size_t val, size_t align) noexcept {
  size_t a = (align < 64) ? 64 : align;
  if ((a & (a - 1)) != 0) {
    // Round non-power-of-two alignment up to next power of two
    size_t p = 64;
    while (p < a) {
      p <<= 1;
    }
    a = p;
  }
  return (val + (a - 1)) & ~(a - 1);
}

inline RedjetFFMSegmentHeader* header_from_payload(void* payload_ptr) noexcept {
  if (payload_ptr == nullptr) {
    return nullptr;
  }
  auto* hdr = reinterpret_cast<RedjetFFMSegmentHeader*>(
      reinterpret_cast<uint8_t*>(payload_ptr) - sizeof(RedjetFFMSegmentHeader));
  if (hdr->magic != RedjetFFMSegmentHeader::SEGMENT_MAGIC) {
    return nullptr;
  }
  return hdr;
}

} // anonymous namespace

void RedjetFastFFM::initialize() {
  RedjetOffHeapReclaimer::initialize();
  _total_segments_allocated.store(0, std::memory_order_relaxed);
  _total_segments_freed_immediate.store(0, std::memory_order_relaxed);
  _total_segments_freed_flow_watch.store(0, std::memory_order_relaxed);
  _total_reserved_boot_borrows.store(0, std::memory_order_relaxed);
  _total_zero_copy_downcalls.store(0, std::memory_order_relaxed);
  _total_bidirectional_realloc_syncs.store(0, std::memory_order_relaxed);
  _active_offheap_java_bytes.store(0, std::memory_order_relaxed);
}

void RedjetFastFFM::shutdown() {
  RedjetOffHeapReclaimer::run_continuous_sweep_cycle();
}

bool RedjetFastFFM::is_enabled() noexcept {
#if INCLUDE_REDJETGC
  return UseRedjetGC;
#else
  return false;
#endif
}

void RedjetFastFFM::release_raw_segment_storage(void* payload_ptr, size_t /*sz*/) {
  RedjetFFMSegmentHeader* hdr = header_from_payload(payload_ptr);
  if (hdr == nullptr) {
    return;
  }

  size_t total_bytes = hdr->total_reserved_bytes;
  bool borrowed = hdr->from_reserved_boot;
  void* raw_base = hdr->raw_allocation_base;
  RedjetOffHeapMarkPolicy policy = hdr->mark_policy;

  // Poison magic to prevent double-free
  hdr->magic = 0;

  RedjetHeap* heap = RedjetHeap::heap();
  if (heap != nullptr && heap->vfs_tree() != nullptr) {
    heap->vfs_tree()->record_domain_free(RedjetMemoryDomain::JAVA_MEMORY, false, total_bytes);
  }

  uint64_t prev = _active_offheap_java_bytes.load(std::memory_order_relaxed);
  while (prev >= total_bytes &&
         !_active_offheap_java_bytes.compare_exchange_weak(prev, prev - total_bytes, std::memory_order_relaxed)) {
  }

  if (policy == RedjetOffHeapMarkPolicy::CONTINUOUS_FLOW_WATCH) {
    _total_segments_freed_flow_watch.fetch_add(1, std::memory_order_relaxed);
  } else {
    _total_segments_freed_immediate.fetch_add(1, std::memory_order_relaxed);
  }

  if (!borrowed && raw_base != nullptr) {
    os::free(raw_base);
  }
}

void* RedjetFastFFM::allocate_segment(size_t byte_size,
                                      size_t byte_alignment,
                                      RedjetOffHeapMarkPolicy policy,
                                      bool zero_initialize,
                                      uint32_t owner_tid) noexcept {
  if (!is_enabled() || byte_size == 0) {
    return nullptr;
  }

  size_t eff_align = (byte_alignment < 64) ? 64 : byte_alignment;
  size_t total_needed = align_up_pow2(sizeof(RedjetFFMSegmentHeader) + byte_size + eff_align, 64);

  void* raw_base = os::malloc(total_needed, mtGC);
  bool from_reserved = false;

  RedjetHeap* heap = RedjetHeap::heap();
  if (raw_base == nullptr && heap != nullptr && heap->vfs_tree() != nullptr) {
    RedjetVFSNode_ReservedBoot* rboot = heap->vfs_tree()->reserved_boot_node();
    if (rboot != nullptr) {
      raw_base = rboot->borrow_emergency_headroom(total_needed);
      if (raw_base != nullptr) {
        from_reserved = true;
        _total_reserved_boot_borrows.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  if (raw_base == nullptr) {
    return nullptr;
  }

  uintptr_t earliest_payload = reinterpret_cast<uintptr_t>(raw_base) + sizeof(RedjetFFMSegmentHeader);
  uintptr_t aligned_payload = align_up_pow2(earliest_payload, eff_align);
  void* payload_ptr = reinterpret_cast<void*>(aligned_payload);

  if (zero_initialize) {
    std::memset(payload_ptr, 0, byte_size);
  }

  auto* hdr = reinterpret_cast<RedjetFFMSegmentHeader*>(
      reinterpret_cast<uint8_t*>(payload_ptr) - sizeof(RedjetFFMSegmentHeader));
  hdr->magic = RedjetFFMSegmentHeader::SEGMENT_MAGIC;
  hdr->raw_allocation_base = raw_base;
  hdr->payload_bytes = byte_size;
  hdr->total_reserved_bytes = total_needed;
  hdr->alignment = static_cast<uint32_t>(eff_align);
  hdr->mark_policy = policy;
  hdr->from_reserved_boot = from_reserved;
  hdr->reclaimer_desc = RedjetOffHeapReclaimer::register_allocation(
      payload_ptr,
      byte_size,
      policy,
      &RedjetFastFFM::release_raw_segment_storage,
      owner_tid,
      static_cast<uint32_t>(eff_align));

  if (heap != nullptr && heap->vfs_tree() != nullptr) {
    heap->vfs_tree()->record_domain_alloc(RedjetMemoryDomain::JAVA_MEMORY, false, total_needed);
  }
  _active_offheap_java_bytes.fetch_add(total_needed, std::memory_order_relaxed);
  _total_segments_allocated.fetch_add(1, std::memory_order_relaxed);

  return payload_ptr;
}

bool RedjetFastFFM::free_segment_immediate(void* segment_payload_ptr) noexcept {
  if (!is_enabled() || segment_payload_ptr == nullptr) {
    return false;
  }
  RedjetFFMSegmentHeader* hdr = header_from_payload(segment_payload_ptr);
  if (hdr == nullptr) {
    return false;
  }
  if (hdr->reclaimer_desc != nullptr) {
    return RedjetOffHeapReclaimer::evict_descriptor(hdr->reclaimer_desc);
  }
  release_raw_segment_storage(segment_payload_ptr, hdr->payload_bytes);
  return true;
}

void RedjetFastFFM::on_tailtrace_segment_dead(void* segment_payload_ptr) noexcept {
  RedjetFFMSegmentHeader* hdr = header_from_payload(segment_payload_ptr);
  if (hdr == nullptr) {
    return;
  }
  if (hdr->mark_policy == RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION) {
    free_segment_immediate(segment_payload_ptr);
  } else {
    RedjetOffHeapReclaimer::release_flow_ref(segment_payload_ptr);
  }
}

void* RedjetFastFFM::sync_native_realloc(void* old_payload_ptr, size_t new_byte_size) noexcept {
  if (!is_enabled() || new_byte_size == 0) {
    return nullptr;
  }
  if (old_payload_ptr == nullptr) {
    return allocate_segment(new_byte_size, 64, RedjetOffHeapMarkPolicy::CONTINUOUS_FLOW_WATCH, true, 0);
  }

  RedjetFFMSegmentHeader* old_hdr = header_from_payload(old_payload_ptr);
  if (old_hdr == nullptr) {
    return nullptr;
  }

  size_t copy_bytes = (old_hdr->payload_bytes < new_byte_size) ? old_hdr->payload_bytes : new_byte_size;
  RedjetOffHeapMarkPolicy policy = old_hdr->mark_policy;
  uint32_t align = old_hdr->alignment;

  void* new_payload = allocate_segment(new_byte_size, align, policy, false, 0);
  if (new_payload == nullptr) {
    return nullptr;
  }

  std::memcpy(new_payload, old_payload_ptr, copy_bytes);
  if (new_byte_size > copy_bytes) {
    std::memset(reinterpret_cast<uint8_t*>(new_payload) + copy_bytes, 0, new_byte_size - copy_bytes);
  }

  // Synchronize live Java MemorySegment / native live_ref slots atomically!
  RedjetNativeTracker::update_native_memory_segment(old_payload_ptr, new_payload, new_byte_size);
  free_segment_immediate(old_payload_ptr);

  _total_bidirectional_realloc_syncs.fetch_add(1, std::memory_order_relaxed);
  return new_payload;
}

RedjetFastFFMTelemetry RedjetFastFFM::snapshot_telemetry() noexcept {
  RedjetFastFFMTelemetry out{};
  out.total_segments_allocated = _total_segments_allocated.load(std::memory_order_relaxed);
  out.total_segments_freed_immediate = _total_segments_freed_immediate.load(std::memory_order_relaxed);
  out.total_segments_freed_flow_watch = _total_segments_freed_flow_watch.load(std::memory_order_relaxed);
  out.total_reserved_boot_borrows = _total_reserved_boot_borrows.load(std::memory_order_relaxed);
  out.total_zero_copy_downcalls = _total_zero_copy_downcalls.load(std::memory_order_relaxed);
  out.total_bidirectional_realloc_syncs = _total_bidirectional_realloc_syncs.load(std::memory_order_relaxed);
  out.active_offheap_java_bytes = _active_offheap_java_bytes.load(std::memory_order_relaxed);
  return out;
}

extern "C" {

RJ_API void* hs_redjet_ffm_allocate(size_t bytes, size_t alignment, const char* rjgc_mark) {
  RedjetOffHeapMarkPolicy policy = RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION;
  if (rjgc_mark != nullptr && std::strcmp(rjgc_mark, "ContinuousFlowWatch") == 0) {
    policy = RedjetOffHeapMarkPolicy::CONTINUOUS_FLOW_WATCH;
  }
  return RedjetFastFFM::allocate_segment(bytes, alignment, policy, true, 0);
}

RJ_API bool hs_redjet_ffm_free_immediate(void* segment_ptr) {
  return RedjetFastFFM::free_segment_immediate(segment_ptr);
}

RJ_API void* hs_redjet_ffm_realloc_sync(void* old_segment_ptr, size_t new_bytes) {
  return RedjetFastFFM::sync_native_realloc(old_segment_ptr, new_bytes);
}

} // extern "C"
