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

#include "gc/redjet/runtime/redjetBarrierSetVAC.hpp"
#include "gc/redjet/api/redjetTailTraces.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetEmptyBox.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/shared/gc_globals.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "vac/c1_Compilation.hpp"
#include "vac/c1_IR.hpp"
#include "vac/c1_LIRGenerator.hpp"
#include "vac/c1_LIRAssembler.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciStreams.hpp"
#include "interpreter/bytecodes.hpp"
#include "runtime/javaThread.hpp"
#endif

extern "C" JNIEXPORT void redjet_offheap_evict(void* handle);

LIR_Opr RedjetBarrierSetVAC::resolve_address(LIRAccess& access, bool resolve_in_register) {
  return BarrierSetC1::resolve_address(access, resolve_in_register);
}

void RedjetBarrierSetVAC::load_at_resolved(LIRAccess& access, LIR_Opr result) {
  BarrierSetC1::load_at_resolved(access, result);
}

void RedjetBarrierSetVAC::store_at_resolved(LIRAccess& access, LIR_Opr value) {
#ifndef REDJET_STANDALONE_BUILD
  if (UseRedjetGC && access.is_oop()) {
    // Inject DeadRefSignal clasp check: if destination slot holds an active OOP,
    // emit reference harbinger write to RPSS ring buffer prior to overwriting.
    emit_dead_ref_signal_clasp(access, value);
  }
#endif
  BarrierSetC1::store_at_resolved(access, value);
}

void RedjetBarrierSetVAC::inject_dockpoint_entry(Compilation* comp, LIRGenerator* gen) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || comp == nullptr || gen == nullptr) {
    return;
  }
  ciMethod* method = comp->method();
  if (method == nullptr || !method->is_loaded()) {
    return;
  }
  // Record dockpoint method entry trigger for zero-STW coordination and report method heat
  redjet_rt_c1_report_method_heat(static_cast<void*>(method->get_Method()), 1, 16, method->redjet_bypass_warmup());
#else
  (void)comp;
  (void)gen;
#endif
}

void RedjetBarrierSetVAC::inject_parkpoint_poll(Compilation* comp, LIRGenerator* gen) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || comp == nullptr || gen == nullptr) {
    return;
  }
  // Parkpoint 0.5-cycle L1 D-Cache thread sync check at loop bottom
  if (comp->method() != nullptr && should_suppress_loop_safepoint(comp->method())) {
    // Suppress heavy safepoints in favor of lean parkpoint poll
    volatile uint32_t* gate = RedjetPointCoordinator::pausepoint_gate_address();
    if (gate != nullptr && *gate != 0) {
      redjet_rt_parkpoint_poll_slow(JavaThread::current(), nullptr);
    }
  }
#else
  (void)comp;
  (void)gen;
#endif
}

bool RedjetBarrierSetVAC::should_suppress_loop_safepoint(ciMethod* method) {
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

void RedjetBarrierSetVAC::emit_dead_ref_signal(LIRGenerator* gen, LIR_Opr rpss_addr, LIR_Opr dead_oop) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || gen == nullptr) {
    return;
  }
  if (dead_oop.is_valid()) {
    uint32_t tid = 0;
    JavaThread* jt = JavaThread::current_or_null();
    if (jt != nullptr) {
      tid = static_cast<uint32_t>(jt->osthread() != nullptr ? jt->osthread()->thread_id() : 0);
    }
    void* oop_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(dead_oop._val));
    if (oop_ptr != nullptr) {
      g_redjet_tail_traces.emit_dead_reference_signal(tid, oop_ptr, 0);
    }
  }
#else
  (void)gen;
  (void)rpss_addr;
  (void)dead_oop;
#endif
}

void RedjetBarrierSetVAC::emit_dead_ref_signal_clasp(LIRAccess& access, LIR_Opr prev_value) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC) {
    return;
  }
  if (prev_value.is_valid()) {
    uint32_t tid = 0;
    JavaThread* jt = JavaThread::current_or_null();
    if (jt != nullptr) {
      tid = static_cast<uint32_t>(jt->osthread() != nullptr ? jt->osthread()->thread_id() : 0);
    }
    void* oop_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(prev_value._val));
    if (oop_ptr != nullptr) {
      g_redjet_tail_traces.emit_dead_reference_signal(tid, oop_ptr, 0);
    }
  }
