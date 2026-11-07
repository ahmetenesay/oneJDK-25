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

#include "gc/redjet/runtime/redjetSnatchRebate.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetLog.hpp"
#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// Static variable definitions
alignas(64) RedjetEmptyBoxSlot      RedjetSnatchRebateEngine::_emptybox_pool[NUM_SIZE_CLASSES][SLOTS_PER_CLASS];
alignas(64) std::atomic<size_t>     RedjetSnatchRebateEngine::_pool_counts[NUM_SIZE_CLASSES];
alignas(64) std::atomic<RedjetRebateTarget>         RedjetSnatchRebateEngine::_target{ RedjetRebateTarget::TARGET_AUTO };
alignas(64) std::atomic<RedjetSnatchAndRebateMode>  RedjetSnatchRebateEngine::_mode{ RedjetSnatchAndRebateMode::MODE_GYC };
alignas(64) std::atomic<size_t>     RedjetSnatchRebateEngine::_gyc_buffered_bytes{ 0 };
alignas(64) std::atomic<bool>       RedjetSnatchRebateEngine::_initialized{ false };
RedjetSnatchRebateMetrics           RedjetSnatchRebateEngine::_metrics;

const char* RedjetSnatchRebateEngine::target_name(RedjetRebateTarget target) noexcept {
  switch (target) {
    case RedjetRebateTarget::TARGET_HEAP: return "Heap (Internal Redistribution)";
    case RedjetRebateTarget::TARGET_OS:   return "OS (Return-To-OS Decommit)";
    case RedjetRebateTarget::TARGET_AUTO: return "Auto (Autonomous Coach Arbitration)";
    default:                             return "Unknown Target";
  }
}

const char* RedjetSnatchRebateEngine::mode_name(RedjetSnatchAndRebateMode mode) noexcept {
  switch (mode) {
    case RedjetSnatchAndRebateMode::MODE_GYC:   return "GYC (Generous Yet Cautious)";
    case RedjetSnatchAndRebateMode::MODE_RAPID: return "Rapid (Aggressive Immediate)";
    case RedjetSnatchAndRebateMode::MODE_LAZY:  return "Lazy (Deferred Batched)";
    case RedjetSnatchAndRebateMode::MODE_AUTO:  return "Auto (Dynamically Calibrated)";
    default:                                    return "Unknown Mode";
  }
}

size_t RedjetSnatchRebateEngine::calculate_size_class(size_t size) noexcept {
  if (size <= 64) return 0;
  if (size >= (2 * 1024 * 1024)) return NUM_SIZE_CLASSES - 1;

  // Power-of-two bucketing
  size_t leading_zeros = 0;
#if defined(_MSC_VER)
  unsigned long index;
  if (_BitScanReverse64(&index, size)) {
    leading_zeros = 63 - index;
  } else {
    leading_zeros = 64;
  }
#else
  leading_zeros = __builtin_clzll(size);
#endif

  size_t sc = (leading_zeros >= 58) ? 0 : (58 - leading_zeros);
  return (sc < NUM_SIZE_CLASSES) ? sc : (NUM_SIZE_CLASSES - 1);
}

bool RedjetSnatchRebateEngine::decommit_to_os(uintptr_t addr, size_t size) {
  if (addr == 0 || size == 0) return false;

  _metrics.os_decommit_syscalls.fetch_add(1, std::memory_order_relaxed);

#if defined(_WIN32)
  BOOL res = VirtualFree(reinterpret_cast<LPVOID>(addr), size, MEM_DECOMMIT);
  if (res) {
    _metrics.os_decommitted_bytes.fetch_add(size, std::memory_order_relaxed);
    return true;
  }
#else
  int res = madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
  if (res == 0) {
    _metrics.os_decommitted_bytes.fetch_add(size, std::memory_order_relaxed);
    return true;
  }
#endif
  return false;
}

