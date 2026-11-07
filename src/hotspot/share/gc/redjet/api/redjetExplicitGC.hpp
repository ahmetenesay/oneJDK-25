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

#ifndef SHARE_GC_REDJET_API_REDJETEXPLICITGC_HPP
#define SHARE_GC_REDJET_API_REDJETEXPLICITGC_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/globalDefinitions.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// RedjetExplicitGC: Compiler-Assisted Explicit Clean Hints (PLAN.md Section 8.7)
// Transforms System.gc() and JDK.ThrowGCHint() into zero-STW micro-latency clean
// Controlled by -XX:+RedjetGCInvokesExplicitCleanHints
// ============================================================================

class RedjetExplicitGC {
private:
  static std::atomic<uint64_t> _explicit_hints_invoked;
  static std::atomic<uint64_t> _explicit_hints_preflighted;
  static std::atomic<uint64_t> _bytes_reclaimed_by_hints;

public:
  // Dispatched by JLC/VAC/MIRC compiler when System.gc() is detected ahead of time
  RJ_API static void preflight_gc_hint(uint32_t method_id, uint32_t bci);

  // Runtime invocation of explicit clean hint (Zero-STW)
  RJ_API static void invoke_explicit_clean_hint();

  // Statistics
  RJ_API static uint64_t hints_invoked() { return _explicit_hints_invoked.load(std::memory_order_relaxed); }
  RJ_API static uint64_t hints_preflighted() { return _explicit_hints_preflighted.load(std::memory_order_relaxed); }
  RJ_API static uint64_t bytes_reclaimed() { return _bytes_reclaimed_by_hints.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_API_REDJETEXPLICITGC_HPP
