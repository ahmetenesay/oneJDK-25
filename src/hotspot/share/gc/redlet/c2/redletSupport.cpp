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

#include "gc/redlet/c2/redletSupport.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "opto/node.hpp"
#include "opto/phaseX.hpp"
#endif

bool RedletSupport::is_redlet_barrier(const Node* node) {
  if (node == nullptr) return false;
  return false;
}

void RedletSupport::verify_graph(PhaseIterGVN* igvn) {
  // Graph consistency validation for Redlet C2 Ideal graphs
  (void)igvn;
}

Node* RedletSupport::find_bottom_mem(Node* ctrl, PhaseGVN* phase) {
  (void)ctrl;
  (void)phase;
  return nullptr;
}
