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

#include "gc/redjet/allocator/redjetVectorRelocator.hpp"
#include "gc/redjet/simd/redjetDispatchFacade.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <x86intrin.h>
#include <immintrin.h>
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetVectorRelocator* RedjetVectorRelocator::_instance = nullptr;
std::atomic<bool>      RedjetVectorRelocator::_initialized{ false };

RedjetVectorRelocator::RedjetVectorRelocator()
  : _batch_count(0),
    _total_objects_relocated(0),
    _total_bytes_relocated(0),
    _total_nt_bytes_written(0),
    _total_forwarding_races(0),
    _total_clasps_updated(0) {
  hs_ticket_lock_init(&_relocator_lock);
  std::memset(_batch_queue, 0, sizeof(_batch_queue));
}

RedjetVectorRelocator::~RedjetVectorRelocator() {
  // Release resources
}

void RedjetVectorRelocator::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetVectorRelocator();
  }
}

void RedjetVectorRelocator::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

void RedjetVectorRelocator::copy_payload_avx512(void* dest, const void* src, size_t bytes) noexcept {
#if defined(__AVX512F__)
  uint8_t* d = static_cast<uint8_t*>(dest);
  const uint8_t* s = static_cast<const uint8_t*>(src);

  while (bytes >= 64) {
    __m512i chunk = _mm512_loadu_si512(reinterpret_cast<const void*>(s));
    _mm512_storeu_si512(reinterpret_cast<void*>(d), chunk);
    d += 64;
    s += 64;
    bytes -= 64;
  }

  if (bytes >= 32) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), chunk);
    d += 32;
    s += 32;
    bytes -= 32;
  }

  if (bytes >= 16) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d), chunk);
    d += 16;
    s += 16;
    bytes -= 16;
  }

  if (bytes > 0) {
    std::memcpy(d, s, bytes);
  }
#else
  copy_payload_avx2(dest, src, bytes);
#endif
}

void RedjetVectorRelocator::copy_payload_avx2(void* dest, const void* src, size_t bytes) noexcept {
#if defined(__AVX2__)
  uint8_t* d = static_cast<uint8_t*>(dest);
  const uint8_t* s = static_cast<const uint8_t*>(src);

  while (bytes >= 32) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), chunk);
    d += 32;
    s += 32;
    bytes -= 32;
  }

  if (bytes >= 16) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d), chunk);
    d += 16;
    s += 16;
    bytes -= 16;
  }

  if (bytes > 0) {
    std::memcpy(d, s, bytes);
  }
#else
  std::memcpy(dest, src, bytes);
#endif
}

void RedjetVectorRelocator::copy_payload_streaming(void* dest, const void* src, size_t bytes) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  uint8_t* d = static_cast<uint8_t*>(dest);
  const uint8_t* s = static_cast<const uint8_t*>(src);

  // Align dest to 64-byte cache line boundary first
  uintptr_t dest_addr = reinterpret_cast<uintptr_t>(d);
  size_t leading = (64 - (dest_addr & 63)) & 63;
  if (leading > 0 && bytes >= leading) {
    std::memcpy(d, s, leading);
    d += leading;
    s += leading;
    bytes -= leading;
  }

#if defined(__AVX512F__)
  while (bytes >= 64) {
    __m512i chunk = _mm512_loadu_si512(reinterpret_cast<const void*>(s));
    _mm512_stream_si512(reinterpret_cast<void*>(d), chunk);
    d += 64;
    s += 64;
    bytes -= 64;
  }
#elif defined(__AVX2__)
  while (bytes >= 32) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
    _mm256_stream_si256(reinterpret_cast<__m256i*>(d), chunk);
    d += 32;
    s += 32;
    bytes -= 32;
  }
#else
  while (bytes >= 16) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
    _mm_stream_si128(reinterpret_cast<__m128i*>(d), chunk);
    d += 16;
    s += 16;
    bytes -= 16;
  }
#endif

  if (bytes > 0) {
    std::memcpy(d, s, bytes);
  }
  _mm_sfence();
#else
  std::memcpy(dest, src, bytes);
#endif
}

RedjetRelocateResult RedjetVectorRelocator::relocate_object(void* src_obj,
                                                           void* dest_buf,
                                                           size_t obj_size,
                                                           void** out_forwarded_addr) {
  if (src_obj == nullptr || dest_buf == nullptr || obj_size < sizeof(uintptr_t)) {
    if (out_forwarded_addr != nullptr) *out_forwarded_addr = nullptr;
    return RedjetRelocateResult::RELOCATE_FAILED_DEST_EXHAUSTED;
  }

  // 1. Fast-path: Check if already forwarded
  std::atomic<uintptr_t>* header_atomic = reinterpret_cast<std::atomic<uintptr_t>*>(src_obj);
  uintptr_t current_hdr = header_atomic->load(std::memory_order_relaxed);

  if ((current_hdr & FORWARD_BIT_MASK) != 0) {
    if (out_forwarded_addr != nullptr) {
      *out_forwarded_addr = reinterpret_cast<void*>(current_hdr & ~FORWARD_BIT_MASK);
    }
    return RedjetRelocateResult::RELOCATE_ALREADY_FORWARDED;
  }

  // 2. High-speed vector copy of payload to new destination buffer
  if (obj_size >= NON_TEMPORAL_THRESHOLD) {
    copy_payload_streaming(dest_buf, src_obj, obj_size);
    _total_nt_bytes_written.fetch_add(obj_size, std::memory_order_relaxed);
  } else {
    copy_payload_avx512(dest_buf, src_obj, obj_size);
  }

  // 3. Atomically install forwarding pointer
  uintptr_t target_forward_hdr = reinterpret_cast<uintptr_t>(dest_buf) | FORWARD_BIT_MASK;

  // CAS operation
  if (header_atomic->compare_exchange_strong(current_hdr, target_forward_hdr,
                                            std::memory_order_release,
                                            std::memory_order_acquire)) {
    // Successfully installed forwarding pointer!
    _total_objects_relocated.fetch_add(1, std::memory_order_relaxed);
    _total_bytes_relocated.fetch_add(obj_size, std::memory_order_relaxed);

    if (out_forwarded_addr != nullptr) {
      *out_forwarded_addr = dest_buf;
    }
    return RedjetRelocateResult::RELOCATE_SUCCESS_COPIED;
  }

  // 4. Lost race: another GC thread won the relocation CAS
  _total_forwarding_races.fetch_add(1, std::memory_order_relaxed);

  if ((current_hdr & FORWARD_BIT_MASK) != 0) {
    if (out_forwarded_addr != nullptr) {
      *out_forwarded_addr = reinterpret_cast<void*>(current_hdr & ~FORWARD_BIT_MASK);
    }
    return RedjetRelocateResult::RELOCATE_FAILED_CONCURRENT_LOST;
  }

  // If header changed but was not forwarded, retry once
  if (out_forwarded_addr != nullptr) {
    *out_forwarded_addr = dest_buf;
  }
  return RedjetRelocateResult::RELOCATE_FAILED_CONCURRENT_LOST;
}

bool RedjetVectorRelocator::enqueue_relocation(const RedjetRelocationTask& task) {
  hs_ticket_lock_acquire(&_relocator_lock);
  if (_batch_count >= MAX_BATCH_CAPACITY) {
    hs_ticket_lock_release(&_relocator_lock);
    return false;
  }
  _batch_queue[_batch_count++] = task;
  hs_ticket_lock_release(&_relocator_lock);
  return true;
}

RedjetRelocationReport RedjetVectorRelocator::flush_batch() {
  RedjetRelocationReport report;
  uint64_t start_tsc = sacculus_rdtsc();

  hs_ticket_lock_acquire(&_relocator_lock);

  size_t count = _batch_count;
  _batch_count = 0;

  for (size_t i = 0; i < count; ++i) {
    const RedjetRelocationTask& task = _batch_queue[i];

    // Prefetch destination cache line ahead by 2 items
    if (i + 2 < count && _batch_queue[i + 2].dest_obj != nullptr) {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_prefetch(static_cast<const char*>(_batch_queue[i + 2].dest_obj), _MM_HINT_T0);
#endif
    }

    void* forwarded_dest = nullptr;
    RedjetRelocateResult res = relocate_object(task.src_obj, task.dest_obj, task.size_bytes, &forwarded_dest);

    if (res == RedjetRelocateResult::RELOCATE_SUCCESS_COPIED) {
      report.objects_evacuated++;
      report.bytes_evacuated += task.size_bytes;
      if (task.size_bytes >= NON_TEMPORAL_THRESHOLD) {
        report.non_temporal_bytes += task.size_bytes;
      }
      if (task.has_clasp) {
        report.tailtraces_clasps_relinked++;
        _total_clasps_updated.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (res == RedjetRelocateResult::RELOCATE_FAILED_CONCURRENT_LOST ||
               res == RedjetRelocateResult::RELOCATE_ALREADY_FORWARDED) {
      report.forwarding_conflicts_resolved++;
    }
  }

  hs_ticket_lock_release(&_relocator_lock);

  sfence_barrier();

  report.duration_tsc = sacculus_rdtsc() - start_tsc;
  return report;
}

void RedjetVectorRelocator::sfence_barrier() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_sfence();
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

void RedjetVectorRelocator::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetVectorRelocator] Evacuation & Relocation Status:");
  st->print_cr("  - Objects Relocated:     %llu",
               static_cast<unsigned long long>(_total_objects_relocated.load(std::memory_order_relaxed)));
  st->print_cr("  - Bytes Relocated:       %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_bytes_relocated.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_bytes_relocated.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  - Streaming NT Bytes:    %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_nt_bytes_written.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_nt_bytes_written.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  - Forwarding Races Lost: %llu",
               static_cast<unsigned long long>(_total_forwarding_races.load(std::memory_order_relaxed)));
  st->print_cr("  - TailTraces Clasps:     %llu relinked",
               static_cast<unsigned long long>(_total_clasps_updated.load(std::memory_order_relaxed)));
}
