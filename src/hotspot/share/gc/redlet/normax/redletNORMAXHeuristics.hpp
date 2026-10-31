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

#ifndef SHARE_GC_REDLET_NORMAX_REDLETNORMAXHEURISTICS_HPP
#define SHARE_GC_REDLET_NORMAX_REDLETNORMAXHEURISTICS_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>

class RedletNORMAXHeuristics {
private:
  std::atomic<size_t>   _allocated_bytes{ 0 };
  std::atomic<uint64_t> _cycle_count{ 0 };
  size_t                _max_tlab_size{ 4 * 1024 * 1024 };

public:
  RedletNORMAXHeuristics();
  virtual ~RedletNORMAXHeuristics() = default;

  virtual const char* name() const { return "NORMAX"; }
  virtual bool should_start_gc() const;
  virtual bool is_dynamic_growth_allowed() const { return true; }
  virtual size_t max_tlab_size() const { return _max_tlab_size; }
  virtual void record_allocation(size_t bytes);
  virtual void record_gc_end();
};

#endif // SHARE_GC_REDLET_NORMAX_REDLETNORMAXHEURISTICS_HPP
