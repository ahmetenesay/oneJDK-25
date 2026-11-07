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

#include "gc/redjet/runtime/redjetBarrierMatrix.hpp"
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

RedjetBarrierMatrix* RedjetBarrierMatrix::_instance = nullptr;
std::atomic<bool> RedjetBarrierMatrix::_initialized{ false };

RedjetBarrierMatrix::RedjetBarrierMatrix()
  : _total_barriers_evaluated(0),
    _total_egg_elisions(0),
    _total_read_elisions(0) {
  hs_ticket_lock_init(&_matrix_lock);

  for (size_t c = 0; c < 4; ++c) {
    for (size_t b = 0; b < 4; ++b) {
      _invocations[c][b].store(0, std::memory_order_relaxed);
      _elisions[c][b].store(0, std::memory_order_relaxed);
    }
  }
}

RedjetBarrierMatrix::~RedjetBarrierMatrix() {
}

void RedjetBarrierMatrix::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetBarrierMatrix();
  }
}

void RedjetBarrierMatrix::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

bool RedjetBarrierMatrix::should_elide_post_write_barrier(uint8_t origin_vfs_tier, bool is_newly_allocated) noexcept {
  _total_barriers_evaluated.fetch_add(1, std::memory_order_relaxed);

  // PLAN.md Section 3.2: If destination object is in Runway Egg (origin tier 0) and newly allocated,
  // post-write barrier is mathematically redundant because Egg has no surviving inbound pointers from older generations.
  if (origin_vfs_tier == 0 && is_newly_allocated) {
    _total_egg_elisions.fetch_add(1, std::memory_order_relaxed);
    return true; // Elide barrier!
  }

  return false;
}

bool RedjetBarrierMatrix::should_elide_load_reference_barrier(bool is_locally_confined, bool is_immutable) noexcept {
  _total_barriers_evaluated.fetch_add(1, std::memory_order_relaxed);

  // PLAN.md Section 3.2: Locally confined objects (stack allocated / scalar replaced)
  // or immutable object references (Strings / Records) never undergo relocation while reading.
  if (is_locally_confined || is_immutable) {
    _total_read_elisions.fetch_add(1, std::memory_order_relaxed);
    return true; // Elide barrier!
  }

  return false;
}

void RedjetBarrierMatrix::record_barrier(RedjetCompilerTier compiler, RedjetBarrierKind barrier, bool was_elided) noexcept {
  size_t c = static_cast<size_t>(compiler);
  size_t b = static_cast<size_t>(barrier);

  if (c < 4 && b < 4) {
    _invocations[c][b].fetch_add(1, std::memory_order_relaxed);
    if (was_elided) {
      _elisions[c][b].fetch_add(1, std::memory_order_relaxed);
    }
  }
}

RedjetBarrierMetrics RedjetBarrierMatrix::get_metrics(RedjetCompilerTier compiler, RedjetBarrierKind barrier) const noexcept {
  size_t c = static_cast<size_t>(compiler);
  size_t b = static_cast<size_t>(barrier);

  RedjetBarrierMetrics m;
  if (c < 4 && b < 4) {
    m.total_invocations = _invocations[c][b].load(std::memory_order_relaxed);
    m.total_elisions = _elisions[c][b].load(std::memory_order_relaxed);
    m.elision_rate = (m.total_invocations > 0) ?
                     (static_cast<double>(m.total_elisions) / static_cast<double>(m.total_invocations)) : 0.0;
  }
  return m;
}

void RedjetBarrierMatrix::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Dynamic Memory Barrier Matrix ---");
  st->print_cr("  Barriers Evaluated:   %llu", static_cast<unsigned long long>(_total_barriers_evaluated.load(std::memory_order_relaxed)));
  st->print_cr("  Egg Young Elisions:   %llu", static_cast<unsigned long long>(_total_egg_elisions.load(std::memory_order_relaxed)));
  st->print_cr("  Read Barrier Elisions:%llu", static_cast<unsigned long long>(_total_read_elisions.load(std::memory_order_relaxed)));

  const char* comp_names[4] = { "C1", "C2", "MIRC", "VAC" };
  const char* bar_names[4]  = { "PreWrite", "PostWrite", "LoadRef", "NativePin" };

  for (size_t c = 0; c < 4; ++c) {
    for (size_t b = 0; b < 4; ++b) {
      uint64_t inv = _invocations[c][b].load(std::memory_order_relaxed);
      uint64_t eli = _elisions[c][b].load(std::memory_order_relaxed);
      if (inv > 0) {
        st->print_cr("  [%s][%s]: %llu invocations, %llu elided (%.1f%%)",
                     comp_names[c], bar_names[b],
                     static_cast<unsigned long long>(inv),
                     static_cast<unsigned long long>(eli),
                     (100.0 * eli / inv));
      }
    }
  }
}
