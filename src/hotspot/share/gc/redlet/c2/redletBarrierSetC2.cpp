/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/c2/redletBarrierSetC2.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "opto/graphKit.hpp"
#endif

Node* RedletBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  return BarrierSetC2::store_at_resolved(access, val);
}

Node* RedletBarrierSetC2::load_at_resolved(C2Access& access, const Type* val_type) const {
  return BarrierSetC2::load_at_resolved(access, val_type);
}

Node* RedletBarrierSetC2::atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access,
                                                         Node* expected_val,
                                                         Node* new_val,
                                                         const Type* val_type) const {
  return BarrierSetC2::atomic_cmpxchg_val_at_resolved(access, expected_val, new_val, val_type);
}

Node* RedletBarrierSetC2::atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access,
                                                          Node* expected_val,
                                                          Node* new_val,
                                                          const Type* value_type) const {
  return BarrierSetC2::atomic_cmpxchg_bool_at_resolved(access, expected_val, new_val, value_type);
}

Node* RedletBarrierSetC2::atomic_xchg_at_resolved(C2AtomicParseAccess& access,
                                                  Node* new_val,
                                                  const Type* val_type) const {
  return BarrierSetC2::atomic_xchg_at_resolved(access, new_val, val_type);
}

Node* RedletBarrierSetC2::atomic_add_at_resolved(C2AtomicParseAccess& access,
                                                 Node* new_val,
                                                 const Type* val_type) const {
  return BarrierSetC2::atomic_add_at_resolved(access, new_val, val_type);
}
