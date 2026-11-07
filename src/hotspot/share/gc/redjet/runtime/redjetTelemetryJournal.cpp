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

#include "gc/redjet/runtime/redjetTelemetryJournal.hpp"
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

RedjetTelemetryJournal* RedjetTelemetryJournal::_instance = nullptr;
std::atomic<bool> RedjetTelemetryJournal::_initialized{ false };

RedjetTelemetryJournal::RedjetTelemetryJournal() {
  std::memset(_records, 0, sizeof(_records));
}

RedjetTelemetryJournal::~RedjetTelemetryJournal() {
}

void RedjetTelemetryJournal::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetTelemetryJournal();
  }
}

void RedjetTelemetryJournal::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

static const char* event_type_to_string(RedjetEventType type) noexcept {
  switch (type) {
    case RedjetEventType::EVENT_GC_CYCLE_START:     return "GC_CYCLE_START";
    case RedjetEventType::EVENT_GC_CYCLE_END:       return "GC_CYCLE_END";
    case RedjetEventType::EVENT_RUNWAY_ALLOCATION:  return "RUNWAY_ALLOCATION";
    case RedjetEventType::EVENT_HOTFIELD_SLAB_FILL: return "HOTFIELD_SLAB_FILL";
    case RedjetEventType::EVENT_FALLINE_QUARANTINE: return "FALLINE_QUARANTINE";
    case RedjetEventType::EVENT_LANDFILL_REBATE:    return "LANDFILL_REBATE";
    case RedjetEventType::EVENT_PUZZLER_DUEL_START: return "PUZZLER_DUEL_START";
    case RedjetEventType::EVENT_PUZZLER_HOTSWAP:    return "PUZZLER_HOTSWAP";
    case RedjetEventType::EVENT_DEFRAG_COMPACTION:  return "DEFRAG_COMPACTION";
    case RedjetEventType::EVENT_THROTTLE_ENGAGED:   return "THROTTLE_ENGAGED";
    case RedjetEventType::EVENT_THROTTLE_RELEASED:  return "THROTTLE_RELEASED";
    case RedjetEventType::EVENT_WX_TRANSITION:      return "WX_TRANSITION";
    case RedjetEventType::EVENT_SLOT16_PATCH:       return "SLOT16_PATCH";
    case RedjetEventType::EVENT_NATIVE_PIN_ACQUIRE: return "NATIVE_PIN_ACQUIRE";
    case RedjetEventType::EVENT_NATIVE_PIN_RELEASE: return "NATIVE_PIN_RELEASE";
    default:                                        return "UNKNOWN_EVENT";
  }
}

void RedjetTelemetryJournal::log_event(RedjetEventType type, uint32_t thread_id, uint16_t cpu_id,
                                       uint32_t duration_ns, uint64_t p0, uint64_t p1,
                                       uint64_t p2, uint64_t p3, const char* tag) noexcept {
  uint64_t seq = _write_head.fetch_add(1, std::memory_order_relaxed);
  size_t idx = seq & RING_BUFFER_MASK;

  RedjetTraceRecord* rec = &_records[idx];
  rec->timestamp_tsc = hs_read_tsc();
  rec->thread_id = thread_id;
  rec->cpu_id = cpu_id;
  rec->event_type = type;
  rec->duration_ns = duration_ns;
  rec->payload[0] = p0;
  rec->payload[1] = p1;
  rec->payload[2] = p2;
  rec->payload[3] = p3;

  if (tag != nullptr) {
    std::strncpy(rec->tag, tag, sizeof(rec->tag) - 1);
    rec->tag[sizeof(rec->tag) - 1] = '\0';
  } else {
    rec->tag[0] = '\0';
  }

  _total_events_logged.fetch_add(1, std::memory_order_relaxed);
  if (seq >= RING_BUFFER_CAPACITY) {
    _total_buffer_overflows.fetch_add(1, std::memory_order_relaxed);
  }
}

size_t RedjetTelemetryJournal::export_binary_trace(uint8_t* dest_buffer, size_t buffer_size, size_t max_records) {
  if (dest_buffer == nullptr || buffer_size == 0) return 0;

  uint64_t current_head = _write_head.load(std::memory_order_relaxed);
  size_t available = std::min<size_t>(current_head, RING_BUFFER_CAPACITY);
  size_t to_export = std::min({ available, max_records, buffer_size / sizeof(RedjetTraceRecord) });

  size_t start_seq = (current_head > to_export) ? (current_head - to_export) : 0;

  for (size_t i = 0; i < to_export; ++i) {
    size_t idx = (start_seq + i) & RING_BUFFER_MASK;
    std::memcpy(dest_buffer + (i * sizeof(RedjetTraceRecord)), &_records[idx], sizeof(RedjetTraceRecord));
  }

  _total_traces_exported.fetch_add(to_export, std::memory_order_relaxed);
  return to_export * sizeof(RedjetTraceRecord);
}

