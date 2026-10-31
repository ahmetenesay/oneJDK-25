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

#include "gc/redlet/normax/redletNORMAXHeuristics.hpp"
#include "gc/redlet/redletMLTC.hpp"
#include "gc/redlet/redletRunway.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#endif

RedletNORMAXHeuristics::RedletNORMAXHeuristics() {
  _max_tlab_size = 4 * 1024 * 1024;
}

bool RedletNORMAXHeuristics::should_start_gc() const {
  // Check if MLTC detects upward memory traffic trend
  if (g_sf_mltc_solver.continuous_upward_trend.load(std::memory_order_relaxed)) {
    return true;
  }
  return false;
}

void RedletNORMAXHeuristics::record_allocation(size_t bytes) {
  _allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RedletNORMAXHeuristics::record_gc_end() {
  _cycle_count.fetch_add(1, std::memory_order_relaxed);
  _allocated_bytes.store(0, std::memory_order_relaxed);
}
