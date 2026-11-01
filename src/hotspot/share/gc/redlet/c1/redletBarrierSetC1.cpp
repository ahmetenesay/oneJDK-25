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

#include "gc/redlet/c1/redletBarrierSetC1.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "c1/c1_Compilation.hpp"
#include "c1/c1_IR.hpp"
#include "c1/c1_LIRGenerator.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciStreams.hpp"
#include "gc/redlet/redletPointCoordinator.hpp"
#include "gc/shared/gc_globals.hpp"
#include "interpreter/bytecodes.hpp"
#endif

LIR_Opr RedletBarrierSetC1::resolve_address(LIRAccess& access, bool resolve_in_register) {
  return BarrierSetC1::resolve_address(access, resolve_in_register);
}

void RedletBarrierSetC1::load_at_resolved(LIRAccess& access, LIR_Opr result) {
  BarrierSetC1::load_at_resolved(access, result);
}

void RedletBarrierSetC1::store_at_resolved(LIRAccess& access, LIR_Opr value) {
  BarrierSetC1::store_at_resolved(access, value);
}

bool RedletBarrierSetC1::should_suppress_loop_safepoint(ciMethod* method) {
#ifndef REDLET_STANDALONE_BUILD
  if (!UseRedletGC || method == nullptr || !method->is_loaded()) {
    return false;
  }
  return method->redlet_batch_exec() ||
         method->redlet_nested_exec() ||
         method->redlet_bypass_warmup();
#else
  (void)method;
  return false;
#endif
}

void RedletBarrierSetC1::report_iterative_bytecode_lir_heat(Compilation* comp, LIRGenerator* gen) {
#ifndef REDLET_STANDALONE_BUILD
  (void)gen;
  if (!UseRedletGC || comp == nullptr) {
    return;
  }
  ciMethod* method = comp->method();
  if (method == nullptr || !method->is_loaded() || method->code_size() <= 0) {
    return;
  }

  // First pass: discover backward branch intervals [header_bci, backedge_bci]
  // to determine exact lexical loop nesting depth at every bytecode offset.
  struct BackedgeRange {
    int start_bci;
    int end_bci;
  };
  constexpr int kMaxTrackedLoops = 32;
  BackedgeRange loops[kMaxTrackedLoops];
  int loop_count = 0;

  {
    ciBytecodeStream scan(method);
    Bytecodes::Code bc;
    while ((bc = scan.next()) != ciBytecodeStream::EOBC()) {
      const int cur = scan.cur_bci();
      int dest = -1;
      switch (bc) {
        case Bytecodes::_goto:
        case Bytecodes::_ifeq:
        case Bytecodes::_ifne:
        case Bytecodes::_iflt:
        case Bytecodes::_ifge:
        case Bytecodes::_ifgt:
        case Bytecodes::_ifle:
        case Bytecodes::_if_icmpeq:
        case Bytecodes::_if_icmpne:
        case Bytecodes::_if_icmplt:
        case Bytecodes::_if_icmpge:
        case Bytecodes::_if_icmpgt:
        case Bytecodes::_if_icmple:
        case Bytecodes::_if_acmpeq:
        case Bytecodes::_if_acmpne:
        case Bytecodes::_ifnull:
        case Bytecodes::_ifnonnull:
          dest = scan.get_dest();
          break;
        case Bytecodes::_goto_w:
          dest = scan.get_far_dest();
          break;
        default:
          break;
      }
      if (dest >= 0 && dest <= cur && loop_count < kMaxTrackedLoops) {
        loops[loop_count].start_bci = dest;
        loops[loop_count].end_bci = cur;
        ++loop_count;
      }
    }
  }

  // Second pass: accumulate LIR operation weight scaled by loop nesting depth,
  // floating-point ALU/FMA density, array load/store bandwidth, and SIMD calls.
  uint32_t max_loop_depth = 0;
  uint64_t total_weight = 0;

  {
    ciBytecodeStream bcs(method);
    Bytecodes::Code bc;
    while ((bc = bcs.next()) != ciBytecodeStream::EOBC()) {
      const int cur = bcs.cur_bci();
      uint32_t active_depth = 0;
      for (int i = 0; i < loop_count; ++i) {
        if (cur >= loops[i].start_bci && cur <= loops[i].end_bci) {
          ++active_depth;
        }
      }
      if (active_depth > max_loop_depth) {
        max_loop_depth = active_depth;
      }

      uint32_t op_base = 1;
      if (bc >= Bytecodes::_fadd && bc <= Bytecodes::_drem) {
        // Floating-point arithmetic (prime AVX-512 / SVE2 / RVV FMA candidates)
        op_base = 8;
      } else if ((bc >= Bytecodes::_iadd && bc <= Bytecodes::_lxor) || bc == Bytecodes::_iinc) {
        // Integer / bitwise ALU operations
        op_base = 3;
      } else if (bc == Bytecodes::_faload || bc == Bytecodes::_daload ||
                 bc == Bytecodes::_fastore || bc == Bytecodes::_dastore) {
        // Contiguous FP array stream operations
        op_base = 10;
      } else if ((bc >= Bytecodes::_iaload && bc <= Bytecodes::_saload) ||
                 (bc >= Bytecodes::_iastore && bc <= Bytecodes::_sastore)) {
        // General array load/store operations
        op_base = 6;
      } else if (bc == Bytecodes::_invokestatic ||
                 bc == Bytecodes::_invokespecial ||
                 bc == Bytecodes::_invokevirtual) {
        // Static / intrinsic / vector kernel call site
        op_base = 12;
      }

      const uint32_t depth_multiplier = 1u + (active_depth * 4u);
      total_weight += static_cast<uint64_t>(op_base) * static_cast<uint64_t>(depth_multiplier);
    }
  }

  // Incorporate HIR linear-scan block loop depth if C1 HIR computed deeper natural loops.
  if (comp->hir() != nullptr && comp->hir()->linear_scan_order() != nullptr) {
    BlockList* blocks = comp->hir()->linear_scan_order();
    for (int i = 0; i < blocks->length(); ++i) {
      BlockBegin* blk = blocks->at(i);
      if (blk != nullptr && blk->loop_depth() > static_cast<int>(max_loop_depth)) {
        max_loop_depth = static_cast<uint32_t>(blk->loop_depth());
      }
    }
  }

  if (method->redlet_batch_exec()) {
    total_weight = (total_weight * 3ULL) / 2ULL + 64ULL;
  }
  if (method->redlet_nested_exec() && max_loop_depth < 2u) {
    max_loop_depth = 2u;
  }

  const uint32_t clamped_weight = (total_weight > 0xFFFFFFFFULL)
                                  ? 0xFFFFFFFFu
                                  : static_cast<uint32_t>(total_weight);
  const bool bypass_warmup = method->redlet_bypass_warmup();

  redlet_rt_c1_report_method_heat(static_cast<void*>(method->get_Method()),
                                  max_loop_depth,
                                  clamped_weight,
                                  bypass_warmup);
#else
  (void)comp;
  (void)gen;
#endif
}
