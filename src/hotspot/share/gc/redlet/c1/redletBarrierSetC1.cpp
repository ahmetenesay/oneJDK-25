/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/c1/redletBarrierSetC1.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "c1/c1_LIRGenerator.hpp"
#endif

LIR_Opr RedletBarrierSetC1::resolve_address(LIRAccess& access, bool resolve_in_register) {
  return BarrierSetC1::resolve_address(access, resolve_in_register);
}

void RedletBarrierSetC1::load_at_resolved(LIRAccess& access, LIR_Opr result) {
  BarrierSetC1::load_at_resolved(access, result);
}

void RedletBarrierSetC1::store_at_resolved(LIRAccess& access, LIR_Opr value) {
  BarrierSetC1::store_at_resolved(access, value);
}
