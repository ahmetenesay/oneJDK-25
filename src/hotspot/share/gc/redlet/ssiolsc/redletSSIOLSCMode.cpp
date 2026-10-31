/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/ssiolsc/redletSSIOLSCMode.hpp"
#include "gc/redlet/ssiolsc/redletSSIOLSCHeuristics.hpp"
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

RedletSSIOLSCMode::RedletSSIOLSCMode(SF_custominiVm_scgc_type_t scgc)
  : _scgc_type(scgc) {}

void RedletSSIOLSCMode::initialize_flags() const {
#ifndef REDLET_STANDALONE_BUILD
  FLAG_SET_DEFAULT(RedletProfile, "SSIOLSC");
  FLAG_SET_DEFAULT(RedletSCMode, true);
  FLAG_SET_DEFAULT(RedletForbidDynamicGrowth, true);
  FLAG_SET_DEFAULT(RedletElasticTLAB, false);
  FLAG_SET_DEFAULT(AlwaysPreTouch, true);
  if (_scgc_type == SF_SCGC_TYPE_PRIMITIVE_RW) {
    FLAG_SET_DEFAULT(RedletSCGCType, "PrimitiveRW");
  } else {
    FLAG_SET_DEFAULT(RedletSCGCType, "Default");
  }
#endif
  SacFor_Gearkit_SSIOLSC_Ex(_scgc_type);
}

RedletSSIOLSCHeuristics* RedletSSIOLSCMode::initialize_heuristics() const {
  return new RedletSSIOLSCHeuristics(_scgc_type);
}
