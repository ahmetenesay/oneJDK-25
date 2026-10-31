/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
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
