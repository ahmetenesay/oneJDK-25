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

#include "gc/redjet/runtime/redjetUnifiedLogger.hpp"
#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/api/redjetExplicitGC.hpp"
#include "gc/redjet/api/redjetTailTraces.hpp"
#include "gc/redjet/runtime/redjetBPUDirector.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include "gc/redjet/simd/redjetSIMDContentionGovernor.hpp"
#include <cstdio>
#include <cstring>

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/engine/redjetSeasonalCoordinator.hpp"
#include "logging/log.hpp"
#include "memory/metaspaceUtils.hpp"
#endif

RedjetUnifiedLogger g_redjet_unified_logger;

RedjetUnifiedLogger& RedjetUnifiedLogger::instance() noexcept {
  return g_redjet_unified_logger;
}

void RedjetUnifiedLogger::push_ring_record(RedjetLogSubsystem subsys,
                                           uint8_t severity,
                                           uint64_t sweep_id,
                                           uint64_t duration_ns,
                                           uint64_t bytes_before,
                                           uint64_t bytes_after,
                                           const char* tag,
                                           const char* msg) noexcept {
  uint64_t seq = _write_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  RedjetStructuredLogRecord& slot = _ring[(seq - 1) % LOG_RING_CAPACITY];
  slot.timestamp_ns = sacculus_now_ns();
  slot.sweep_id     = sweep_id;
  slot.duration_ns  = duration_ns;
  slot.bytes_before = bytes_before;
  slot.bytes_after  = bytes_after;
  slot.subsystem    = static_cast<uint8_t>(subsys);
  slot.severity     = severity;
  if (tag != nullptr) {
    std::strncpy(slot.tag, tag, sizeof(slot.tag) - 1);
    slot.tag[sizeof(slot.tag) - 1] = '\0';
  } else {
    slot.tag[0] = '\0';
  }
  if (msg != nullptr) {
    std::strncpy(slot.message, msg, sizeof(slot.message) - 1);
    slot.message[sizeof(slot.message) - 1] = '\0';
  } else {
    slot.message[0] = '\0';
  }
  slot.sequence_no.store(seq, std::memory_order_release);
}

#ifndef REDJET_STANDALONE_BUILD

void RedjetUnifiedLogger::log_cycle_start(uint64_t sweep_id,
                                          GCCause::Cause cause,
                                          size_t used_before,
                                          size_t capacity) noexcept {
  const char* cause_str = GCCause::to_string(cause);
  const char* season = RedjetSeasonalCoordinator::season_name(RedjetSeasonalCoordinator::current_season());
  RedjetGCVecTier vec_tier = RedjetSIMDContentionGovernor::instance().snapshot().current_global_tier;

  log_info(gc, start)("GC(%llu) RedjetGC Cycle (%s) [Season=%s, VecTier=%s, Used=%zuM/%zuM]",
                      static_cast<unsigned long long>(sweep_id),
                      cause_str,
                      season,
                      RedjetSIMDContentionGovernor::tier_name(vec_tier),
                      used_before / M,
                      capacity / M);

  char buf[128];
  std::snprintf(buf, sizeof(buf), "Start (%s) Season=%s Tier=%s",
                cause_str, season, RedjetSIMDContentionGovernor::tier_name(vec_tier));
  push_ring_record(RedjetLogSubsystem::CYCLE, 2, sweep_id, 0, used_before, used_before, "gc_start", buf);
}

void RedjetUnifiedLogger::log_phase(uint64_t sweep_id,
                                    const char* phase_name,
                                    uint64_t duration_ns,
                                    size_t items_processed) noexcept {
  double us = static_cast<double>(duration_ns) / 1000.0;
  if (items_processed > 0) {
    log_info(gc, phases)("GC(%llu)   %-38s %.3f us (%zu items)",
                         static_cast<unsigned long long>(sweep_id),
                         phase_name ? phase_name : "Phase",
                         us,
                         items_processed);
  } else {
    log_info(gc, phases)("GC(%llu)   %-38s %.3f us",
                         static_cast<unsigned long long>(sweep_id),
                         phase_name ? phase_name : "Phase",
                         us);
  }

  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s: %.3f us (items=%zu)",
                phase_name ? phase_name : "Phase", us, items_processed);
  push_ring_record(RedjetLogSubsystem::PHASE, 2, sweep_id, duration_ns, items_processed, 0, "gc_phase", buf);
}

