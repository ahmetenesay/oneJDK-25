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

#ifndef SHARE_GC_REDJET_REDJETMONITORINGSUPPORT_HPP
#define SHARE_GC_REDJET_REDJETMONITORINGSUPPORT_HPP

#include "memory/allocation.hpp"

class GenerationCounters;
class RedjetSpaceCounters;
class RedjetHeap;
class PerfVariable;

class RedjetMonitoringSupport : public CHeapObj<mtGC> {
private:
  GenerationCounters*  _heap_counters;
  RedjetSpaceCounters* _space_counters;

  // EmptyBox live telemetry
  PerfVariable*       _emptybox_created;
  PerfVariable*       _emptybox_claimed;
  PerfVariable*       _emptybox_timeouts;
  PerfVariable*       _emptybox_active_bytes;

  // MLTC live telemetry
  PerfVariable*       _mltc_bandwidth;
  PerfVariable*       _mltc_congestion_score;
  PerfVariable*       _mltc_paced_allocs;
  PerfVariable*       _mltc_stall_ns;
  PerfVariable*       _mltc_leak_tier;

  // RMRP live telemetry
  PerfVariable*       _rmrp_plump_reclaimed;
  PerfVariable*       _rmrp_seq_reclaimed;
  PerfVariable*       _rmrp_vfs_reclaimed;
  PerfVariable*       _rmrp_zero_dedup;
  PerfVariable*       _rmrp_vfs_triggers;

public:
  RedjetMonitoringSupport(RedjetHeap* heap);
  void update_counters();
};

#endif // SHARE_GC_REDJET_REDJETMONITORINGSUPPORT_HPP