size_t RedjetTelemetryJournal::export_chrome_trace_json(char* json_buffer, size_t buffer_capacity, size_t max_records) {
  if (json_buffer == nullptr || buffer_capacity < 1024) return 0;

  uint64_t current_head = _write_head.load(std::memory_order_relaxed);
  size_t available = std::min<size_t>(current_head, RING_BUFFER_CAPACITY);
  size_t to_export = std::min(available, max_records);

  size_t start_seq = (current_head > to_export) ? (current_head - to_export) : 0;

  size_t offset = 0;
  offset += std::snprintf(json_buffer + offset, buffer_capacity - offset, "{\"traceEvents\": [\n");

  for (size_t i = 0; i < to_export; ++i) {
    if (offset + 512 >= buffer_capacity) break; // Buffer full protection

    size_t idx = (start_seq + i) & RING_BUFFER_MASK;
    const RedjetTraceRecord* r = &_records[idx];

    double ts_us = static_cast<double>(r->timestamp_tsc) / 3000.0; // Normalized to microseconds
    double dur_us = static_cast<double>(r->duration_ns) / 1000.0;

    const char* comma = (i + 1 < to_export) ? "," : "";

    offset += std::snprintf(json_buffer + offset, buffer_capacity - offset,
      "  {\"name\": \"%s\", \"cat\": \"redjet_gc\", \"ph\": \"X\", \"ts\": %.2f, \"dur\": %.2f, "
      "\"pid\": 1, \"tid\": %u, \"args\": {\"cpu\": %u, \"tag\": \"%s\", \"p0\": %llu, \"p1\": %llu}}%s\n",
      event_type_to_string(r->event_type), ts_us, dur_us,
      r->thread_id, static_cast<uint32_t>(r->cpu_id), r->tag,
      static_cast<unsigned long long>(r->payload[0]),
      static_cast<unsigned long long>(r->payload[1]),
      comma);
  }

  if (offset + 32 < buffer_capacity) {
    offset += std::snprintf(json_buffer + offset, buffer_capacity - offset, "],\n\"displayTimeUnit\": \"ns\"}\n");
  }

  _total_traces_exported.fetch_add(to_export, std::memory_order_relaxed);
  return offset;
}

bool RedjetTelemetryJournal::dump_trace_to_file(const char* filepath, bool format_as_json) {
  if (filepath == nullptr) return false;

  FILE* fp = std::fopen(filepath, format_as_json ? "w" : "wb");
  if (fp == nullptr) return false;

  if (format_as_json) {
    constexpr size_t BUF_SZ = 1024 * 1024; // 1 MB scratch
    char* json_buf = reinterpret_cast<char*>(std::malloc(BUF_SZ));
    if (json_buf != nullptr) {
      size_t written = export_chrome_trace_json(json_buf, BUF_SZ, 4096);
      std::fwrite(json_buf, 1, written, fp);
      std::free(json_buf);
    }
  } else {
    constexpr size_t BIN_SZ = 512 * 1024;
    uint8_t* bin_buf = reinterpret_cast<uint8_t*>(std::malloc(BIN_SZ));
    if (bin_buf != nullptr) {
      size_t written = export_binary_trace(bin_buf, BIN_SZ, 4096);
      std::fwrite(bin_buf, 1, written, fp);
      std::free(bin_buf);
    }
  }

  std::fclose(fp);
  return true;
}

void RedjetTelemetryJournal::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Telemetry Journal & Chrome Trace Engine ---");
  st->print_cr("  Ring Buffer Capacity: %zu records (%zu bytes)",
               RING_BUFFER_CAPACITY, RING_BUFFER_CAPACITY * sizeof(RedjetTraceRecord));
  st->print_cr("  Total Events Logged:  %llu", static_cast<unsigned long long>(_total_events_logged.load(std::memory_order_relaxed)));
  st->print_cr("  Buffer Overflows:     %llu", static_cast<unsigned long long>(_total_buffer_overflows.load(std::memory_order_relaxed)));
  st->print_cr("  Traces Exported:      %llu", static_cast<unsigned long long>(_total_traces_exported.load(std::memory_order_relaxed)));
}
