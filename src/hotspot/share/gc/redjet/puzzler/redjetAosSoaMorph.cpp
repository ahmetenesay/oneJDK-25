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

#include "gc/redjet/puzzler/redjetAosSoaMorph.hpp"
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

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetAosSoaMorph::RedjetAosSoaMorph() {
  _pitstop_buffer_pool = nullptr;
  _pool_capacity = 0;
  _pool_offset.store(0, std::memory_order_relaxed);

  _total_aos_to_soa_transforms.store(0, std::memory_order_relaxed);
  _total_elements_sliced.store(0, std::memory_order_relaxed);
  _total_mutations_written_back.store(0, std::memory_order_relaxed);
  _total_scatter_operations.store(0, std::memory_order_relaxed);
  _total_bytes_transferred.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < MAX_CONCURRENT_SESSIONS; ++i) {
    _sessions[i].reset();
  }
}

RedjetAosSoaMorph::~RedjetAosSoaMorph() {
  destroy();
}

void RedjetAosSoaMorph::init(void* pitstop_pool, size_t pool_bytes) {
  hs_ticket_guard_t guard(_morph_lock);
  _pitstop_buffer_pool = static_cast<uint8_t*>(pitstop_pool);
  _pool_capacity = pool_bytes;
  _pool_offset.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < MAX_CONCURRENT_SESSIONS; ++i) {
    _sessions[i].reset();
  }
}

void RedjetAosSoaMorph::destroy() {
  hs_ticket_guard_t guard(_morph_lock);
  for (size_t i = 0; i < MAX_CONCURRENT_SESSIONS; ++i) {
    if (_sessions[i].is_active.load(std::memory_order_relaxed)) {
      write_back_all(static_cast<uint32_t>(i));
      _sessions[i].reset();
    }
  }
  _pitstop_buffer_pool = nullptr;
  _pool_capacity = 0;
  _pool_offset.store(0, std::memory_order_relaxed);
}

uint8_t* RedjetAosSoaMorph::allocate_slice_storage(size_t bytes) {
  if (_pitstop_buffer_pool == nullptr || bytes == 0) {
    return nullptr;
  }
  // 64-byte alignment enforcement for AVX-512 / AVX2 streaming
  size_t aligned = (bytes + 63) & ~static_cast<size_t>(63);
  size_t prev = _pool_offset.fetch_add(aligned, std::memory_order_relaxed);
  if (prev + aligned <= _pool_capacity) {
    return _pitstop_buffer_pool + prev;
  }
  return nullptr; // Pool exhausted
}

