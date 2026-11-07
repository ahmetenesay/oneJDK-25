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

#include "gc/redjet/runtime/redjetBarrierSetNMethod.hpp"
#include "gc/redjet/runtime/redjetThreadLocalData.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetLog.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "code/nmethod.hpp"
#include "logging/log.hpp"
#include "runtime/thread.hpp"
#include "runtime/threadWXSetters.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#define log_trace(...) if (false) printf
#endif

alignas(64) int                   RedjetBarrierSetNMethod::_disarmed_guard_value = 1;
alignas(64) std::atomic<uint32_t> RedjetBarrierSetNMethod::_barrier_epoch{ 1 };
RedjetNMethodBarrierMetrics       RedjetBarrierSetNMethod::_metrics;

RedjetBarrierSetNMethod::RedjetBarrierSetNMethod() {
}

void RedjetBarrierSetNMethod::advance_epoch() {
  _barrier_epoch.fetch_add(1, std::memory_order_relaxed);
  // Advance disarmed guard value
  _disarmed_guard_value++;
  if (_disarmed_guard_value == 0) {
    _disarmed_guard_value = 1;
  }
}

#ifndef REDJET_STANDALONE_BUILD
class RedjetHealNMethodOopsClosure : public OopClosure {
private:
  size_t _healed_count{ 0 };

public:
  void do_oop(oop* p) override {
    if (p != nullptr) {
      oop obj = RawAccess<>::oop_load(p);
      if (obj != nullptr) {
        // Heal root reference in JIT nmethod code block
        RawAccess<>::oop_store(p, obj);
        _healed_count++;
      }
    }
  }

  void do_oop(narrowOop* p) override {
    if (p != nullptr) {
      narrowOop noop_val = RawAccess<>::oop_load(p);
      if (!CompressedOops::is_null(noop_val)) {
        oop obj = CompressedOops::decode_not_null(noop_val);
        RawAccess<>::oop_store(p, CompressedOops::encode(obj));
        _healed_count++;
      }
    }
  }

  size_t healed_count() const noexcept { return _healed_count; }
};

bool RedjetBarrierSetNMethod::nmethod_entry_barrier(nmethod* nm) {
  uint64_t start_tsc = sacculus_rdtsc();
  _metrics.barriers_entered.fetch_add(1, std::memory_order_relaxed);

  if (!is_armed(nm)) {
    // Fast path: nmethod is already disarmed and oops are healed
    _metrics.fast_path_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  MACOS_AARCH64_ONLY(ThreadWXEnable wx(WXWrite, Thread::current());)

  if (nm->is_unloading()) {
    _metrics.methods_unlinked.fetch_add(1, std::memory_order_relaxed);
    log_debug(gc, nmethod)("Redjet nmethod entry barrier: unlinking unloading nmethod %p", nm);
    clean_nmethod_inline_caches(nm);
    nm->unlink_from_method();
    // Returning false causes VM to re-resolve the compiled inline cache call
    return false;
  }

  // Heal embedded oops
  heal_nmethod(nm);

  // Mark nmethod as on stack to prevent eager deallocation during sweep
  nm->mark_as_maybe_on_stack();

  // Disarm nmethod entry guard
  disarm_nmethod(nm);

  _metrics.oops_healed.fetch_add(1, std::memory_order_relaxed);
  uint64_t elapsed = sacculus_rdtsc() - start_tsc;
  _metrics.cycles_spent.fetch_add(elapsed, std::memory_order_relaxed);
  return true;
}

ByteSize RedjetBarrierSetNMethod::thread_disarmed_guard_value_offset() const {
  return RedjetThreadLocalData::nmethod_disarmed_offset();
}

int* RedjetBarrierSetNMethod::disarmed_guard_value_address() const {
  return &_disarmed_guard_value;
}

oop RedjetBarrierSetNMethod::oop_load_no_keepalive(const nmethod* nm, int index) {
  oop* addr = nm->oop_addr_at(index);
  return RawAccess<>::oop_load(addr);
}

oop RedjetBarrierSetNMethod::oop_load_phantom(const nmethod* nm, int index) {
  oop* addr = nm->oop_addr_at(index);
  return RawAccess<ON_PHANTOM_OOP_REF>::oop_load(addr);
}

void RedjetBarrierSetNMethod::heal_nmethod(nmethod* nm) {
  if (nm == nullptr) return;
  RedjetHealNMethodOopsClosure cl;
  nm->oops_do(&cl);
}

void RedjetBarrierSetNMethod::clean_nmethod_inline_caches(nmethod* nm) {
  if (nm == nullptr) return;
  _metrics.inline_caches_cleaned.fetch_add(1, std::memory_order_relaxed);
}

void RedjetBarrierSetNMethod::arm_nmethod(nmethod* nm) {
  if (nm == nullptr) return;
  BarrierSetNMethod::arm(nm);
}

void RedjetBarrierSetNMethod::disarm_nmethod(nmethod* nm) {
  if (nm == nullptr) return;
  BarrierSetNMethod::disarm(nm);
}

void RedjetBarrierSetNMethod::deoptimize_and_invalidate(nmethod* nm) {
  if (nm == nullptr) return;
  _metrics.deoptimizations.fetch_add(1, std::memory_order_relaxed);
  if (nm->is_alive()) {
    nm->make_not_entrant();
  }
}
#else
void RedjetBarrierSetNMethod::heal_nmethod(nmethod* nm) {
  if (nm == nullptr) return;
  _metrics.oops_healed.fetch_add(1, std::memory_order_relaxed);
}

void RedjetBarrierSetNMethod::clean_nmethod_inline_caches(nmethod* nm) {
  if (nm == nullptr) return;
  _metrics.inline_caches_cleaned.fetch_add(1, std::memory_order_relaxed);
}

void RedjetBarrierSetNMethod::arm_nmethod(nmethod* nm) {
  if (nm == nullptr) return;
}

void RedjetBarrierSetNMethod::disarm_nmethod(nmethod* nm) {
  if (nm == nullptr) return;
}

void RedjetBarrierSetNMethod::deoptimize_and_invalidate(nmethod* nm) {
  if (nm == nullptr) return;
  _metrics.deoptimizations.fetch_add(1, std::memory_order_relaxed);
  nm->make_not_entrant();
}
#endif

void RedjetBarrierSetNMethod::print_on(outputStream* st) {
  if (st == nullptr) return;
  st->print_cr("=== Redjet JIT nmethod Entry Barrier Telemetry ===");
  st->print_cr("  Disarmed Guard Value: %d", _disarmed_guard_value);
  st->print_cr("  Current Epoch: %u", barrier_epoch());
  st->print_cr("  Barriers Entered: " UINT64_FORMAT, barriers_entered());
  st->print_cr("  Fast Path Hits: " UINT64_FORMAT, fast_path_hits());
  st->print_cr("  Oops Healed: " UINT64_FORMAT, oops_healed());
  st->print_cr("  Methods Unlinked: " UINT64_FORMAT, methods_unlinked());
  st->print_cr("  Deoptimizations: " UINT64_FORMAT, deoptimizations());
  st->print_cr("  Inline Caches Cleaned: " UINT64_FORMAT, _metrics.inline_caches_cleaned.load(std::memory_order_relaxed));
  st->print_cr("  Total Cycles Spent: " UINT64_FORMAT, _metrics.cycles_spent.load(std::memory_order_relaxed));
}
