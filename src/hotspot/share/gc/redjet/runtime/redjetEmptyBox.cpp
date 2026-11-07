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

#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include <atomic>

namespace {
    std::atomic<uint64_t> g_eb_reservations_created{ 0 };
    std::atomic<uint64_t> g_eb_reservations_claimed{ 0 };
    std::atomic<uint64_t> g_eb_timeouts_detected{ 0 };
    std::atomic<uint64_t> g_eb_theft_prevented{ 0 };
    std::atomic<uint64_t> g_eb_tombstones_scrubbed{ 0 };
    std::atomic<size_t>   g_eb_active_reserved_bytes{ 0 };
}

void RedjetEmptyBoxManager::record_reservation(size_t bytes) {
    g_eb_reservations_created.fetch_add(1, std::memory_order_relaxed);
    g_eb_active_reserved_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RedjetEmptyBoxManager::record_claim(size_t bytes) {
    g_eb_reservations_claimed.fetch_add(1, std::memory_order_relaxed);
    if (g_eb_active_reserved_bytes.load(std::memory_order_relaxed) >= bytes) {
        g_eb_active_reserved_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

void RedjetEmptyBoxManager::record_timeout() {
    g_eb_timeouts_detected.fetch_add(1, std::memory_order_relaxed);
}

void RedjetEmptyBoxManager::record_theft_prevented() {
    g_eb_theft_prevented.fetch_add(1, std::memory_order_relaxed);
}

void RedjetEmptyBoxManager::record_scrub(size_t bytes) {
    g_eb_tombstones_scrubbed.fetch_add(1, std::memory_order_relaxed);
    if (g_eb_active_reserved_bytes.load(std::memory_order_relaxed) >= bytes) {
        g_eb_active_reserved_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

RedjetEmptyBoxTelemetry RedjetEmptyBoxManager::get_telemetry() {
    RedjetEmptyBoxTelemetry tel;
    tel.total_reservations_created = g_eb_reservations_created.load(std::memory_order_relaxed);
    tel.total_reservations_claimed = g_eb_reservations_claimed.load(std::memory_order_relaxed);
    tel.total_timeouts_detected = g_eb_timeouts_detected.load(std::memory_order_relaxed);
    tel.total_theft_attempts_prevented = g_eb_theft_prevented.load(std::memory_order_relaxed);
    tel.total_tombstones_scrubbed = g_eb_tombstones_scrubbed.load(std::memory_order_relaxed);
    tel.active_reserved_bytes = g_eb_active_reserved_bytes.load(std::memory_order_relaxed);
    return tel;
}

void RedjetEmptyBoxManager::reset_telemetry() {
    g_eb_reservations_created.store(0, std::memory_order_relaxed);
    g_eb_reservations_claimed.store(0, std::memory_order_relaxed);
    g_eb_timeouts_detected.store(0, std::memory_order_relaxed);
    g_eb_theft_prevented.store(0, std::memory_order_relaxed);
    g_eb_tombstones_scrubbed.store(0, std::memory_order_relaxed);
    g_eb_active_reserved_bytes.store(0, std::memory_order_relaxed);
}
