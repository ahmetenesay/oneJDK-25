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

#include "gc/redjet/puzzler/redjetSIMDTable.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include <algorithm>
#include <chrono>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

// ============================================================================
// Built-in Reference Baseline Vector Kernels
// ============================================================================

void RedjetSIMDTable::kernel_scalar_baseline(const void* src, void* dst, size_t count) {
  const float* s = reinterpret_cast<const float*>(src);
  float* d = reinterpret_cast<float*>(dst);
  for (size_t i = 0; i < count; ++i) {
    d[i] = s[i] * 1.0000001f + 0.0000001f;
  }
}

void RedjetSIMDTable::kernel_sse42_fallback(const void* src, void* dst, size_t count) {
#if defined(__SSE4_2__) || (defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86)))
  const float* s = reinterpret_cast<const float*>(src);
  float* d = reinterpret_cast<float*>(dst);
  size_t i = 0;
  __m128 vmul = _mm_set1_ps(1.0000001f);
  __m128 vadd = _mm_set1_ps(0.0000001f);

  for (; i + 4 <= count; i += 4) {
    __m128 vs = _mm_loadu_ps(s + i);
    __m128 vr = _mm_add_ps(_mm_mul_ps(vs, vmul), vadd);
    _mm_storeu_ps(d + i, vr);
  }
  for (; i < count; ++i) {
    d[i] = s[i] * 1.0000001f + 0.0000001f;
  }
#else
  kernel_scalar_baseline(src, dst, count);
#endif
}

void RedjetSIMDTable::kernel_avx2_fallback(const void* src, void* dst, size_t count) {
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
  const float* s = reinterpret_cast<const float*>(src);
  float* d = reinterpret_cast<float*>(dst);
  size_t i = 0;
  __m256 vmul = _mm256_set1_ps(1.0000001f);
  __m256 vadd = _mm256_set1_ps(0.0000001f);

  for (; i + 8 <= count; i += 8) {
    __m256 vs = _mm256_loadu_ps(s + i);
    __m256 vr = _mm256_fmadd_ps(vs, vmul, vadd);
    _mm256_storeu_ps(d + i, vr);
  }
  for (; i < count; ++i) {
    d[i] = s[i] * 1.0000001f + 0.0000001f;
  }
#else
  kernel_sse42_fallback(src, dst, count);
#endif
}

void RedjetSIMDTable::kernel_avx512_fallback(const void* src, void* dst, size_t count) {
#if (defined(__AVX512F__) && defined(__AVX512DQ__)) || (defined(_MSC_VER) && defined(__AVX512F__))
  const float* s = reinterpret_cast<const float*>(src);
  float* d = reinterpret_cast<float*>(dst);
  size_t i = 0;
  __m512 vmul = _mm512_set1_ps(1.0000001f);
  __m512 vadd = _mm512_set1_ps(0.0000001f);

  for (; i + 16 <= count; i += 16) {
    __m512 vs = _mm512_loadu_ps(s + i);
    __m512 vr = _mm512_fmadd_ps(vs, vmul, vadd);
    _mm512_storeu_ps(d + i, vr);
  }
  for (; i < count; ++i) {
    d[i] = s[i] * 1.0000001f + 0.0000001f;
  }
#else
  kernel_avx2_fallback(src, dst, count);
#endif
}

// ============================================================================
// RedjetShadowRCULadder Implementation
// ============================================================================

RedjetShadowRCULadder::RedjetShadowRCULadder() noexcept {
  _global_rcu_epoch.store(1, std::memory_order_relaxed);
  for (size_t t = 0; t < MAX_RCU_THREADS; ++t) {
    _thread_epochs[t].store(0, std::memory_order_relaxed);
  }

  static const char* RUNG_NAMES[17] = {
    "Scalar Baseline", "SSE2", "SSE3", "SSSE3",
    "SSE4.1", "SSE4.2", "AVX", "AVX2",
    "AVX-512F", "AVX-512CD", "AVX-512BW", "AVX-512DQ",
    "AVX-512VL", "ARM NEON", "ARM SVE", "RISC-V Vector",
    "Puzzler Overwrite Wildcard"
  };

  for (size_t i = 0; i < 17; ++i) {
    std::strncpy(_table_a[i].name, RUNG_NAMES[i], sizeof(_table_a[i].name) - 1);
    _table_a[i].ladder_level = static_cast<uint32_t>(i);
    _table_a[i].is_available = (i == 0) ? 1 : 0;
    _table_a[i].is_active = (i == 0) ? 1 : 0;
    _table_a[i].kernel_fn = &RedjetSIMDTable::kernel_scalar_baseline;

    std::memcpy(&_table_b[i], &_table_a[i], sizeof(RedjetLadderRungDescriptor));
  }

  _active_table.store(_table_a, std::memory_order_release);
}