void RedjetUnifiedLogger::log_ergo_decision(uint64_t sweep_id,
                                            RedjetLogSubsystem subsys,
                                            const char* tag,
                                            const char* detail) noexcept {
  log_info(gc, ergo)("GC(%llu) [Ergo:%s] %s",
                     static_cast<unsigned long long>(sweep_id),
                     tag ? tag : "redjet",
                     detail ? detail : "");
  push_ring_record(subsys, 2, sweep_id, 0, 0, 0, tag, detail);
}

void RedjetUnifiedLogger::log_vfs_domain_summary(uint64_t sweep_id, RedjetHeap* heap) noexcept {
  if (heap == nullptr) return;
  RedjetVFSTree* vfs = heap->vfs_tree();
  size_t hs_on  = vfs->domain_used_bytes(RedjetMemoryDomain::HOTSPOT_MEMORY, true);
  size_t hs_off = vfs->domain_used_bytes(RedjetMemoryDomain::HOTSPOT_MEMORY, false);
  size_t jv_on  = vfs->domain_used_bytes(RedjetMemoryDomain::JAVA_MEMORY, true);
  size_t jv_off = vfs->domain_used_bytes(RedjetMemoryDomain::JAVA_MEMORY, false);

  log_info(gc, heap)("GC(%llu) VFS Domains: java_memory(on=%zuK, off=%zuK) | hotspot_memory(on=%zuK, off=%zuK, header_pane=%zuK, code_boot=%zuK)",
                     static_cast<unsigned long long>(sweep_id),
                     jv_on / K, jv_off / K,
                     hs_on / K, hs_off / K,
                     RedjetCompressedSpaces::instance().klass_used_bytes() / K,
                     RedjetCompressedSpaces::instance().code_boot_used_bytes() / K);

  char buf[128];
  std::snprintf(buf, sizeof(buf), "java(on=%zuK,off=%zuK) hotspot(on=%zuK,off=%zuK)",
                jv_on / K, jv_off / K, hs_on / K, hs_off / K);
  push_ring_record(RedjetLogSubsystem::VFS_DOMAIN, 2, sweep_id, 0, jv_on + jv_off, hs_on + hs_off, "vfs_domains", buf);
}

void RedjetUnifiedLogger::log_cycle_end(uint64_t sweep_id,
                                        GCCause::Cause cause,
                                        size_t used_before,
                                        size_t used_after,
                                        size_t capacity,
                                        uint64_t total_duration_ns) noexcept {
  _total_cycles_logged.fetch_add(1, std::memory_order_relaxed);
  _total_phase_ns_accum.fetch_add(total_duration_ns, std::memory_order_relaxed);
  uint64_t prev_peak = _peak_cycle_ns.load(std::memory_order_relaxed);
  while (total_duration_ns > prev_peak &&
         !_peak_cycle_ns.compare_exchange_weak(prev_peak, total_duration_ns, std::memory_order_relaxed)) {}

  double ms = static_cast<double>(total_duration_ns) / 1000000.0;
  const char* cause_str = GCCause::to_string(cause);

  log_info(gc)("GC(%llu) RedjetGC Cycle (%s) %zuM->%zuM(%zuM) %.3fms [TailTraces=%llu, BPU=%.2f%%]",
               static_cast<unsigned long long>(sweep_id),
               cause_str,
               used_before / M,
               used_after / M,
               capacity / M,
               ms,
               static_cast<unsigned long long>(g_redjet_tail_traces.total_signals()),
               RedjetBPUDirector::instance().accuracy_percent());

  MetaspaceCombinedStats meta_stats = MetaspaceUtils::get_combined_statistics();
  log_info(gc, metaspace)("GC(%llu) Metaspace: %zuK->%zuK(%zuK) | HeaderPane: %zuK/%zuK",
                          static_cast<unsigned long long>(sweep_id),
                          meta_stats.used() / K,
                          meta_stats.used() / K,
                          meta_stats.reserved() / K,
                          RedjetCompressedSpaces::instance().klass_used_bytes() / K,
                          RedjetCompressedSpaces::instance().klass_reserved_bytes() / K);

  char buf[128];
  std::snprintf(buf, sizeof(buf), "Done (%s) %zuM->%zuM in %.3fms",
                cause_str, used_before / M, used_after / M, ms);
  push_ring_record(RedjetLogSubsystem::CYCLE, 2, sweep_id, total_duration_ns, used_before, used_after, "gc_end", buf);
}

