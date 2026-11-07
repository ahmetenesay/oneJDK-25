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

#include "gc/redjet/simd/redjetVectorMath.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <cpuid.h>
#include <immintrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetSimdLevel RedjetVectorMath::_active_level = RedjetSimdLevel::SCALAR_FALLBACK;
bool RedjetVectorMath::_initialized = false;

std::atomic<uint64_t> RedjetVectorMath::_total_vector_ops{ 0 };
std::atomic<uint64_t> RedjetVectorMath::_total_bytes_processed{ 0 };
std::atomic<uint64_t> RedjetVectorMath::_total_fma_elements{ 0 };
std::atomic<uint64_t> RedjetVectorMath::_total_popcnt_vectors{ 0 };

void RedjetVectorMath::init() {
  if (_initialized) return;
  detect_cpu_simd_capabilities();
  _initialized = true;
}

void RedjetVectorMath::detect_cpu_simd_capabilities() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  int cpu_info[4] = { 0 };

#if defined(_MSC_VER)
  __cpuid(cpu_info, 1);
#else
  __cpuid(1, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif

  bool sse42 = (cpu_info[2] & (1 << 20)) != 0;
  bool avx   = (cpu_info[2] & (1 << 28)) != 0;

  bool avx2 = false;
  bool avx512f = false;

  if (avx) {
#if defined(_MSC_VER)
    __cpuidex(cpu_info, 7, 0);
#else
    __cpuid_count(7, 0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif
    avx2    = (cpu_info[1] & (1 << 5)) != 0;
    avx512f = (cpu_info[1] & (1 << 16)) != 0;
  }

  if (avx512f) {
    _active_level = RedjetSimdLevel::AVX512;
  } else if (avx2) {
    _active_level = RedjetSimdLevel::AVX2;
  } else if (sse42) {
    _active_level = RedjetSimdLevel::SSE4_2;
  } else {
    _active_level = RedjetSimdLevel::SCALAR_FALLBACK;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  _active_level = RedjetSimdLevel::ARM_NEON;
#else
  _active_level = RedjetSimdLevel::SCALAR_FALLBACK;
#endif
}

bool RedjetVectorMath::mem_equal_512(const void* src1, const void* src2, size_t bytes) noexcept {
  if (src1 == src2 || bytes == 0) return true;
  if (src1 == nullptr || src2 == nullptr) return false;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_processed.fetch_add(bytes, std::memory_order_relaxed);

  const uint8_t* p1 = reinterpret_cast<const uint8_t*>(src1);
  const uint8_t* p2 = reinterpret_cast<const uint8_t*>(src2);
  size_t offset = 0;

#if defined(__AVX512F__)
  while (offset + 64 <= bytes) {
    __m512i v1 = _mm512_loadu_si512(reinterpret_cast<const void*>(p1 + offset));
    __m512i v2 = _mm512_loadu_si512(reinterpret_cast<const void*>(p2 + offset));
    __mmask8 mask = _mm512_cmpeq_epi64_mask(v1, v2);
    if (mask != 0xFF) {
      return false; // Mismatch in this 64-byte block
    }
    offset += 64;
  }
#elif defined(__AVX2__)
  while (offset + 32 <= bytes) {
    __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p1 + offset));
    __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p2 + offset));
    __m256i cmp = _mm256_cmpeq_epi8(v1, v2);
    int mask = _mm256_movemask_epi8(cmp);
    if (mask != -1) {
      return false;
    }
    offset += 32;
  }
#endif

  // Residual scalar compare
  while (offset < bytes) {
    if (p1[offset] != p2[offset]) {
      return false;
    }
    offset++;
  }

  return true;
}

uint64_t RedjetVectorMath::vector_sum_u64(const uint64_t* data, size_t count) noexcept {
  if (data == nullptr || count == 0) return 0;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_processed.fetch_add(count * sizeof(uint64_t), std::memory_order_relaxed);

  size_t i = 0;
  uint64_t total = 0;

#if defined(__AVX512F__)
  __m512i vec_acc = _mm512_setzero_si512();
  while (i + 8 <= count) {
    __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(&data[i]));
    vec_acc = _mm512_add_epi64(vec_acc, v);
    i += 8;
  }
  alignas(64) uint64_t buf[8];
  _mm512_storeu_si512(reinterpret_cast<void*>(buf), vec_acc);
  for (int j = 0; j < 8; ++j) {
    total += buf[j];
  }
#elif defined(__AVX2__)
  __m256i vec_acc = _mm256_setzero_si256();
  while (i + 4 <= count) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&data[i]));
    vec_acc = _mm256_add_epi64(vec_acc, v);
    i += 4;
  }
  alignas(32) uint64_t buf[4];
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(buf), vec_acc);
  for (int j = 0; j < 4; ++j) {
    total += buf[j];
  }
#endif

  while (i < count) {
    total += data[i];
    i++;
  }

  return total;
}

uint64_t RedjetVectorMath::vector_min_u64(const uint64_t* data, size_t count) noexcept {
  if (data == nullptr || count == 0) return 0;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_processed.fetch_add(count * sizeof(uint64_t), std::memory_order_relaxed);

  uint64_t min_val = data[0];
  for (size_t i = 1; i < count; ++i) {
    if (data[i] < min_val) {
      min_val = data[i];
    }
  }
  return min_val;
}

uint64_t RedjetVectorMath::vector_max_u64(const uint64_t* data, size_t count) noexcept {
  if (data == nullptr || count == 0) return 0;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_processed.fetch_add(count * sizeof(uint64_t), std::memory_order_relaxed);

  uint64_t max_val = data[0];
  for (size_t i = 1; i < count; ++i) {
    if (data[i] > max_val) {
      max_val = data[i];
    }
  }
  return max_val;
}