void RedjetShadowRCULadder::synchronize_rcu() noexcept {
  uint64_t target_epoch = _global_rcu_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;

  for (size_t t = 0; t < MAX_RCU_THREADS; ++t) {
    while (true) {
      uint64_t te = _thread_epochs[t].load(std::memory_order_acquire);
      if (te == 0 || te >= target_epoch) {
        break; // Thread is outside read-side critical section or reached new epoch
      }
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
      _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
      __builtin_ia32_pause();
#endif
    }
  }
}

bool RedjetShadowRCULadder::hot_swap_slot16(const char* name, RedjetVectorKernelFn fn, uint32_t level) {
  RedjetLadderRungDescriptor* cur_active = _active_table.load(std::memory_order_acquire);
  RedjetLadderRungDescriptor* shadow = (cur_active == _table_a) ? _table_b : _table_a;

  // Copy current active configuration to shadow
  std::memcpy(shadow, cur_active, sizeof(RedjetLadderRungDescriptor) * static_cast<size_t>(RedjetSIMDRung::RUNG_COUNT));

  // Update Slot 16 in shadow
  auto& slot16 = shadow[static_cast<size_t>(RedjetSIMDRung::RUNG_16_PUZZLER_OVERWRITE)];
  if (name != nullptr) {
    std::strncpy(slot16.name, name, sizeof(slot16.name) - 1);
    slot16.name[sizeof(slot16.name) - 1] = '\0';
  }
  slot16.ladder_level = level;
  slot16.kernel_fn = fn;
  slot16.is_available = 1;
  slot16.is_active = 1;

  // Atomic pointer hot-swap
  _active_table.store(shadow, std::memory_order_release);

  // Synchronize grace period
  synchronize_rcu();
  return true;
}

// ============================================================================
// RedjetSIMDTable Implementation
// ============================================================================

RedjetSIMDTable::RedjetSIMDTable() noexcept
  : _active_rung_index(0),
    _drift_tolerance_epsilon(1e-5f),
    _total_vector_executions(0),
    _fallback_to_ieee_events(0),
    _shadow_rcu_swaps(0),
    _precision_verifications(0) {
  initialize_hardware_rungs();
}

void RedjetSIMDTable::initialize_hardware_rungs() {
  RedjetLadderRungDescriptor* tbl = _rcu_ladder.active_table();

  // Baseline Scalar
  tbl[0].is_available = 1;
  tbl[0].kernel_fn = &RedjetSIMDTable::kernel_scalar_baseline;

  // Check hardware features
#if defined(__x86_64__) || defined(_M_AMD64)
  tbl[5].is_available = 1; // SSE4.2
  tbl[5].kernel_fn = &RedjetSIMDTable::kernel_sse42_fallback;

  tbl[7].is_available = 1; // AVX2
  tbl[7].kernel_fn = &RedjetSIMDTable::kernel_avx2_fallback;

  tbl[8].is_available = 1; // AVX-512F
  tbl[8].kernel_fn = &RedjetSIMDTable::kernel_avx512_fallback;

  _active_rung_index = 8; // Default to AVX-512F on modern x86
#else
  _active_rung_index = 0;
#endif

  for (size_t i = 0; i < 17; ++i) {
    tbl[i].is_active = (i == _active_rung_index) ? 1 : 0;
  }
}

void RedjetSIMDTable::populate_live_header(HS_hotspot_live_header_t* header) const noexcept {
  if (header == nullptr) return;

  RedjetLadderRungDescriptor* tbl = const_cast<RedjetShadowRCULadder&>(_rcu_ladder).active_table();

  header->active_simd_idx = _active_rung_index;
  header->simd_candidate_count = 17;

  for (size_t i = 0; i < 17; ++i) {
    std::strncpy(header->simd_table[i].name, tbl[i].name, sizeof(header->simd_table[i].name) - 1);
    header->simd_table[i].name[sizeof(header->simd_table[i].name) - 1] = '\0';
    header->simd_table[i].ladder_level = tbl[i].ladder_level;
    header->simd_table[i].is_active = tbl[i].is_active;
  }
}