#else
  (void)access;
  (void)prev_value;
#endif
}

void RedjetBarrierSetVAC::emit_offheap_evict_trap(LIRGenerator* gen, LIR_Opr handle_ptr) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || gen == nullptr || !handle_ptr.is_valid()) {
    return;
  }
  void* raw_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(handle_ptr._val));
  if (raw_handle != nullptr) {
    redjet_offheap_evict(raw_handle);
  }
#else
  (void)gen;
  (void)handle_ptr;
#endif
}

void RedjetBarrierSetVAC::emit_emptybox_guard(LIRGenerator* gen, LIR_Opr slot_addr) {
#ifndef REDJET_STANDALONE_BUILD
  if (!UseRedjetGC || gen == nullptr || !slot_addr.is_valid()) {
    return;
  }
  if (slot_addr.is_address() && slot_addr._val != 0) {
    volatile uint32_t* slot_hdr = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(slot_addr._val));
    if (slot_hdr != nullptr && *slot_hdr == RedjetEmptyBox::HS_EMPTYBOX_MAGIC) {
      RedjetEmptyBoxManager::get_telemetry();
    }
  }
#else
  (void)gen;
  (void)slot_addr;
#endif
}

bool RedjetBarrierSetVAC::is_vac_accelerated_method(ciMethod* method) {
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

void RedjetBarrierSetVAC::report_iterative_bytecode_lir_heat(Compilation* comp, LIRGenerator* gen) {
#ifndef REDJET_STANDALONE_BUILD
  (void)gen;
  if (!UseRedjetGC || comp == nullptr) {
    return;
  }
  ciMethod* method = comp->method();
  if (method == nullptr || !method->is_loaded() || method->code_size() <= 0) {
    return;
  }

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
        op_base = 8;
      } else if ((bc >= Bytecodes::_iadd && bc <= Bytecodes::_lxor) || bc == Bytecodes::_iinc) {
        op_base = 3;
      } else if (bc == Bytecodes::_faload || bc == Bytecodes::_daload ||
                 bc == Bytecodes::_fastore || bc == Bytecodes::_dastore) {
        op_base = 10;
      } else if ((bc >= Bytecodes::_iaload && bc <= Bytecodes::_saload) ||
                 (bc >= Bytecodes::_iastore && bc <= Bytecodes::_sastore)) {
        op_base = 6;
      } else if (bc == Bytecodes::_invokestatic ||
                 bc == Bytecodes::_invokespecial ||
                 bc == Bytecodes::_invokevirtual) {
        op_base = 12;
      }

      const uint32_t depth_multiplier = 1u + (active_depth * 4u);
      total_weight += static_cast<uint64_t>(op_base) * static_cast<uint64_t>(depth_multiplier);
    }
  }

  if (comp->hir() != nullptr && comp->hir()->linear_scan_order() != nullptr) {
    BlockList* blocks = comp->hir()->linear_scan_order();
    for (int i = 0; i < blocks->length(); ++i) {
      BlockBegin* blk = blocks->at(i);
      if (blk != nullptr && blk->loop_depth() > static_cast<int>(max_loop_depth)) {
        max_loop_depth = static_cast<uint32_t>(blk->loop_depth());
      }
    }
  }

  if (method->redjet_batch_exec()) {
    total_weight = (total_weight * 3ULL) / 2ULL + 64ULL;
  }
  if (method->redjet_nested_exec() && max_loop_depth < 2u) {
    max_loop_depth = 2u;
  }

  const uint32_t clamped_weight = (total_weight > 0xFFFFFFFFULL)
                                  ? 0xFFFFFFFFu
                                  : static_cast<uint32_t>(total_weight);
  const bool bypass_warmup = method->redjet_bypass_warmup();

  redjet_rt_c1_report_method_heat(static_cast<void*>(method->get_Method()),
                                  max_loop_depth,
                                  clamped_weight,
                                  bypass_warmup);
#else
  (void)comp;
  (void)gen;
#endif
}
