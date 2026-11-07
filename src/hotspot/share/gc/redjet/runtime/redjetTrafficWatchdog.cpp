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

#include "gc/redjet/runtime/redjetTrafficWatchdog.hpp"
#include "gc/redjet/allocator/redjetPageDefrag.hpp"
#include "gc/redjet/allocator/redjetStreamThrottle.hpp"
#include "gc/redjet/simd/redjetDispatchFacade.hpp"

RedjetTrafficWatchdog* RedjetTrafficWatchdog::_instance = nullptr;
std::atomic<bool>      RedjetTrafficWatchdog::_initialized{ false };

RedjetTrafficWatchdog::RedjetTrafficWatchdog() {
  hs_ticket_lock_init(&_watchdog_lock);
  for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
    _threads[i].thread_id = 0;
    _threads[i].last_sample_tsc = sacculus_rdtsc();
    _threads[i].accumulated_bytes = 0;
    _threads[i].current_rate_mbps = 0.0;
    _threads[i].is_flow_watched = false;
  }
}

RedjetTrafficWatchdog::~RedjetTrafficWatchdog() {
  // Release resources
}

void RedjetTrafficWatchdog::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetTrafficWatchdog();
  }
}

void RedjetTrafficWatchdog::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

void RedjetTrafficWatchdog::set_thread_flow_watch(uint32_t thread_id, bool enable) {
  size_t slot = thread_id % MAX_TRACKED_THREADS;
  hs_ticket_lock_acquire(&_watchdog_lock);
  _threads[slot].thread_id = thread_id;
  _threads[slot].is_flow_watched = enable;
  _threads[slot].last_sample_tsc = sacculus_rdtsc();
  _threads[slot].accumulated_bytes = 0;
  _threads[slot].current_rate_mbps = 0.0;
  hs_ticket_lock_release(&_watchdog_lock);
}

bool RedjetTrafficWatchdog::record_allocation(uint32_t thread_id, size_t bytes) {
  _total_monitored_bytes.fetch_add(bytes, std::memory_order_relaxed);

  size_t slot = thread_id % MAX_TRACKED_THREADS;
  uint64_t now_tsc = sacculus_rdtsc();
  bool surge = false;

  hs_ticket_lock_acquire(&_watchdog_lock);

  ThreadFlowRecord& rec = _threads[slot];
  if (rec.thread_id != thread_id) {
    rec.thread_id = thread_id;
    rec.last_sample_tsc = now_tsc;
    rec.accumulated_bytes = bytes;
    rec.current_rate_mbps = 0.0;
    rec.is_flow_watched = false;
    hs_ticket_lock_release(&_watchdog_lock);
    return false;
  }

  rec.accumulated_bytes += bytes;
  uint64_t elapsed_tsc = now_tsc - rec.last_sample_tsc;

  // Evaluate rate every ~100 microseconds (assuming ~3 GHz TSC => 300,000 cycles)
  constexpr uint64_t SAMPLE_WINDOW_CYCLES = 300000ULL;

  if (elapsed_tsc >= SAMPLE_WINDOW_CYCLES) {
    double freq_hz = 3.0e9; // Fallback 3.0 GHz
#if defined(REDJET_STANDALONE_BUILD)
    freq_hz = 3.0e9;
#else
    uint64_t detected_freq = RedjetDispatchFacade::rdtsc_freq_hz();
    if (detected_freq > 0) {
      freq_hz = static_cast<double>(detected_freq);
    }
#endif

    double elapsed_sec = static_cast<double>(elapsed_tsc) / freq_hz;
    if (elapsed_sec > 0.000001) {
      double rate_bytes_sec = static_cast<double>(rec.accumulated_bytes) / elapsed_sec;
      rec.current_rate_mbps = rate_bytes_sec / (1024.0 * 1024.0);

      if (rec.is_flow_watched || rec.current_rate_mbps >= SURGE_THRESHOLD_MBPS) {
        if (rec.current_rate_mbps >= SURGE_THRESHOLD_MBPS) {
          surge = true;
          _total_surges_detected.fetch_add(1, std::memory_order_relaxed);

          // Trigger rapid 256KB page rebate if page defrag is available
          RedjetPageDefrag* defrag = RedjetPageDefrag::get();
          if (defrag != nullptr) {
            defrag->rebate_bulk_pages(4); // 4 * 64KB = 256KB rapid rebate
            _total_rebates_triggered.fetch_add(1, std::memory_order_relaxed);
          }

          // Trigger stream throttle to pace mutator memory bus saturation
          RedjetStreamThrottle* throttle = RedjetStreamThrottle::get();
          if (throttle != nullptr) {
            throttle->set_global_congestion_throttle(true);
          }
        }
      }
    }

    rec.last_sample_tsc = now_tsc;
    rec.accumulated_bytes = 0;
  }

  hs_ticket_lock_release(&_watchdog_lock);
  return surge;
}

double RedjetTrafficWatchdog::get_thread_rate_mbps(uint32_t thread_id) const noexcept {
  size_t slot = thread_id % MAX_TRACKED_THREADS;
  return _threads[slot].current_rate_mbps;
}

void RedjetTrafficWatchdog::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetTrafficWatchdog] Allocation Surge Status:");
  st->print_cr("  - Monitored Bytes:       %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_monitored_bytes.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_monitored_bytes.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  - Surges Detected:       %llu",
               static_cast<unsigned long long>(_total_surges_detected.load(std::memory_order_relaxed)));
  st->print_cr("  - Page Rebates Triggered: %llu (256KB each)",
               static_cast<unsigned long long>(_total_rebates_triggered.load(std::memory_order_relaxed)));
  st->print_cr("  - Active Watched Threads:");
  for (size_t i = 0; i < MAX_TRACKED_THREADS; ++i) {
    if (_threads[i].is_flow_watched || _threads[i].current_rate_mbps > 50.0) {
      st->print_cr("      Thread %u: Rate = %.2f MB/s [Watched: %s]",
                   _threads[i].thread_id,
                   _threads[i].current_rate_mbps,
                   _threads[i].is_flow_watched ? "YES" : "NO");
    }
  }
}