uint64_t RedjetVectorMath::vector_popcount_u64(const uint64_t* data, size_t count) noexcept {
  if (data == nullptr || count == 0) return 0;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_popcnt_vectors.fetch_add(count, std::memory_order_relaxed);

  uint64_t total_set_bits = 0;
  for (size_t i = 0; i < count; ++i) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    total_set_bits += __popcnt64(data[i]);
#elif defined(__GNUC__) || defined(__clang__)
    total_set_bits += __builtin_popcountll(data[i]);
#else
    uint64_t x = data[i];
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    total_set_bits += (x * 0x0101010101010101ULL) >> 56;
#endif
  }
  return total_set_bits;
}

uint64_t RedjetVectorMath::vector_hash_512(const void* data, size_t bytes, uint64_t seed) noexcept {
  if (data == nullptr || bytes == 0) return seed;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_processed.fetch_add(bytes, std::memory_order_relaxed);

  const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
  uint64_t h = seed ^ 0x9E3779B97F4A7C15ULL;
  size_t offset = 0;

  while (offset + 8 <= bytes) {
    uint64_t word;
    std::memcpy(&word, p + offset, 8);
    h ^= word;
    h *= 0xC6A4A7935BD1E995ULL;
    h ^= h >> 47;
    offset += 8;
  }

  while (offset < bytes) {
    h ^= static_cast<uint64_t>(p[offset]);
    h *= 0x100000001B3ULL;
    offset++;
  }

  return h;
}

float RedjetVectorMath::vector_dot_product_f32(const float* a, const float* b, size_t count) noexcept {
  if (a == nullptr || b == nullptr || count == 0) return 0.0f;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_fma_elements.fetch_add(count, std::memory_order_relaxed);

  size_t i = 0;
  float sum = 0.0f;

#if defined(__AVX512F__)
  __m512 vec_sum = _mm512_setzero_ps();
  while (i + 16 <= count) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    vec_sum = _mm512_fmadd_ps(va, vb, vec_sum);
    i += 16;
  }
  alignas(64) float buf[16];
  _mm512_storeu_ps(buf, vec_sum);
  for (int j = 0; j < 16; ++j) {
    sum += buf[j];
  }
#elif defined(__AVX2__)
  __m256 vec_sum = _mm256_setzero_ps();
  while (i + 8 <= count) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    vec_sum = _mm256_fmadd_ps(va, vb, vec_sum);
    i += 8;
  }
  alignas(32) float buf[8];
  _mm256_storeu_ps(buf, vec_sum);
  for (int j = 0; j < 8; ++j) {
    sum += buf[j];
  }
#endif

  while (i < count) {
    sum += a[i] * b[i];
    i++;
  }

  return sum;
}

void RedjetVectorMath::vector_axpy_f32(float alpha, const float* x, float* y, size_t count) noexcept {
  if (x == nullptr || y == nullptr || count == 0) return;

  _total_vector_ops.fetch_add(1, std::memory_order_relaxed);
  _total_fma_elements.fetch_add(count, std::memory_order_relaxed);

  size_t i = 0;

#if defined(__AVX512F__)
  __m512 valpha = _mm512_set1_ps(alpha);
  while (i + 16 <= count) {
    __m512 vx = _mm512_loadu_ps(&x[i]);
    __m512 vy = _mm512_loadu_ps(&y[i]);
    vy = _mm512_fmadd_ps(valpha, vx, vy);
    _mm512_storeu_ps(&y[i], vy);
    i += 16;
  }
#elif defined(__AVX2__)
  __m256 valpha = _mm256_set1_ps(alpha);
  while (i + 8 <= count) {
    __m256 vx = _mm256_loadu_ps(&x[i]);
    __m256 vy = _mm256_loadu_ps(&y[i]);
    vy = _mm256_fmadd_ps(valpha, vx, vy);
    _mm256_storeu_ps(&y[i], vy);
    i += 8;
  }
#endif

  while (i < count) {
    y[i] += alpha * x[i];
    i++;
  }
}

void RedjetVectorMath::print_on(outputStream* st) {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Multi-Architecture SIMD & Vector Math ---");
  st->print_cr("  Active SIMD Level:    %s",
               (_active_level == RedjetSimdLevel::AVX512) ? "AVX-512 (512-bit ZMM registers)" :
               (_active_level == RedjetSimdLevel::AVX2) ? "AVX2 (256-bit YMM registers)" :
               (_active_level == RedjetSimdLevel::SSE4_2) ? "SSE 4.2 (128-bit XMM registers)" :
               (_active_level == RedjetSimdLevel::ARM_NEON) ? "ARM NEON (128-bit registers)" :
               (_active_level == RedjetSimdLevel::ARM_SVE) ? "ARM SVE Scalable Vector Extensions" : "Scalar Fallback");
  st->print_cr("  Total Vector Ops:     %llu", static_cast<unsigned long long>(_total_vector_ops.load(std::memory_order_relaxed)));
  st->print_cr("  Total Bytes Processed:%llu bytes", static_cast<unsigned long long>(_total_bytes_processed.load(std::memory_order_relaxed)));
  st->print_cr("  Total FMA Elements:   %llu", static_cast<unsigned long long>(_total_fma_elements.load(std::memory_order_relaxed)));
  st->print_cr("  Popcount Vectors:     %llu", static_cast<unsigned long long>(_total_popcnt_vectors.load(std::memory_order_relaxed)));
}
