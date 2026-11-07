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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETBARRIERSETVAC_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETBARRIERSETVAC_HPP

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/c1/barrierSetC1.hpp"
#else
#include <cstddef>
#include <cstdint>

class LIRAccess;
class LIR_Opr {
public:
  int _val{ 0 };
  bool is_valid() const { return _val != 0; }
  bool is_oop() const { return false; }
};

class BarrierSetC1 {
public:
  virtual ~BarrierSetC1() = default;
  virtual LIR_Opr resolve_address(LIRAccess& access, bool resolve_in_register) { return LIR_Opr(); }
  virtual void load_at_resolved(LIRAccess& access, LIR_Opr result) {}
  virtual void store_at_resolved(LIRAccess& access, LIR_Opr value) {}
};
#endif

class ciMethod;
class Compilation;
class LIRGenerator;

// ============================================================================
// RedjetBarrierSetVAC: Canonical Vectorized Ahead-of-time Compiler (VAC)
// Tier-1 Barrier Implementation for RedjetGC.
// Injects:
// 1. 2-Point dockpoint entry trampolines and loop bottom parkpoint polls.
// 2. DeadRefSignal clasp writes to the RPSS ring buffer on reference stores.
// 3. OffHeapEvict immediate RAII eviction traps for marked off-heap handles.
// 4. EmptyBox hardware guards testing slot headers against HS_EMPTYBOX_MAGIC.
// ============================================================================
class RedjetBarrierSetVAC : public BarrierSetC1 {
protected:
  LIR_Opr resolve_address(LIRAccess& access, bool resolve_in_register) override;

public:
  RedjetBarrierSetVAC() = default;
  virtual ~RedjetBarrierSetVAC() = default;

  void load_at_resolved(LIRAccess& access, LIR_Opr result) override;
  void store_at_resolved(LIRAccess& access, LIR_Opr value) override;

  // 2-Point Synchronization Injection
  static void inject_dockpoint_entry(Compilation* comp, LIRGenerator* gen);
  static void inject_parkpoint_poll(Compilation* comp, LIRGenerator* gen);
  static bool should_suppress_loop_safepoint(ciMethod* method);

  // DeadRefSignal Clasp Emission to RPSS Ring Buffer
  static void emit_dead_ref_signal(LIRGenerator* gen, LIR_Opr rpss_addr, LIR_Opr dead_oop);
  static void emit_dead_ref_signal_clasp(LIRAccess& access, LIR_Opr prev_value);

  // Deterministic Off-Heap Immediate Eviction Traps
  static void emit_offheap_evict_trap(LIRGenerator* gen, LIR_Opr handle_ptr);

  // EmptyBox Hardware Guard (Slot Header 0xEB0C5107 Protection)
  static void emit_emptybox_guard(LIRGenerator* gen, LIR_Opr slot_addr);

  // VAC Heat Telemetry and Kernel Acceleration
  static void report_iterative_bytecode_lir_heat(Compilation* comp, LIRGenerator* gen);
  static bool is_vac_accelerated_method(ciMethod* method);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETBARRIERSETVAC_HPP
