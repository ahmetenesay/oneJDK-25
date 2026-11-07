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

#include "gc/redjet/runtime/redjetBarrierSetMIRC.hpp"
#include "gc/redjet/runtime/redjetSupport.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/api/redjetTailTraces.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "gc/redjet/vfs/redjetVFSNode_CodeBoot.hpp"
#include "gc/shared/gc_globals.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "mirc/connode.hpp"
#include "mirc/graphKit.hpp"
#include "mirc/type.hpp"
#include "mirc/memnode.hpp"
#include "mirc/cfgnode.hpp"
#include "mirc/subnode.hpp"
#include "ci/ciMethod.hpp"
#endif

// ============================================================================
// C Runtime Helper Leaf Types & Functions
// ============================================================================

extern "C" JNIEXPORT void redjet_rt_dead_ref_signal(void* rpss, void* oop_ptr) {
  if (oop_ptr == nullptr) return;
  g_redjet_tail_traces.emit_dead_reference_signal(0, oop_ptr, 0);
}

extern "C" JNIEXPORT void redjet_offheap_evict(void* handle) {
  if (handle == nullptr) {
    return;
  }
  // Immediate deterministic RAII eviction trap for marked off-heap handles
  g_redjet_tail_traces.cancel_signal_on_escape(handle);

  RedjetHeap* heap = RedjetHeap::heap();
  if (heap != nullptr) {
    RedjetVFSTree* vfs = heap->vfs_tree();
    if (vfs != nullptr) {
      RedjetVFSNode* code_boot = vfs->find_node("/heap/off_heap/code_boot");
      if (code_boot != nullptr) {
        RedjetVFSNode_CodeBoot* cb = static_cast<RedjetVFSNode_CodeBoot*>(code_boot);
        cb->free_code_page(handle);
      }
    }
  }
}

#ifndef REDJET_STANDALONE_BUILD
static const TypeFunc* redjet_mirc_hookpoint_Type() {
  const Type** fields = TypeTuple::fields(3);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::NOTNULL; // JavaThread* thread
  fields[TypeFunc::Parms + 1] = TypePtr::BOTTOM;     // void* target_oop
  fields[TypeFunc::Parms + 2] = TypeInt::INT;        // uint32_t fn_id
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 3, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}

static const TypeFunc* redjet_mirc_offheap_evict_Type() {
  const Type** fields = TypeTuple::fields(1);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::NOTNULL; // void* handle_ptr
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 1, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}

static const TypeFunc* redjet_mirc_deadref_Type() {
  const Type** fields = TypeTuple::fields(2);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::BOTTOM;  // void* rpss
  fields[TypeFunc::Parms + 1] = TypePtr::BOTTOM;     // void* oop_ptr
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 2, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}
#endif

// ============================================================================
// BarrierSetC2 Virtual Overrides
// ============================================================================

Node* RedjetBarrierSetMIRC::store_at_resolved(C2Access& access, C2AccessValue& val) const {
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC && access.is_oop() && access.is_parse_access()) {
    C2ParseAccess& parse_access = static_cast<C2ParseAccess&>(access);
    GraphKit* kit = parse_access.kit();
    if (kit != nullptr && !kit->stopped()) {
      Node* adr = access.addr().node();
      if (adr != nullptr) {
        Node* old_val = kit->make_load(kit->control(), adr, TypeInstPtr::BOTTOM, T_OBJECT, MemNode::unordered);
        if (old_val != nullptr) {
          emit_dead_ref_signal(kit, adr, old_val);
        }
      }
    }
  }
#endif
  return BarrierSetC2::store_at_resolved(access, val);
}

Node* RedjetBarrierSetMIRC::load_at_resolved(C2Access& access, const Type* val_type) const {
  return BarrierSetC2::load_at_resolved(access, val_type);
}

Node* RedjetBarrierSetMIRC::atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access,
                                                           Node* expected_val,
                                                           Node* new_val,
                                                           const Type* val_type) const {
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC && access.is_oop()) {
    GraphKit* kit = access.kit();
    if (kit != nullptr && !kit->stopped() && expected_val != nullptr) {
      emit_dead_ref_signal(kit, access.addr().node(), expected_val);
    }
  }
#endif
  return BarrierSetC2::atomic_cmpxchg_val_at_resolved(access, expected_val, new_val, val_type);
}

Node* RedjetBarrierSetMIRC::atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access,
                                                            Node* expected_val,
                                                            Node* new_val,
                                                            const Type* value_type) const {
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC && access.is_oop()) {
    GraphKit* kit = access.kit();
    if (kit != nullptr && !kit->stopped() && expected_val != nullptr) {
      emit_dead_ref_signal(kit, access.addr().node(), expected_val);
    }
  }
#endif
  return BarrierSetC2::atomic_cmpxchg_bool_at_resolved(access, expected_val, new_val, value_type);
}

