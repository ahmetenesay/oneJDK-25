/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETBARRIERSET_HPP
#define SHARE_GC_REDLET_REDLETBARRIERSET_HPP

#include "gc/shared/barrierSet.hpp"

class RedletBarrierSet: public BarrierSet {
  friend class VMStructs;

public:
  RedletBarrierSet();

  void print_on(outputStream *st) const override {}

  void on_thread_create(Thread* thread) override;
  void on_thread_destroy(Thread* thread) override;

  template <DecoratorSet decorators, typename BarrierSetT = RedletBarrierSet>
  class AccessBarrier: public BarrierSet::AccessBarrier<decorators, BarrierSetT> {};
};

template<>
struct BarrierSet::GetName<RedletBarrierSet> {
  static const BarrierSet::Name value = BarrierSet::RedletBarrierSet;
};

template<>
struct BarrierSet::GetType<BarrierSet::RedletBarrierSet> {
  typedef ::RedletBarrierSet type;
};

#endif // SHARE_GC_REDLET_REDLETBARRIERSET_HPP
