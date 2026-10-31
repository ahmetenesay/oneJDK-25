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

#include "gc/redlet/normax/redletNORMAXMode.hpp"
#include "gc/redlet/normax/redletNORMAXHeuristics.hpp"
#include "gc/redlet/redletGearkit.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals_extension.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#define FLAG_SET_ERGO(flag, value) ((void)0)
#define FLAG_SET_DEFAULT(flag, value) ((void)0)
#endif

void RedletNORMAXMode::initialize_flags() const {
#ifndef REDLET_STANDALONE_BUILD
  FLAG_SET_DEFAULT(RedletProfile, "NORMAX");
  FLAG_SET_DEFAULT(RedletSCMode, false);
  FLAG_SET_DEFAULT(RedletForbidDynamicGrowth, false);
  FLAG_SET_DEFAULT(RedletElasticTLAB, true);
  FLAG_SET_DEFAULT(RedletHeteroArenas, true);
#endif
  SacFor_Gearkit_NORMAX();
}

RedletNORMAXHeuristics* RedletNORMAXMode::initialize_heuristics() const {
  return new RedletNORMAXHeuristics();
}
