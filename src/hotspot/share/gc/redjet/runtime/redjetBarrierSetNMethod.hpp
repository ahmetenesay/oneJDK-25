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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETBARRIERSETNMETHOD_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETBARRIERSETNMETHOD_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef RJ_API
#if defined(_WIN32) || defined(__CYGWIN__)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/barrierSetNMethod.hpp"
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#else
class BarrierSetNMethod {};
class nmethod {
public:
  bool is_unloading() const { return false; }
  bool is_alive() const { return true; }
  void unlink_from_method() {}
  void mark_as_maybe_on_stack() {}
  void make_not_entrant() {}
  void** oop_addr_at(int idx) const { return nullptr; }
};
class ByteSize {
  int _v;
public:
  ByteSize(int v = 0) : _v(v) {}
  int value() const { return _v; }
};
typedef void* oop;
typedef uint32_t narrowOop;
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;

// ============================================================================
// RedjetBarrierSetNMethod (PLAN.md Section 8.10.8)
// Standard HotSpot nmethod entry barrier, code cache deoptimization
// invalidation hooks, self-healing nmethod oops, and disarmed guard epoch.
// ============================================================================

struct alignas(64) RedjetNMethodBarrierMetrics {
  std::atomic<uint64_t> barriers_entered{ 0 };
  std::atomic<uint64_t> fast_path_hits{ 0 };
  std::atomic<uint64_t> oops_healed{ 0 };
  std::atomic<uint64_t> methods_unlinked{ 0 };
  std::atomic<uint64_t> deoptimizations{ 0 };
  std::atomic<uint64_t> inline_caches_cleaned{ 0 };
  std::atomic<uint64_t> cycles_spent{ 0 };
};

class RedjetBarrierSetNMethod : public BarrierSetNMethod {
private:
  static alignas(64) int         _disarmed_guard_value;
  static alignas(64) std::atomic<uint32_t> _barrier_epoch;
  static RedjetNMethodBarrierMetrics _metrics;

public:
  RedjetBarrierSetNMethod();
  virtual ~RedjetBarrierSetNMethod() = default;

#ifndef REDJET_STANDALONE_BUILD
  bool nmethod_entry_barrier(nmethod* nm) override;
  ByteSize thread_disarmed_guard_value_offset() const override;
  int* disarmed_guard_value_address() const override;

  oop oop_load_no_keepalive(const nmethod* nm, int index) override;
  oop oop_load_phantom(const nmethod* nm, int index) override;
#endif

  // Generation / Epoch management
  RJ_API static void advance_epoch();
  static int  disarmed_value() noexcept { return _disarmed_guard_value; }
  static uint32_t barrier_epoch() noexcept { return _barrier_epoch.load(std::memory_order_relaxed); }

  // Heal oops embedded in JIT nmethod code
  RJ_API static void heal_nmethod(nmethod* nm);

  // Invalidate and trigger deoptimization for nmethods on code cache updates
  RJ_API static void deoptimize_and_invalidate(nmethod* nm);

  // Clean inline caches for an unlinked compiled method
  RJ_API static void clean_nmethod_inline_caches(nmethod* nm);

  // Arm / Disarm individual nmethods
  RJ_API static void arm_nmethod(nmethod* nm);
  RJ_API static void disarm_nmethod(nmethod* nm);

  // Telemetry queries
  static const RedjetNMethodBarrierMetrics& metrics() noexcept { return _metrics; }
  static uint64_t barriers_entered()   noexcept { return _metrics.barriers_entered.load(std::memory_order_relaxed); }
  static uint64_t fast_path_hits()     noexcept { return _metrics.fast_path_hits.load(std::memory_order_relaxed); }
  static uint64_t oops_healed()        noexcept { return _metrics.oops_healed.load(std::memory_order_relaxed); }
  static uint64_t methods_unlinked()   noexcept { return _metrics.methods_unlinked.load(std::memory_order_relaxed); }
  static uint64_t deoptimizations()    noexcept { return _metrics.deoptimizations.load(std::memory_order_relaxed); }

  RJ_API static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETBARRIERSETNMETHOD_HPP
