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

#include "gc/redjet/allocator/redjetMemScrubber.hpp"
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

RedjetMemScrubber* RedjetMemScrubber::_instance = nullptr;
std::atomic<bool> RedjetMemScrubber::_initialized{ false };

RedjetMemScrubber::RedjetMemScrubber()
  : _target_bandwidth_mbps(500),
    _total_bytes_scrubbed(0),
    _total_blocks_sanitized(0),
    _total_hugepages_sanitized(0),
    _total_streaming_stores(0),
    _redundant_zero_writes_skipped(0),
    _scrubber_throttles_engaged(0) {
  hs_ticket_lock_init(&_scrubber_lock);
}

RedjetMemScrubber::~RedjetMemScrubber() {
}

void RedjetMemScrubber::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetMemScrubber();
  }
}

void RedjetMemScrubber::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

bool RedjetMemScrubber::is_range_all_zero(const void* src, size_t bytes) noexcept {
  if (src == nullptr || bytes == 0) return true;

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(src);
  size_t offset = 0;

#if defined(__AVX512F__)
  while (offset + 64 <= bytes) {
    __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(ptr + offset));
    __mmask64 m = _mm512_test_epi8_mask(v, v);
    if (m != 0) return false;
    offset += 64;
  }
#elif defined(__AVX2__)
  __m256i zero256 = _mm256_setzero_si256();
  while (offset + 32 <= bytes) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr + offset));
    if (!_mm256_testz_si256(v, v)) return false;
    offset += 32;
  }
#endif

  while (offset + 8 <= bytes) {
    if (*reinterpret_cast<const uint64_t*>(ptr + offset) != 0) return false;
    offset += 8;
  }

  while (offset < bytes) {
    if (ptr[offset] != 0) return false;
    offset++;
  }

  return true;
}

void RedjetMemScrubber::zero_range_streaming(void* dest, size_t bytes) noexcept {
  if (dest == nullptr || bytes == 0) return;

  uint8_t* ptr = reinterpret_cast<uint8_t*>(dest);
  size_t offset = 0;

  // Align to 64-byte cache line boundary
  while ((reinterpret_cast<uintptr_t>(ptr + offset) & 63) != 0 && offset < bytes) {
    ptr[offset++] = 0;
  }

#if defined(__AVX512F__)
  __m512i zero512 = _mm512_setzero_si512();

  // 4x unrolled streaming store loop: 256 bytes per iteration
  while (offset + UNROLL_STRIDE_BYTES <= bytes) {
    _mm512_stream_si512(reinterpret_cast<void*>(ptr + offset), zero512);
    _mm512_stream_si512(reinterpret_cast<void*>(ptr + offset + 64), zero512);
    _mm512_stream_si512(reinterpret_cast<void*>(ptr + offset + 128), zero512);
    _mm512_stream_si512(reinterpret_cast<void*>(ptr + offset + 192), zero512);

    offset += UNROLL_STRIDE_BYTES;
    _total_streaming_stores.fetch_add(4, std::memory_order_relaxed);
  }

  // Single 64-byte chunks
  while (offset + 64 <= bytes) {
    _mm512_stream_si512(reinterpret_cast<void*>(ptr + offset), zero512);
    offset += 64;
    _total_streaming_stores.fetch_add(1, std::memory_order_relaxed);
  }

#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
  _mm_sfence();
#elif defined(__x86_64__) || defined(__i386__)
  _mm_sfence();
#endif
#elif defined(__AVX2__)
  __m256i zero256 = _mm256_setzero_si256();
  while (offset + 32 <= bytes) {
    _mm256_stream_si256(reinterpret_cast<__m256i*>(ptr + offset), zero256);
    offset += 32;
    _total_streaming_stores.fetch_add(1, std::memory_order_relaxed);
  }
  _mm_sfence();
#elif defined(__x86_64__) || defined(_M_X64)
  while (offset + 8 <= bytes) {
    _mm_stream_si64(reinterpret_cast<long long*>(ptr + offset), 0);
    offset += 8;
    _total_streaming_stores.fetch_add(1, std::memory_order_relaxed);
  }
  _mm_sfence();
#endif

  // Tail bytes
  while (offset < bytes) {
    ptr[offset++] = 0;
  }

  _total_bytes_scrubbed.fetch_add(bytes, std::memory_order_relaxed);
}

void RedjetMemScrubber::sanitize_64k_block(void* block_addr) noexcept {
  if (block_addr == nullptr) return;

  // Check if block is already zeroed
  if (is_range_all_zero(block_addr, BLOCK_64K)) {
    _redundant_zero_writes_skipped.fetch_add(BLOCK_64K, std::memory_order_relaxed);
    _total_blocks_sanitized.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  zero_range_streaming(block_addr, BLOCK_64K);
  _total_blocks_sanitized.fetch_add(1, std::memory_order_relaxed);
}

void RedjetMemScrubber::sanitize_2m_page(void* page_addr) noexcept {
  if (page_addr == nullptr) return;

  if (is_range_all_zero(page_addr, PAGE_2M)) {
    _redundant_zero_writes_skipped.fetch_add(PAGE_2M, std::memory_order_relaxed);
    _total_hugepages_sanitized.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  zero_range_streaming(page_addr, PAGE_2M);
  _total_hugepages_sanitized.fetch_add(1, std::memory_order_relaxed);
}

void RedjetMemScrubber::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Autonomous DRAM Memory Scrubber ---");
  st->print_cr("  Target Bandwidth Throttle: %llu MB/s", (unsigned long long)_target_bandwidth_mbps.load(std::memory_order_relaxed));
  st->print_cr("  Total Bytes Scrubbed:      %llu bytes (%llu MB)",
               (unsigned long long)_total_bytes_scrubbed.load(std::memory_order_relaxed),
               (unsigned long long)(_total_bytes_scrubbed.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  64KB Blocks Sanitized:     %llu", (unsigned long long)_total_blocks_sanitized.load(std::memory_order_relaxed));
  st->print_cr("  2MB HugePages Sanitized:   %llu", (unsigned long long)_total_hugepages_sanitized.load(std::memory_order_relaxed));
  st->print_cr("  Streaming Stores Issued:   %llu", (unsigned long long)_total_streaming_stores.load(std::memory_order_relaxed));
  st->print_cr("  Redundant Writes Skipped:  %llu bytes (%llu MB)",
               (unsigned long long)_redundant_zero_writes_skipped.load(std::memory_order_relaxed),
               (unsigned long long)(_redundant_zero_writes_skipped.load(std::memory_order_relaxed) / (1024 * 1024)));
}