void RedjetSnatchRebateEngine::initialize(RedjetRebateTarget target, RedjetSnatchAndRebateMode mode) {
  if (_initialized.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  _target.store(target, std::memory_order_release);
  _mode.store(mode, std::memory_order_release);
  _gyc_buffered_bytes.store(0, std::memory_order_release);

  for (size_t sc = 0; sc < NUM_SIZE_CLASSES; ++sc) {
    _pool_counts[sc].store(0, std::memory_order_relaxed);
    for (size_t slot = 0; slot < SLOTS_PER_CLASS; ++slot) {
      _emptybox_pool[sc][slot].address = 0;
      _emptybox_pool[sc][slot].size_bytes = 0;
      _emptybox_pool[sc][slot].size_class = static_cast<uint32_t>(sc);
      _emptybox_pool[sc][slot].state.store(0, std::memory_order_relaxed);
      _emptybox_pool[sc][slot].snatch_tsc = 0;
    }
  }

  redjet_hotlog("[SNATCH-REBATE] Subsystem initialized. Target: %s, Mode: %s",
                target_name(target), mode_name(mode));
}

void RedjetSnatchRebateEngine::shutdown() {
  if (!_initialized.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  flush_buffers(true);
  redjet_hotlog("[SNATCH-REBATE] Subsystem shutdown completed.");
}

void* RedjetSnatchRebateEngine::snatch_emptybox(size_t size_bytes) {
  _metrics.total_snatch_attempts.fetch_add(1, std::memory_order_relaxed);

  size_t sc = calculate_size_class(size_bytes);
  size_t count = _pool_counts[sc].load(std::memory_order_relaxed);
  if (count == 0) {
    _metrics.total_snatch_misses.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  // Scan slots in size class for an available emptybox
  for (size_t i = 0; i < SLOTS_PER_CLASS; ++i) {
    uint32_t state = _emptybox_pool[sc][i].state.load(std::memory_order_acquire);
    if (state == SLOT_STATE_AVAILABLE) {
      if (_emptybox_pool[sc][i].state.compare_exchange_strong(state, SLOT_STATE_CLAIMED,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_relaxed)) {
        void* ptr = reinterpret_cast<void*>(_emptybox_pool[sc][i].address);
        _emptybox_pool[sc][i].address = 0;
        _emptybox_pool[sc][i].size_bytes = 0;
        _emptybox_pool[sc][i].state.store(SLOT_STATE_FREE, std::memory_order_release); // Return slot to Free
        _pool_counts[sc].fetch_sub(1, std::memory_order_relaxed);

        _metrics.total_snatch_hits.fetch_add(1, std::memory_order_relaxed);
        _metrics.total_snatched_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
        return ptr;
      }
    }
  }

  _metrics.total_snatch_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

bool RedjetSnatchRebateEngine::deposit_emptybox(void* addr, size_t size_bytes) {
  if (addr == nullptr || size_bytes == 0) return false;

  size_t sc = calculate_size_class(size_bytes);

  for (size_t i = 0; i < SLOTS_PER_CLASS; ++i) {
    uint32_t state = _emptybox_pool[sc][i].state.load(std::memory_order_relaxed);
    if (state == SLOT_STATE_FREE) {
      // Reserve slot for deposit first to prevent concurrent snatcher from reading unwritten address
      if (_emptybox_pool[sc][i].state.compare_exchange_strong(state, SLOT_STATE_RESERVED_DEPOSIT,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_relaxed)) {
        _emptybox_pool[sc][i].address = reinterpret_cast<uintptr_t>(addr);
        _emptybox_pool[sc][i].size_bytes = size_bytes;
        _emptybox_pool[sc][i].snatch_tsc = sacculus_rdtsc();
        // Publish payload atomically with release barrier
        _emptybox_pool[sc][i].state.store(SLOT_STATE_AVAILABLE, std::memory_order_release);
        _pool_counts[sc].fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }

  return false; // Size class pool full
}

void RedjetSnatchRebateEngine::rebate_block(void* addr, size_t size_bytes) {
  if (addr == nullptr || size_bytes == 0) return;

  _metrics.total_rebate_cycles.fetch_add(1, std::memory_order_relaxed);

  RedjetRebateTarget tgt = _target.load(std::memory_order_relaxed);
  RedjetSnatchAndRebateMode md = _mode.load(std::memory_order_relaxed);

  // Auto target arbitration
  if (tgt == RedjetRebateTarget::TARGET_AUTO) {
    // If block is small (<= 64KB), prefer internal Heap redistribution to save syscalls
    if (size_bytes <= 64 * 1024) {
      tgt = RedjetRebateTarget::TARGET_HEAP;
    } else {
      tgt = RedjetRebateTarget::TARGET_OS;
    }
  }

  if (tgt == RedjetRebateTarget::TARGET_HEAP) {
    // Internal Redistribution: Try depositing to EmptyBox pool first
    if (!deposit_emptybox(addr, size_bytes)) {
      // Pool full: record as internal heap buffer
      _gyc_buffered_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    }
    _metrics.heap_redistributed_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    return;
  }

  // Target OS: evaluate pacing mode
  if (md == RedjetSnatchAndRebateMode::MODE_RAPID) {
    _metrics.rapid_rebate_events.fetch_add(1, std::memory_order_relaxed);
    decommit_to_os(reinterpret_cast<uintptr_t>(addr), size_bytes);
    return;
  }

  if (md == RedjetSnatchAndRebateMode::MODE_GYC) {
    size_t buffered = _gyc_buffered_bytes.fetch_add(size_bytes, std::memory_order_relaxed) + size_bytes;
    _metrics.gyc_buffer_retained_bytes.store(buffered, std::memory_order_relaxed);
    if (buffered > GYC_BUFFER_CAPACITY) {
      // Buffer exceeded: decommit surplus to OS
      decommit_to_os(reinterpret_cast<uintptr_t>(addr), size_bytes);
      _gyc_buffered_bytes.fetch_sub(size_bytes, std::memory_order_relaxed);
    } else {
      if (!deposit_emptybox(addr, size_bytes)) {
        // Pool full for this size class: decommit to OS to avoid leaking pointer
        decommit_to_os(reinterpret_cast<uintptr_t>(addr), size_bytes);
        _gyc_buffered_bytes.fetch_sub(size_bytes, std::memory_order_relaxed);
      }
    }
    return;
  }

  if (md == RedjetSnatchAndRebateMode::MODE_LAZY) {
    if (!deposit_emptybox(addr, size_bytes)) {
      // Pool full: flush to OS
      decommit_to_os(reinterpret_cast<uintptr_t>(addr), size_bytes);
    }
    return;
  }

  // Default fallback: direct decommit
  decommit_to_os(reinterpret_cast<uintptr_t>(addr), size_bytes);
}

size_t RedjetSnatchRebateEngine::flush_buffers(bool force_os_decommit) {
  size_t total_flushed = 0;

  for (size_t sc = 0; sc < NUM_SIZE_CLASSES; ++sc) {
    for (size_t i = 0; i < SLOTS_PER_CLASS; ++i) {
      uint32_t state = _emptybox_pool[sc][i].state.load(std::memory_order_relaxed);
      if (state == SLOT_STATE_AVAILABLE) {
        if (_emptybox_pool[sc][i].state.compare_exchange_strong(state, SLOT_STATE_CLAIMED,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_relaxed)) {
          uintptr_t addr = _emptybox_pool[sc][i].address;
          size_t sz = _emptybox_pool[sc][i].size_bytes;
          if (addr != 0 && sz != 0) {
            decommit_to_os(addr, sz);
            total_flushed += sz;
          }
          _emptybox_pool[sc][i].address = 0;
          _emptybox_pool[sc][i].size_bytes = 0;
          _emptybox_pool[sc][i].state.store(SLOT_STATE_FREE, std::memory_order_release);
        }
      }
    }
    _pool_counts[sc].store(0, std::memory_order_relaxed);
  }

  _gyc_buffered_bytes.store(0, std::memory_order_relaxed);
  _metrics.lazy_batch_flushes.fetch_add(1, std::memory_order_relaxed);
  return total_flushed;
}

void RedjetSnatchRebateEngine::print_on(outputStream* st) {
  if (st == nullptr) return;

  RedjetRebateTarget tgt = _target.load(std::memory_order_relaxed);
  RedjetSnatchAndRebateMode md = _mode.load(std::memory_order_relaxed);

  st->print_cr("Redjet Snatch & Rebate Engine Status:");
  st->print_cr("  Target Distribution:         %s", target_name(tgt));
  st->print_cr("  Pacing Mode:                 %s", mode_name(md));
  st->print_cr("  GYC Buffered Bytes:          %zu", _gyc_buffered_bytes.load(std::memory_order_relaxed));
  st->print_cr("  Total Snatch Attempts:       %llu", (unsigned long long)_metrics.total_snatch_attempts.load(std::memory_order_relaxed));
  st->print_cr("  Total Snatch Hits:           %llu", (unsigned long long)_metrics.total_snatch_hits.load(std::memory_order_relaxed));
  st->print_cr("  Total Snatch Misses:         %llu", (unsigned long long)_metrics.total_snatch_misses.load(std::memory_order_relaxed));
  st->print_cr("  Total Snatched Bytes:        %llu", (unsigned long long)_metrics.total_snatched_bytes.load(std::memory_order_relaxed));
  st->print_cr("  Total Rebate Cycles:         %llu", (unsigned long long)_metrics.total_rebate_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Heap Redistributed Bytes:    %llu", (unsigned long long)_metrics.heap_redistributed_bytes.load(std::memory_order_relaxed));
  st->print_cr("  OS Decommitted Bytes:        %llu", (unsigned long long)_metrics.os_decommitted_bytes.load(std::memory_order_relaxed));
  st->print_cr("  OS Decommit Syscalls:        %llu", (unsigned long long)_metrics.os_decommit_syscalls.load(std::memory_order_relaxed));
  st->print_cr("  Rapid Rebate Events:         %llu", (unsigned long long)_metrics.rapid_rebate_events.load(std::memory_order_relaxed));
  st->print_cr("  Lazy Batch Flushes:          %llu", (unsigned long long)_metrics.lazy_batch_flushes.load(std::memory_order_relaxed));
}
