/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
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
