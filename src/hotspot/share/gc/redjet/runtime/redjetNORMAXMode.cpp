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

#include "gc/redjet/runtime/redjetNORMAXMode.hpp"
#include "gc/redjet/runtime/redjetNORMAXHeuristics.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals_extension.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#define FLAG_SET_ERGO(flag, value) ((void)0)
#define FLAG_SET_DEFAULT(flag, value) ((void)0)
#endif

void RedjetNORMAXMode::initialize_flags() const {
#ifndef REDJET_STANDALONE_BUILD
  FLAG_SET_DEFAULT(RedjetProfile, "NORMAX");
  FLAG_SET_DEFAULT(RedjetSCMode, false);
  FLAG_SET_DEFAULT(RedjetForbidDynamicGrowth, false);
  FLAG_SET_DEFAULT(RedjetElasticTLAB, true);
  FLAG_SET_DEFAULT(RedjetHeteroArenas, true);
#endif
  SacFor_Gearkit_NORMAX();
}

RedjetNORMAXHeuristics* RedjetNORMAXMode::initialize_heuristics() const {
  return new RedjetNORMAXHeuristics();
}
