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
