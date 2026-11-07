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

#ifndef SHARE_GC_REDJET_C2_REDJETSUPPORT_HPP
#define SHARE_GC_REDJET_C2_REDJETSUPPORT_HPP

#ifndef REDJET_STANDALONE_BUILD
#include "memory/allStatic.hpp"
#else
class AllStatic {};
#endif

class ciMethod;
class GraphKit;
class Node;
class PhaseGVN;
class PhaseIterGVN;

// C++14-clean C2 Ideal Graph support for Redjet SFVM 4-Point Coordination & SIMD acceleration.
class RedjetSupport : public AllStatic {
public:
  static bool is_redjet_barrier(const Node* node);
  static void verify_graph(PhaseIterGVN* igvn);
  static Node* find_bottom_mem(Node* ctrl, PhaseGVN* phase);

  // Returns true if the method carries any Redjet execution/pinning annotation.
  static bool is_redjet_accelerated_method(ciMethod* method);

  // Returns true if heavy STW C2 SafePoint nodes should be replaced by
  // Redjet's non-STW Pausepoint fast gate poll.
  static bool should_bypass_c2_loop_safepoint(ciMethod* method);

  // Emits <1-cycle fast check of g_redjet_pausepoint_gate_flag at C2 method entry
  // for @RedjetBatchExec / @RedjetExemptPin / @RedjetBypassWarmup methods,
  // branching to redjet_rt_pausepoint_poll_slow only on PROB_UNLIKELY_MAG(5).
  static void emit_method_entry_hook(GraphKit* kit, ciMethod* method);

  // Emits lightweight non-STW Pausepoint gate poll in C2 Ideal IR so hot SIMD
  // loops participate in Pitstop ISA hot-swaps without full STW safepoint tax.
  static void emit_pausepoint_fast_poll(GraphKit* kit);
};

#endif // SHARE_GC_REDJET_C2_REDJETSUPPORT_HPP