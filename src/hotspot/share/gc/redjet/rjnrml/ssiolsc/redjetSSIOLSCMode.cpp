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

#include "gc/redjet/rjnrml/ssiolsc/redjetSSIOLSCMode.hpp"
#include "gc/redjet/rjnrml/ssiolsc/redjetSSIOLSCHeuristics.hpp"
#include "gc/redjet/rjnrml/redjetGearkit.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/rjnrml/redjet_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals_extension.hpp"
#else
#include <cstdio>
#define log_info(...) if (false) printf
#define FLAG_SET_ERGO(flag, value) ((void)0)
#define FLAG_SET_DEFAULT(flag, value) ((void)0)
#endif

RedjetSSIOLSCMode::RedjetSSIOLSCMode(HS_custominiVm_scgc_type_t scgc)
  : _scgc_type(scgc) {}

void RedjetSSIOLSCMode::initialize_flags() const {
#ifndef REDJET_STANDALONE_BUILD
  FLAG_SET_DEFAULT(RedjetProfile, "SSIOLSC");
  FLAG_SET_DEFAULT(RedjetSCMode, true);
  FLAG_SET_DEFAULT(RedjetForbidDynamicGrowth, true);
  FLAG_SET_DEFAULT(RedjetElasticTLAB, false);
  FLAG_SET_DEFAULT(AlwaysPreTouch, true);
  if (_scgc_type == HS_SCGC_TYPE_PRIMITIVE_RW) {
    FLAG_SET_DEFAULT(RedjetSCGCType, "PrimitiveRW");
  } else {
    FLAG_SET_DEFAULT(RedjetSCGCType, "Default");
  }
#endif
  SacFor_Gearkit_SSIOLSC_Ex(_scgc_type);
}

RedjetSSIOLSCHeuristics* RedjetSSIOLSCMode::initialize_heuristics() const {
  return new RedjetSSIOLSCHeuristics(_scgc_type);
}
