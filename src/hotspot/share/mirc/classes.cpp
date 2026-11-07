/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "mirc/addnode.hpp"
#include "mirc/arraycopynode.hpp"
#include "mirc/callnode.hpp"
#include "mirc/castnode.hpp"
#include "mirc/cfgnode.hpp"
#include "mirc/connode.hpp"
#include "mirc/convertnode.hpp"
#include "mirc/countbitsnode.hpp"
#include "mirc/divnode.hpp"
#include "mirc/intrinsicnode.hpp"
#include "mirc/locknode.hpp"
#include "mirc/loopnode.hpp"
#include "mirc/machnode.hpp"
#include "mirc/memnode.hpp"
#include "mirc/mathexactnode.hpp"
#include "mirc/movenode.hpp"
#include "mirc/mulnode.hpp"
#include "mirc/multnode.hpp"
#include "mirc/narrowptrnode.hpp"
#include "mirc/node.hpp"
#include "mirc/opaquenode.hpp"
#include "mirc/rootnode.hpp"
#include "mirc/subnode.hpp"
#include "mirc/subtypenode.hpp"
#include "mirc/vectornode.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_SHENANDOAHGC
#include "gc/shenandoah/c2/shenandoahBarrierSetC2.hpp"
#endif

// ----------------------------------------------------------------------------
// Build a table of virtual functions to map from Nodes to dense integer
// opcode names.
int Node::Opcode() const { return Op_Node; }
#define macro(x) int x##Node::Opcode() const { return Op_##x; }
#define optionalmacro(x)
#include "classes.hpp"
#undef macro
#undef optionalmacro