// =========================================================================
// 1. Transform AOS Array to SOA Slice Planes (Dynamic Shadow Slicing)
// Non-blocking shadow copy of target fields into contiguous SIMD planes
// =========================================================================
uint32_t RedjetAosSoaMorph::slice_aos_to_soa(uintptr_t aos_base,
                                             size_t element_count,
                                             size_t aos_stride,
                                             const RedjetFieldDescriptor* fields,
                                             size_t field_count) {
  if (aos_base == 0 || element_count == 0 || aos_stride == 0 ||
      fields == nullptr || field_count == 0) {
    return UINT32_MAX;
  }

  hs_ticket_guard_t guard(_morph_lock);

  // Find free session slot
  uint32_t session_id = UINT32_MAX;
  for (size_t i = 0; i < MAX_CONCURRENT_SESSIONS; ++i) {
    if (!_sessions[i].is_active.load(std::memory_order_relaxed)) {
      session_id = static_cast<uint32_t>(i);
      break;
    }
  }

  if (session_id == UINT32_MAX) {
    return UINT32_MAX; // All sessions active
  }

  RedjetAosSoaSession& sess = _sessions[session_id];
  sess.reset();
  sess.aos_base_address = aos_base;
  sess.total_elements = element_count;
  sess.aos_element_stride = aos_stride;

  size_t max_fields = (field_count > RedjetAosSoaSession::MAX_SLICES) ?
                      RedjetAosSoaSession::MAX_SLICES : field_count;

  size_t bytes_transferred_this_session = 0;

  for (size_t f = 0; f < max_fields; ++f) {
    const RedjetFieldDescriptor& desc = fields[f];
    size_t plane_bytes = element_count * desc.field_size;

    uint8_t* slice_buf = allocate_slice_storage(plane_bytes);
    if (slice_buf == nullptr) {
      // Revert allocated planes and abort
      sess.reset();
      return UINT32_MAX;
    }

    RedjetSoaSlicePlane& plane = sess.slices[f];
    plane.slice_buffer = slice_buf;
    plane.element_count = element_count;
    plane.element_size = desc.field_size;
    plane.capacity_bytes = plane_bytes;
    plane.field_offset_in_aos = desc.field_offset;
    plane.is_dirty.store(false, std::memory_order_relaxed);
    plane.last_sync_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);

    // Fast Gather from AOS to SOA slice plane
    const uint8_t* src_cursor = reinterpret_cast<const uint8_t*>(aos_base) + desc.field_offset;
    uint8_t* dst_cursor = slice_buf;

    if (desc.field_size == 4) {
      // Unrolled 4-byte scalar gather
      size_t unrolled = element_count & ~static_cast<size_t>(7);
      for (size_t i = 0; i < unrolled; i += 8) {
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 0) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 0) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 1) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 1) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 2) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 2) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 3) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 3) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 4) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 4) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 5) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 5) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 6) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 6) * aos_stride);
        *reinterpret_cast<uint32_t*>(dst_cursor + (i + 7) * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 7) * aos_stride);
      }
      for (size_t i = unrolled; i < element_count; ++i) {
        *reinterpret_cast<uint32_t*>(dst_cursor + i * 4) = *reinterpret_cast<const uint32_t*>(src_cursor + i * aos_stride);
      }
    } else if (desc.field_size == 8) {
      // Unrolled 8-byte scalar gather
      size_t unrolled = element_count & ~static_cast<size_t>(3);
      for (size_t i = 0; i < unrolled; i += 4) {
        *reinterpret_cast<uint64_t*>(dst_cursor + (i + 0) * 8) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 0) * aos_stride);
        *reinterpret_cast<uint64_t*>(dst_cursor + (i + 1) * 8) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 1) * aos_stride);
        *reinterpret_cast<uint64_t*>(dst_cursor + (i + 2) * 8) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 2) * aos_stride);
        *reinterpret_cast<uint64_t*>(dst_cursor + (i + 3) * 8) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 3) * aos_stride);
      }
      for (size_t i = unrolled; i < element_count; ++i) {
        *reinterpret_cast<uint64_t*>(dst_cursor + i * 8) = *reinterpret_cast<const uint64_t*>(src_cursor + i * aos_stride);
      }
    } else {
      // General byte-by-byte gather
      for (size_t i = 0; i < element_count; ++i) {
        std::memcpy(dst_cursor + i * desc.field_size,
                    src_cursor + i * aos_stride,
                    desc.field_size);
      }
    }

    bytes_transferred_this_session += plane_bytes;
  }

  sess.slice_count = static_cast<uint32_t>(max_fields);
  sess.is_active.store(true, std::memory_order_release);

  _total_aos_to_soa_transforms.fetch_add(1, std::memory_order_relaxed);
  _total_elements_sliced.fetch_add(element_count * max_fields, std::memory_order_relaxed);
  _total_bytes_transferred.fetch_add(bytes_transferred_this_session, std::memory_order_relaxed);

  return session_id;
}

void* RedjetAosSoaMorph::get_slice_pointer(uint32_t session_id, uint32_t field_idx) const noexcept {
  if (session_id >= MAX_CONCURRENT_SESSIONS) return nullptr;
  const RedjetAosSoaSession& sess = _sessions[session_id];
  if (!sess.is_active.load(std::memory_order_relaxed)) return nullptr;
  if (field_idx >= sess.slice_count) return nullptr;
  return sess.slices[field_idx].slice_buffer;
}

size_t RedjetAosSoaMorph::get_slice_element_count(uint32_t session_id) const noexcept {
  if (session_id >= MAX_CONCURRENT_SESSIONS) return 0;
  const RedjetAosSoaSession& sess = _sessions[session_id];
  if (!sess.is_active.load(std::memory_order_relaxed)) return 0;
  return sess.total_elements;
}