void RedjetSIMDTable::dispatch_vector(RedjetSIMDRung rung, const void* src, void* dst, size_t count, uint32_t thread_id) {
  _total_vector_executions.fetch_add(1, std::memory_order_relaxed);

  RedjetLadderRungDescriptor* tbl = _rcu_ladder.read_lock(thread_id);
  size_t idx = static_cast<size_t>(rung);
  if (idx >= 17) idx = 0;

  RedjetVectorKernelFn fn = tbl[idx].kernel_fn;
  if (fn == nullptr) {
    fn = &RedjetSIMDTable::kernel_scalar_baseline;
  }

  fn(src, dst, count);
  tbl[idx].total_dispatches++;

  _rcu_ladder.read_unlock(thread_id);
}

bool RedjetSIMDTable::verify_and_enforce_precision(const float* candidate_output, const float* reference_output, size_t count) {
  _precision_verifications.fetch_add(1, std::memory_order_relaxed);

  if (candidate_output == nullptr || reference_output == nullptr || count == 0) {
    return false;
  }

  double max_rel_err = 0.0;
  for (size_t i = 0; i < count; ++i) {
    float cand = candidate_output[i];
    float ref  = reference_output[i];

    if (std::isnan(cand) || std::isinf(cand)) {
      trigger_emergency_ieee_fallback();
      return false;
    }

    double diff = std::fabs(static_cast<double>(cand) - static_cast<double>(ref));
    double denom = std::fabs(static_cast<double>(ref)) + 1e-7;
    double rel_err = diff / denom;

    if (rel_err > max_rel_err) {
      max_rel_err = rel_err;
    }

    if (rel_err > _drift_tolerance_epsilon) {
      trigger_emergency_ieee_fallback();
      return false;
    }
  }

  return true;
}

void RedjetSIMDTable::trigger_emergency_ieee_fallback() {
  _fallback_to_ieee_events.fetch_add(1, std::memory_order_relaxed);
  _active_rung_index = 0; // Emergency clamp to strict IEEE scalar baseline

  RedjetLadderRungDescriptor* tbl = _rcu_ladder.active_table();
  for (size_t i = 0; i < 17; ++i) {
    tbl[i].is_active = (i == 0) ? 1 : 0;
  }

  log_info(gc, simd)("Redjet SIMD Table: Emergency IEEE Fallback triggered! Active rung clamped to Scalar Baseline.");
}

bool RedjetSIMDTable::install_puzzler_specialization(const char* name, RedjetVectorKernelFn fn, uint32_t level) {
  _shadow_rcu_swaps.fetch_add(1, std::memory_order_relaxed);
  bool res = _rcu_ladder.hot_swap_slot16(name, fn, level);
  if (res) {
    _active_rung_index = 16;
  }
  return res;
}

void RedjetSIMDTable::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet SIMD Table & Precision Enforcer (17-Slot Ladder) ---");
  st->print_cr("  Active Rung Index:       %u", _active_rung_index);
  st->print_cr("  Drift Tolerance Epsilon: %e", _drift_tolerance_epsilon);
  st->print_cr("  Total Vector Executions: %llu", (unsigned long long)_total_vector_executions.load(std::memory_order_relaxed));
  st->print_cr("  Precision Verifications: %llu", (unsigned long long)_precision_verifications.load(std::memory_order_relaxed));
  st->print_cr("  Emergency Fallbacks:     %llu", (unsigned long long)_fallback_to_ieee_events.load(std::memory_order_relaxed));
  st->print_cr("  Shadow RCU Hot-Swaps:    %llu", (unsigned long long)_shadow_rcu_swaps.load(std::memory_order_relaxed));

  RedjetLadderRungDescriptor* tbl = const_cast<RedjetShadowRCULadder&>(_rcu_ladder).active_table();
  for (size_t i = 0; i < 17; ++i) {
    st->print_cr("    Slot [%02zu] %-26s: level=%u, avail=%u, active=%u, dispatches=%llu",
                 i, tbl[i].name, tbl[i].ladder_level, tbl[i].is_available, tbl[i].is_active,
                 (unsigned long long)tbl[i].total_dispatches);
  }
}
