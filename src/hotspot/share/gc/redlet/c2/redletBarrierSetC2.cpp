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
