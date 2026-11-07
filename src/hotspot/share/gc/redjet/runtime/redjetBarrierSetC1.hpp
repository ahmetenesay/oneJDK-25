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

#ifndef SHARE_GC_REDJET_C1_REDJETBARRIERSETC1_HPP
#define SHARE_GC_REDJET_C1_REDJETBARRIERSETC1_HPP

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/c1/barrierSetC1.hpp"
#else
#include <cstddef>
#include <cstdint>

class LIRAccess;
class LIR_Opr {
public:
  int _val{ 0 };
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

// C++14-clean header included by standard HotSpot C1 compiler units.
class RedjetBarrierSetC1 : public BarrierSetC1 {
protected:
  LIR_Opr resolve_address(LIRAccess& access, bool resolve_in_register) override;

public:
  RedjetBarrierSetC1() = default;

  void load_at_resolved(LIRAccess& access, LIR_Opr result) override;
  void store_at_resolved(LIRAccess& access, LIR_Opr value) override;

  // Returns true when RedjetGC is active and the target method is annotated
  // with @RedjetBatchExec, @RedjetNestedExec, or @RedjetBypassWarmup, allowing
  // C1 backward-branch loop safepoints to be suppressed for zero-jitter loops.
  static bool should_suppress_loop_safepoint(ciMethod* method);

  // Computes iterative bytecode/LIR loop depth and arithmetic/SIMD density
  // weights during C1 LIR generation and feeds heat telemetry into RedjetMotherline.
  static void report_iterative_bytecode_lir_heat(Compilation* comp, LIRGenerator* gen);
};

#endif // SHARE_GC_REDJET_C1_REDJETBARRIERSETC1_HPP