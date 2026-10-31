/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_C1_REDLETBARRIERSETC1_HPP
#define SHARE_GC_REDLET_C1_REDLETBARRIERSETC1_HPP

#ifndef REDLET_STANDALONE_BUILD
#include "gc/shared/c1/barrierSetC1.hpp"
#else
#include <cstddef>
#include <cstdint>

class LIRAccess;
class LIR_Opr {
public:
  int _val{ 0 };
};

class BarrierSetC1 {
public:
  virtual ~BarrierSetC1() = default;
  virtual LIR_Opr resolve_address(LIRAccess& access, bool resolve_in_register) { return LIR_Opr(); }
  virtual void load_at_resolved(LIRAccess& access, LIR_Opr result) {}
  virtual void store_at_resolved(LIRAccess& access, LIR_Opr value) {}
};
#endif

class RedletBarrierSetC1 : public BarrierSetC1 {
protected:
  LIR_Opr resolve_address(LIRAccess& access, bool resolve_in_register) override;

public:
  RedletBarrierSetC1() = default;

  void load_at_resolved(LIRAccess& access, LIR_Opr result) override;
  void store_at_resolved(LIRAccess& access, LIR_Opr value) override;
};

#endif // SHARE_GC_REDLET_C1_REDLETBARRIERSETC1_HPP
