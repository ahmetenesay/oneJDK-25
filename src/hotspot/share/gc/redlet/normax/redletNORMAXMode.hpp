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

#ifndef SHARE_GC_REDLET_NORMAX_REDLETNORMAXMODE_HPP
#define SHARE_GC_REDLET_NORMAX_REDLETNORMAXMODE_HPP

class RedletNORMAXHeuristics;

class RedletNORMAXMode {
public:
  RedletNORMAXMode() = default;
  virtual ~RedletNORMAXMode() = default;

  virtual void initialize_flags() const;
  virtual RedletNORMAXHeuristics* initialize_heuristics() const;
  virtual const char* name() const { return "NORMAX"; }
  virtual bool is_safety_critical() const { return false; }
};

#endif // SHARE_GC_REDLET_NORMAX_REDLETNORMAXMODE_HPP
