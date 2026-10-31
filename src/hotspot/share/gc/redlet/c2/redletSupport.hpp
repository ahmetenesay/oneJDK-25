/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_C2_REDLETSUPPORT_HPP
#define SHARE_GC_REDLET_C2_REDLETSUPPORT_HPP

#ifndef REDLET_STANDALONE_BUILD
#include "memory/allStatic.hpp"
#else
class AllStatic {};
#endif

class Node;
class PhaseGVN;
class PhaseIterGVN;

class RedletSupport : public AllStatic {
public:
  static bool is_redlet_barrier(const Node* node);
  static void verify_graph(PhaseIterGVN* igvn);
  static Node* find_bottom_mem(Node* ctrl, PhaseGVN* phase);
};

#endif // SHARE_GC_REDLET_C2_REDLETSUPPORT_HPP
