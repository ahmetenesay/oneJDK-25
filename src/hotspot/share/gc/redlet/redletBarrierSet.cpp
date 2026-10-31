/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/redletBarrierSet.hpp"
#include "gc/redlet/redletThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "runtime/javaThread.hpp"
#ifdef COMPILER1
#include "gc/redlet/c1/redletBarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/redlet/c2/redletBarrierSetC2.hpp"
#endif

class RedletBarrierSetC1;
class RedletBarrierSetC2;

RedletBarrierSet::RedletBarrierSet() : BarrierSet(
          make_barrier_set_assembler<BarrierSetAssembler>(),
          make_barrier_set_c1<RedletBarrierSetC1>(),
          make_barrier_set_c2<RedletBarrierSetC2>(),
          nullptr /* barrier_set_nmethod */,
          nullptr /* barrier_set_stack_chunk */,
          BarrierSet::FakeRtti(BarrierSet::RedletBarrierSet)) {}

void RedletBarrierSet::on_thread_create(Thread* thread) {
  RedletThreadLocalData::create(thread);
}

void RedletBarrierSet::on_thread_destroy(Thread* thread) {
  RedletThreadLocalData::destroy(thread);
}