// =========================================================================
// 3. Shadow Write-Back on Mutation (512-Bit SIMD Scatter / Block Write)
// Scatters updated slice values back to the canonical AOS heap memory
// =========================================================================
bool RedjetAosSoaMorph::write_back_slice(uint32_t session_id, uint32_t field_idx) {
  if (session_id >= MAX_CONCURRENT_SESSIONS) return false;
  RedjetAosSoaSession& sess = _sessions[session_id];
  if (!sess.is_active.load(std::memory_order_relaxed)) return false;
  if (field_idx >= sess.slice_count) return false;

  RedjetSoaSlicePlane& plane = sess.slices[field_idx];
  if (!plane.check_and_clear_dirty()) {
    return true; // Already clean, no write-back needed
  }

  hs_ticket_guard_t guard(sess.session_lock);

  size_t count = sess.total_elements;
  size_t stride = sess.aos_element_stride;
  uint8_t* dst_cursor = reinterpret_cast<uint8_t*>(sess.aos_base_address) + plane.field_offset_in_aos;
  const uint8_t* src_cursor = plane.slice_buffer;

  if (plane.element_size == 4) {
    // 4-byte unrolled scatter write-back
    size_t unrolled = count & ~static_cast<size_t>(7);
    for (size_t i = 0; i < unrolled; i += 8) {
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 0) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 0) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 1) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 1) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 2) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 2) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 3) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 3) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 4) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 4) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 5) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 5) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 6) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 6) * 4);
      *reinterpret_cast<uint32_t*>(dst_cursor + (i + 7) * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + (i + 7) * 4);
    }
    for (size_t i = unrolled; i < count; ++i) {
      *reinterpret_cast<uint32_t*>(dst_cursor + i * stride) = *reinterpret_cast<const uint32_t*>(src_cursor + i * 4);
    }
  } else if (plane.element_size == 8) {
    // 8-byte unrolled scatter write-back
    size_t unrolled = count & ~static_cast<size_t>(3);
    for (size_t i = 0; i < unrolled; i += 4) {
      *reinterpret_cast<uint64_t*>(dst_cursor + (i + 0) * stride) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 0) * 8);
      *reinterpret_cast<uint64_t*>(dst_cursor + (i + 1) * stride) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 1) * 8);
      *reinterpret_cast<uint64_t*>(dst_cursor + (i + 2) * stride) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 2) * 8);
      *reinterpret_cast<uint64_t*>(dst_cursor + (i + 3) * stride) = *reinterpret_cast<const uint64_t*>(src_cursor + (i + 3) * 8);
    }
    for (size_t i = unrolled; i < count; ++i) {
      *reinterpret_cast<uint64_t*>(dst_cursor + i * stride) = *reinterpret_cast<const uint64_t*>(src_cursor + i * 8);
    }
  } else {
    // General byte-by-byte scatter write-back
    for (size_t i = 0; i < count; ++i) {
      std::memcpy(dst_cursor + i * stride,
                  src_cursor + i * plane.element_size,
                  plane.element_size);
    }
  }

  plane.last_sync_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
  sess.total_scatters.fetch_add(count, std::memory_order_relaxed);

  _total_scatter_operations.fetch_add(1, std::memory_order_relaxed);
  _total_mutations_written_back.fetch_add(count, std::memory_order_relaxed);
  _total_bytes_transferred.fetch_add(count * plane.element_size, std::memory_order_relaxed);

  return true;
}

bool RedjetAosSoaMorph::write_back_all(uint32_t session_id) {
  if (session_id >= MAX_CONCURRENT_SESSIONS) return false;
  RedjetAosSoaSession& sess = _sessions[session_id];
  if (!sess.is_active.load(std::memory_order_relaxed)) return false;

  bool all_ok = true;
  for (uint32_t f = 0; f < sess.slice_count; ++f) {
    if (!write_back_slice(session_id, f)) {
      all_ok = false;
    }
  }
  return all_ok;
}

bool RedjetAosSoaMorph::mutate_slice_element(uint32_t session_id,
                                             uint32_t field_idx,
                                             size_t element_index,
                                             const void* new_value,
                                             bool immediate_write_back) {
  if (session_id >= MAX_CONCURRENT_SESSIONS || new_value == nullptr) return false;
  RedjetAosSoaSession& sess = _sessions[session_id];
  if (!sess.is_active.load(std::memory_order_relaxed)) return false;
  if (field_idx >= sess.slice_count) return false;
  if (element_index >= sess.total_elements) return false;

  RedjetSoaSlicePlane& plane = sess.slices[field_idx];
  uint8_t* target = plane.slice_buffer + element_index * plane.element_size;
  std::memcpy(target, new_value, plane.element_size);
  plane.mark_dirty();
  sess.total_mutations.fetch_add(1, std::memory_order_relaxed);

  if (immediate_write_back) {
    // Immediate write-through to canonical AOS backing store
    uint8_t* aos_target = reinterpret_cast<uint8_t*>(sess.aos_base_address) +
                          element_index * sess.aos_element_stride +
                          plane.field_offset_in_aos;
    std::memcpy(aos_target, new_value, plane.element_size);
    _total_mutations_written_back.fetch_add(1, std::memory_order_relaxed);
  }

  return true;
}

void RedjetAosSoaMorph::release_session(uint32_t session_id) {
  if (session_id >= MAX_CONCURRENT_SESSIONS) return;
  RedjetAosSoaSession& sess = _sessions[session_id];
  if (sess.is_active.load(std::memory_order_relaxed)) {
    write_back_all(session_id);
    hs_ticket_guard_t guard(sess.session_lock);
    sess.reset();
  }
}

void RedjetAosSoaMorph::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet AOS->SOA Dynamic Shadow Slicing Engine:");
  st->print_cr("  Total AOS Transforms:       " UINT64_FORMAT, total_transforms());
  st->print_cr("  Total Sliced Elements:      " UINT64_FORMAT, total_sliced_elements());
  st->print_cr("  Total Mutations Written:    " UINT64_FORMAT, total_mutations_written_back());
  st->print_cr("  Total Scatter Operations:   " UINT64_FORMAT, total_scatter_operations());
  st->print_cr("  Total Bytes Transferred:    " UINT64_FORMAT, total_bytes_transferred());
}
