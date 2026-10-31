/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_C2_REDLETBARRIERSETC2_HPP
#define SHARE_GC_REDLET_C2_REDLETBARRIERSETC2_HPP

#ifndef REDLET_STANDALONE_BUILD
#include "gc/shared/c2/barrierSetC2.hpp"
#else
#include <cstddef>
#include <cstdint>

class Node;
class Type;
class C2Access;
class C2AccessValue;
class C2AtomicParseAccess;

class BarrierSetC2 {
public:
  virtual ~BarrierSetC2() = default;
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const { return nullptr; }
  virtual Node* load_at_resolved(C2Access& access, const Type* val_type) const { return nullptr; }
  virtual Node* atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                               Node* new_val, const Type* val_type) const { return nullptr; }
  virtual Node* atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                Node* new_val, const Type* value_type) const { return nullptr; }
  virtual Node* atomic_xchg_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* val_type) const { return nullptr; }
  virtual Node* atomic_add_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* val_type) const { return nullptr; }
};
#endif

class RedletBarrierSetC2 : public BarrierSetC2 {
protected:
  Node* store_at_resolved(C2Access& access, C2AccessValue& val) const override;
  Node* load_at_resolved(C2Access& access, const Type* val_type) const override;

  Node* atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access,
                                       Node* expected_val,
                                       Node* new_val,
                                       const Type* val_type) const override;

  Node* atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access,
                                        Node* expected_val,
                                        Node* new_val,
                                        const Type* value_type) const override;

  Node* atomic_xchg_at_resolved(C2AtomicParseAccess& access,
                                Node* new_val,
                                const Type* val_type) const override;

  Node* atomic_add_at_resolved(C2AtomicParseAccess& access,
                               Node* new_val,
                               const Type* val_type) const override;

public:
  RedletBarrierSetC2() = default;
};

#endif // SHARE_GC_REDLET_C2_REDLETBARRIERSETC2_HPP
