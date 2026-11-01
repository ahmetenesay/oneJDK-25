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

#include "gc/redjet/rjnrml/c2/redjetSupport.hpp"
#ifndef REDJET_STANDALONE_BUILD
#include "ci/ciMethod.hpp"
#include "gc/redjet/rjnrml/redjetPointCoordinator.hpp"
#include "gc/shared/gc_globals.hpp"
#include "opto/callnode.hpp"
#include "opto/cfgnode.hpp"
#include "opto/connode.hpp"
#include "opto/graphKit.hpp"
#include "opto/memnode.hpp"
#include "opto/node.hpp"
#include "opto/phaseX.hpp"
#include "opto/subnode.hpp"
#include "opto/type.hpp"
#include <cstring>
#endif

#ifndef REDJET_STANDALONE_BUILD
static const TypeFunc* redjet_pausepoint_poll_Type() {
  const Type** fields = TypeTuple::fields(2);
  fields[TypeFunc::Parms + 0] = TypeRawPtr::NOTNULL; // JavaThread* thread
  fields[TypeFunc::Parms + 1] = TypeRawPtr::BOTTOM;  // void* target_oop (nullable)
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 2, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);
  return TypeFunc::make(domain, range);
}
#endif

bool RedjetSupport::is_redjet_barrier(const Node* node) {
#ifndef REDJET_STANDALONE_BUILD
  if (node == nullptr) {
    return false;
  }
  if (node->is_CallLeaf()) {
    const CallLeafNode* call = node->as_CallLeaf();
    if (call->_name != nullptr &&
        (std::strncmp(call->_name, "redjet_", 7) == 0)) {
      return true;
    }
  }
  return false;
#else
  (void)node;
  return false;
#endif
}

void RedjetSupport::verify_graph(PhaseIterGVN* igvn) {
#ifndef REDJET_STANDALONE_BUILD
  if (igvn == nullptr || igvn->C == nullptr || igvn->C->root() == nullptr) {
    return;
  }
  RootNode* root = igvn->C->root();
  assert(root->req() >= 1, "Redjet C2 Ideal root node must remain connected");
#else
  (void)igvn;
#endif
}

Node* RedjetSupport::find_bottom_mem(Node* ctrl, PhaseGVN* phase) {
#ifndef REDJET_STANDALONE_BUILD
  if (ctrl == nullptr || phase == nullptr) {
    return nullptr;
  }
  Node* cur = ctrl;
  for (int steps = 0; cur != nullptr && steps < 64; ++steps) {
    if (cur->is_SafePoint()) {
      Node* mem = cur->as_SafePoint()->memory();
      if (mem != nullptr) {
        if (mem->is_MergeMem()) {
          return mem->as_MergeMem()->base_memory();
        }
        return mem;
      }
    }
    if (cur->is_Region()) {
      cur = (cur->req() > 1) ? cur->in(1) : nullptr;
    } else {
      cur = (cur->req() > 0) ? cur->in(0) : nullptr;
    }
  }
  return nullptr;
#else
  (void)ctrl;
  (void)phase;
  return nullptr;
#endif
}

bool RedjetSupport::is_redjet_accelerated_method(ciMethod* method) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || method == nullptr || !method->is_loaded()) {
    return false;
  }
  return method->redjet_batch_exec() ||
         method->redjet_nested_exec() ||
         method->redjet_bypass_warmup() ||
         method->redjet_exempt_pin();
#else
  (void)method;
  return false;
#endif
}

bool RedjetSupport::should_bypass_c2_loop_safepoint(ciMethod* method) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || method == nullptr || !method->is_loaded()) {
    return false;
  }
  return method->redjet_batch_exec() ||
         method->redjet_nested_exec() ||
         method->redjet_bypass_warmup();
#else
  (void)method;
  return false;
#endif
}

void RedjetSupport::emit_pausepoint_fast_poll(GraphKit* kit) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || kit->stopped()) {
    return;
  }

  PhaseGVN& gvn = kit->gvn();
  address gate_ptr = (address)RedjetPointCoordinator::pausepoint_gate_address();
  if (gate_ptr == nullptr) {
    return;
  }

  Node* gate_adr = kit->makecon(TypeRawPtr::make(gate_ptr));
  Node* gate_val = kit->make_load(kit->control(),
                                  gate_adr,
                                  TypeInt::INT,
                                  T_INT,
                                  MemNode::unordered);
  Node* cmp = gvn.transform(new CmpINode(gate_val, kit->intcon(0)));
  Node* tst = gvn.transform(new BoolNode(cmp, BoolTest::ne));

  IfNode* iff = kit->create_and_map_if(kit->control(), tst, PROB_UNLIKELY_MAG(5), COUNT_UNKNOWN);
  Node* if_slow = gvn.transform(new IfTrueNode(iff));
  Node* if_fast = gvn.transform(new IfFalseNode(iff));

  RegionNode* merge = new RegionNode(3);
  merge->init_req(1, if_fast);

  // Cold path: invoke non-STW Pausepoint slow handler for Pitstop ISA hot-swap / coordination
  kit->set_control(if_slow);
  Node* thread = gvn.transform(new ThreadLocalNode());
  Node* null_oop = kit->makecon(TypeRawPtr::NULL_PTR);
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         redjet_pausepoint_poll_Type(),
                         CAST_FROM_FN_PTR(address, redjet_rt_pausepoint_poll_slow),
                         "redjet_pausepoint_poll",
                         nullptr, // no_memory_effects
                         thread,
                         null_oop);
  merge->init_req(2, kit->control());

  kit->set_control(gvn.transform(merge));
  kit->record_for_igvn(merge);
#else
  (void)kit;
#endif
}

void RedjetSupport::emit_method_entry_hook(GraphKit* kit, ciMethod* method) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || kit == nullptr || method == nullptr || !method->is_loaded() || kit->stopped()) {
    return;
  }
  if (!method->redjet_batch_exec() &&
      !method->redjet_exempt_pin() &&
      !method->redjet_nested_exec() &&
      !method->redjet_bypass_warmup()) {
    return;
  }

  // Zero-overhead (<1 cycle) Pausepoint gate check on C2 method entry;
  // cold path (PROB_UNLIKELY_MAG(5)) calls redjet_rt_pausepoint_poll_slow.
  emit_pausepoint_fast_poll(kit);
#else
  (void)kit;
  (void)method;
#endif
}
