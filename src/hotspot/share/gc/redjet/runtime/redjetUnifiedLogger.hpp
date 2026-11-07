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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETUNIFIEDLOGGER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETUNIFIEDLOGGER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/gcCause.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// RedjetUnifiedLogger
//
// Shenandoah-grade HotSpot Unified Logging (`-Xlog:gc*`) + Lock-Free Java-Side
// Structured Log Ring for RedjetGC.
// Covers:
//  - Cycle banner & completion (`gc, start` / `gc`) with exact cause & MB/us
//  - Sub-microsecond phase telemetry (`gc, phases`)
//  - Ergonomics, Seasonal transitions & SIMD Contention (`gc, ergo`)
//  - VFS `hotspot_memory` vs `java_memory` ledger (`gc, heap`)
//  - Metaspace & `HeaderPane` `narrowKlass` summary (`gc, metaspace`)
//  - Full end-of-cycle / JVM-exit statistics table (`gc, stats`)
//  - 1024-slot lock-free ring exposed to Java `jdk.gc.redjet.telemetry.RedjetUnifiedLogStream`
// ============================================================================

enum class RedjetLogSubsystem : uint8_t {
  CYCLE      = 0,
  PHASE      = 1,
  SEASONAL   = 2,
  VFS_DOMAIN = 3,
  TAILTRACES = 4,
  PUZZLER    = 5,
  SIMD_GOV   = 6,
  MLTC_RMRP  = 7,
  OFF_HEAP   = 8,
  FAST_JNI   = 9
};

struct alignas(64) RedjetStructuredLogRecord {
  std::atomic<uint64_t> sequence_no{ 0 };
  uint64_t              timestamp_ns{ 0 };
  uint64_t              sweep_id{ 0 };
  uint64_t              duration_ns{ 0 };
  uint64_t              bytes_before{ 0 };
  uint64_t              bytes_after{ 0 };
  uint8_t               subsystem{ 0 };
  uint8_t               severity{ 0 }; // 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR
  char                  tag[30]{ 0 };
  char                  message[128]{ 0 };
};

class RedjetHeap;

class RedjetUnifiedLogger {
public:
  static constexpr size_t LOG_RING_CAPACITY = 1024;

private:
  RedjetStructuredLogRecord         _ring[LOG_RING_CAPACITY];
  alignas(64) std::atomic<uint64_t> _write_seq{ 0 };
  std::atomic<uint64_t>             _total_cycles_logged{ 0 };
  std::atomic<uint64_t>             _total_phase_ns_accum{ 0 };
  std::atomic<uint64_t>             _peak_cycle_ns{ 0 };

  void push_ring_record(RedjetLogSubsystem subsys,
                        uint8_t severity,
                        uint64_t sweep_id,
                        uint64_t duration_ns,
                        uint64_t bytes_before,
                        uint64_t bytes_after,
                        const char* tag,
                        const char* msg) noexcept;

public:
  RedjetUnifiedLogger() = default;
  ~RedjetUnifiedLogger() = default;

  static RedjetUnifiedLogger& instance() noexcept;

#ifndef REDJET_STANDALONE_BUILD
  void log_cycle_start(uint64_t sweep_id, GCCause::Cause cause, size_t used_before, size_t capacity) noexcept;
  void log_phase(uint64_t sweep_id, const char* phase_name, uint64_t duration_ns, size_t items_processed = 0) noexcept;
  void log_ergo_decision(uint64_t sweep_id, RedjetLogSubsystem subsys, const char* tag, const char* detail) noexcept;
  void log_vfs_domain_summary(uint64_t sweep_id, RedjetHeap* heap) noexcept;
  void log_cycle_end(uint64_t sweep_id,
                     GCCause::Cause cause,
                     size_t used_before,
                     size_t used_after,
                     size_t capacity,
                     uint64_t total_duration_ns) noexcept;
  void print_statistics_summary(outputStream* st, const RedjetHeap* heap) const;
#endif

  // Lock-free reader for Java `RedjetUnifiedLogStream` / JMX MXBean
  RJ_API size_t copy_recent_records(uint64_t since_seq,
                                    RedjetStructuredLogRecord* out_buf,
                                    size_t max_records,
                                    uint64_t* out_latest_seq) const noexcept;

  uint64_t latest_sequence() const noexcept { return _write_seq.load(std::memory_order_acquire); }
  uint64_t total_cycles_logged() const noexcept { return _total_cycles_logged.load(std::memory_order_relaxed); }
  uint64_t peak_cycle_ns() const noexcept { return _peak_cycle_ns.load(std::memory_order_relaxed); }
};

// RAII Phase Timer that emits HotSpot Unified Logging `log_info(gc, phases)` + structured ring entry
class RedjetGCPhaseLogScope {
private:
  uint64_t    _sweep_id;
  const char* _phase_name;
  uint64_t    _start_ns;
  size_t      _items{ 0 };
public:
  RedjetGCPhaseLogScope(uint64_t sweep_id, const char* phase_name) noexcept
    : _sweep_id(sweep_id), _phase_name(phase_name), _start_ns(sacculus_now_ns()) {}

  void set_items(size_t items) noexcept { _items = items; }

  ~RedjetGCPhaseLogScope() noexcept {
#ifndef REDJET_STANDALONE_BUILD
    uint64_t elapsed = sacculus_now_ns() - _start_ns;
    RedjetUnifiedLogger::instance().log_phase(_sweep_id, _phase_name, elapsed, _items);
#endif
  }
};

extern RedjetUnifiedLogger g_redjet_unified_logger;

#endif // SHARE_GC_REDJET_RUNTIME_REDJETUNIFIEDLOGGER_HPP