void RedjetUnifiedLogger::print_statistics_summary(outputStream* st, const RedjetHeap* heap) const {
  if (st == nullptr) return;
  uint64_t cycles = total_cycles_logged();
  double avg_us = (cycles > 0)
      ? (static_cast<double>(_total_phase_ns_accum.load(std::memory_order_relaxed)) / (1000.0 * static_cast<double>(cycles)))
      : 0.0;
  double peak_us = static_cast<double>(peak_cycle_ns()) / 1000.0;

  st->print_cr("================================================================================");
  st->print_cr("                   RedjetGC Unified Telemetry & Statistics                      ");
  st->print_cr("================================================================================");
  st->print_cr("  Total GC Cycles Logged:    %llu (Avg: %.2f us, Peak: %.2f us)",
               static_cast<unsigned long long>(cycles), avg_us, peak_us);
  st->print_cr("  TailTraces Signals:        %llu (Pending: %llu, FalsePos Filtered: %llu)",
               static_cast<unsigned long long>(g_redjet_tail_traces.total_signals()),
               static_cast<unsigned long long>(g_redjet_tail_traces.pending_signals()),
               static_cast<unsigned long long>(g_redjet_tail_traces.false_positives_cancelled()));
  st->print_cr("  Explicit Clean Hints:      Preflight=%llu, Invoked=%llu, Reclaimed=%llu KB",
               static_cast<unsigned long long>(RedjetExplicitGC::hints_preflighted()),
               static_cast<unsigned long long>(RedjetExplicitGC::hints_invoked()),
               static_cast<unsigned long long>(RedjetExplicitGC::bytes_reclaimed() / 1024));
  RedjetBPUDirector::instance().print_on(st);
  RedjetSIMDContentionGovernor::instance().print_on(st);
  RedjetCompressedSpaces::instance().print_on(st);
  if (heap != nullptr) {
    const_cast<RedjetHeap*>(heap)->vfs_tree()->print_domain_summary_on(st);
  }
  st->print_cr("================================================================================");
}

#endif // !REDJET_STANDALONE_BUILD

size_t RedjetUnifiedLogger::copy_recent_records(uint64_t since_seq,
                                                RedjetStructuredLogRecord* out_buf,
                                                size_t max_records,
                                                uint64_t* out_latest_seq) const noexcept {
  uint64_t latest = _write_seq.load(std::memory_order_acquire);
  if (out_latest_seq != nullptr) {
    *out_latest_seq = latest;
  }
  if (out_buf == nullptr || max_records == 0 || latest == 0 || since_seq >= latest) {
    return 0;
  }
  uint64_t oldest_available = (latest > LOG_RING_CAPACITY) ? (latest - LOG_RING_CAPACITY + 1) : 1;
  uint64_t start_seq = (since_seq + 1 > oldest_available) ? (since_seq + 1) : oldest_available;
  size_t copied = 0;
  for (uint64_t s = start_seq; s <= latest && copied < max_records; ++s) {
    const RedjetStructuredLogRecord& src = _ring[(s - 1) % LOG_RING_CAPACITY];
    if (src.sequence_no.load(std::memory_order_acquire) == s) {
      out_buf[copied].sequence_no.store(s, std::memory_order_relaxed);
      out_buf[copied].timestamp_ns = src.timestamp_ns;
      out_buf[copied].sweep_id     = src.sweep_id;
      out_buf[copied].duration_ns  = src.duration_ns;
      out_buf[copied].bytes_before = src.bytes_before;
      out_buf[copied].bytes_after  = src.bytes_after;
      out_buf[copied].subsystem    = src.subsystem;
      out_buf[copied].severity     = src.severity;
      std::memcpy(out_buf[copied].tag, src.tag, sizeof(src.tag));
      std::memcpy(out_buf[copied].message, src.message, sizeof(src.message));
      copied++;
    }
  }
  return copied;
}
