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

#include "gc/redjet/rjnrml/c2/redjetBarrierSetC2.hpp"
#include "gc/redjet/rjnrml/c2/redjetSupport.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/rjnrml/redjetPointCoordinator.hpp"
#include "gc/shared/gc_globals.hpp"
#include "opto/connode.hpp"
#include "opto/graphKit.hpp"
#include "opto/type.hpp"
#endif

#ifndef REDJET_STANDALONE_BUILD
static const TypeFunc* redjet_hookpoint_Type() {
  const Type** fields = TypeTuple::fields(3);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::NOTNULL; // JavaThread* thread
  fields[TypeFunc::Parms + 1] = TypePtr::BOTTOM;     // void* target_oop
  fields[TypeFunc::Parms + 2] = TypeInt::INT;        // uint32_t fn_id
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 3, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}
#endif

Node* RedjetBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  return BarrierSetC2::store_at_resolved(access, val);
}

Node* RedjetBarrierSetC2::load_at_resolved(C2Access& access, const Type* val_type) const {
  return BarrierSetC2::load_at_resolved(access, val_type);
}

Node* RedjetBarrierSetC2::atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access,
                                                         Node* expected_val,
                                                         Node* new_val,
                                                         const Type* val_type) const {
  return BarrierSetC2::atomic_cmpxchg_val_at_resolved(access, expected_val, new_val, val_type);
}

Node* RedjetBarrierSetC2::atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access,
                                                          Node* expected_val,
                                                          Node* new_val,
                                                          const Type* value_type) const {
  return BarrierSetC2::atomic_cmpxchg_bool_at_resolved(access, expected_val, new_val, value_type);
}

Node* RedjetBarrierSetC2::atomic_xchg_at_resolved(C2AtomicParseAccess& access,
                                                  Node* new_val,
                                                  const Type* val_type) const {
  return BarrierSetC2::atomic_xchg_at_resolved(access, new_val, val_type);
}

Node* RedjetBarrierSetC2::atomic_add_at_resolved(C2AtomicParseAccess& access,
                                                 Node* new_val,
                                                 const Type* val_type) const {
  return BarrierSetC2::atomic_add_at_resolved(access, new_val, val_type);
}

void RedjetBarrierSetC2::emit_hookpoint_pin_enter(GraphKit* kit, Node* target_oop, uint32_t fn_id) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped()) {
    return;
  }
  Node* thread = kit->gvn().transform(new ThreadLocalNode());
  Node* oop_arg = (target_oop != nullptr) ? target_oop : kit->makecon(TypeRawPtr::NULL_PTR);
  Node* id_arg = kit->intcon(static_cast<jint>(fn_id));
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_hookpoint_Type(),
                         CAST_FROM_FN_PTR(address, redjet_rt_hookpoint_enter),
                         "redjet_hookpoint_enter",
                         nullptr,
                         thread,
                         oop_arg,
                         id_arg);
#else
  (void)kit;
  (void)target_oop;
  (void)fn_id;
#endif
}

void RedjetBarrierSetC2::emit_hookpoint_pin_exit(GraphKit* kit, Node* target_oop, uint32_t fn_id) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped()) {
    return;
  }
  Node* thread = kit->gvn().transform(new ThreadLocalNode());
  Node* oop_arg = (target_oop != nullptr) ? target_oop : kit->makecon(TypeRawPtr::NULL_PTR);
  Node* id_arg = kit->intcon(static_cast<jint>(fn_id));
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_hookpoint_Type(),
                         CAST_FROM_FN_PTR(address, redjet_rt_hookpoint_exit),
                         "redjet_hookpoint_exit",
                         nullptr,
                         thread,
                         oop_arg,
                         id_arg);
#else
  (void)kit;
  (void)target_oop;
  (void)fn_id;
#endif
}

void RedjetBarrierSetC2::emit_pausepoint_poll(GraphKit* kit) {
  RedjetSupport::emit_pausepoint_fast_poll(kit);
}

bool RedjetBarrierSetC2::is_accelerated_kernel(ciMethod* method) {
  return RedjetSupport::is_redjet_accelerated_method(method);
}