Node* RedjetBarrierSetMIRC::atomic_xchg_at_resolved(C2AtomicParseAccess& access,
                                                    Node* new_val,
                                                    const Type* val_type) const {
  Node* prev = BarrierSetC2::atomic_xchg_at_resolved(access, new_val, val_type);
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC && access.is_oop()) {
    GraphKit* kit = access.kit();
    if (kit != nullptr && !kit->stopped() && prev != nullptr) {
      emit_dead_ref_signal(kit, access.addr().node(), prev);
    }
  }
#endif
  return prev;
}

Node* RedjetBarrierSetMIRC::atomic_add_at_resolved(C2AtomicParseAccess& access,
                                                   Node* new_val,
                                                   const Type* val_type) const {
  return BarrierSetC2::atomic_add_at_resolved(access, new_val, val_type);
}

void RedjetBarrierSetMIRC::emit_dockpoint_trampoline(GraphKit* kit, Node* slot_target) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped() || slot_target == nullptr) {
    return;
  }
  if (kit->C != nullptr && kit->C->method() != nullptr) {
    redjet_rt_c1_report_method_heat(static_cast<void*>(kit->C->method()->get_Method()), 1, 32, true);
  }
#else
  (void)kit;
  (void)slot_target;
#endif
}

void RedjetBarrierSetMIRC::emit_parkpoint_poll(GraphKit* kit) {
  RedjetSupport::emit_pausepoint_fast_poll(kit);
}

void RedjetBarrierSetMIRC::emit_dead_ref_signal(GraphKit* kit, Node* rpss_addr, Node* dead_oop) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped() || dead_oop == nullptr) {
    return;
  }
  Node* rpss_node = (rpss_addr != nullptr) ? rpss_addr : kit->makecon(TypeRawPtr::NULL_PTR);
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_mirc_deadref_Type(),
                         CAST_FROM_FN_PTR(address, redjet_rt_dead_ref_signal),
                         "redjet_dead_ref_signal",
                         nullptr,
                         rpss_node,
                         dead_oop);
#else
  (void)kit;
  (void)rpss_addr;
  (void)dead_oop;
#endif
}

Node* RedjetBarrierSetMIRC::emit_dead_ref_signal_atomic(GraphKit* kit, Node* rpss_addr, Node* dead_oop) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped() || dead_oop == nullptr) {
    return nullptr;
  }
  emit_dead_ref_signal(kit, rpss_addr, dead_oop);
  return dead_oop;
#else
  (void)kit;
  (void)rpss_addr;
  (void)dead_oop;
  return nullptr;
#endif
}

void RedjetBarrierSetMIRC::emit_offheap_evict_trap(GraphKit* kit, Node* handle_ptr) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped() || handle_ptr == nullptr) {
    return;
  }
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_mirc_offheap_evict_Type(),
                         CAST_FROM_FN_PTR(address, redjet_offheap_evict),
                         "redjet_offheap_evict",
                         nullptr,
                         handle_ptr);
#else
  (void)kit;
  (void)handle_ptr;
#endif
}

void RedjetBarrierSetMIRC::emit_emptybox_guard(GraphKit* kit, Node* mem_ptr) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped() || mem_ptr == nullptr) {
    return;
  }
  PhaseGVN& gvn = kit->gvn();
  Node* hdr = kit->make_load(kit->control(), mem_ptr, TypeInt::INT, T_INT, MemNode::unordered);
  Node* magic = kit->intcon(static_cast<jint>(RedjetEmptyBox::HS_EMPTYBOX_MAGIC));
  Node* cmp = gvn.transform(new CmpINode(hdr, magic));
  Node* tst = gvn.transform(new BoolNode(cmp, BoolTest::ne));
  IfNode* iff = kit->create_and_map_if(kit->control(), tst, PROB_LIKELY_MAG(5), COUNT_UNKNOWN);
  Node* if_valid = gvn.transform(new IfTrueNode(iff));
  kit->set_control(if_valid);
#else
  (void)kit;
  (void)mem_ptr;
#endif
}

void RedjetBarrierSetMIRC::emit_hookpoint_pin_enter(GraphKit* kit, Node* target_oop, uint32_t fn_id) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped()) {
    return;
  }
  Node* thread = kit->gvn().transform(new ThreadLocalNode());
  Node* oop_arg = (target_oop != nullptr) ? target_oop : kit->makecon(TypeRawPtr::NULL_PTR);
  Node* id_arg = kit->intcon(static_cast<jint>(fn_id));
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_mirc_hookpoint_Type(),
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

void RedjetBarrierSetMIRC::emit_hookpoint_pin_exit(GraphKit* kit, Node* target_oop, uint32_t fn_id) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped()) {
    return;
  }
  Node* thread = kit->gvn().transform(new ThreadLocalNode());
  Node* oop_arg = (target_oop != nullptr) ? target_oop : kit->makecon(TypeRawPtr::NULL_PTR);
  Node* id_arg = kit->intcon(static_cast<jint>(fn_id));
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_mirc_hookpoint_Type(),
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

bool RedjetBarrierSetMIRC::is_accelerated_kernel(ciMethod* method) {
  return RedjetSupport::is_redjet_accelerated_method(method);
}
