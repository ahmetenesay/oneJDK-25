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

#include "runtime/pointCoordinator.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/threads.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "logging/log.hpp"

#if INCLUDE_REDJETGC
#include "gc/shared/gc_globals.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

alignas(64) std::atomic<uint8_t>  PointCoordinator::_global_park_armed{ 0 };
alignas(64) std::atomic<uint8_t>  PointCoordinator::_global_dock_armed{ 0 };
std::atomic<uint64_t>             PointCoordinator::_total_park_polls{ 0 };
std::atomic<uint64_t>             PointCoordinator::_total_dock_checks{ 0 };
std::atomic<uint64_t>             PointCoordinator::_total_jni_transitions{ 0 };
std::atomic<uint64_t>             PointCoordinator::_total_slot16_hotswaps{ 0 };
std::atomic<uint64_t>             PointCoordinator::_total_park_synchronizations{ 0 };
std::atomic<bool>                 PointCoordinator::_initialized{ false };

void PointCoordinator::initialize() {
  if (_initialized.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  _global_park_armed.store(0, std::memory_order_relaxed);
  _global_dock_armed.store(0, std::memory_order_relaxed);
  _total_park_polls.store(0, std::memory_order_relaxed);
  _total_dock_checks.store(0, std::memory_order_relaxed);
  _total_jni_transitions.store(0, std::memory_order_relaxed);
  _total_slot16_hotswaps.store(0, std::memory_order_relaxed);
  _total_park_synchronizations.store(0, std::memory_order_relaxed);

#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::initialize();
  }
#endif

  log_info(gc, init)("PointCoordinator (Tier 1 Universal Runtime Arbiter) initialized with L1-D poll flags");
}

void PointCoordinator::shutdown() {
  if (!_initialized.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  disarm_park_all();
  disarm_dock_all();

#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::shutdown();
  }
#endif
}

bool PointCoordinator::is_park_armed(JavaThread* thread) {
  _total_park_polls.fetch_add(1, std::memory_order_relaxed);
  if (thread != nullptr) {
    return thread->park_flag() != 0 || _global_park_armed.load(std::memory_order_relaxed) != 0;
  }
  return _global_park_armed.load(std::memory_order_relaxed) != 0;
}

bool PointCoordinator::is_dock_armed(JavaThread* thread) {
  _total_dock_checks.fetch_add(1, std::memory_order_relaxed);
  if (thread != nullptr) {
    return thread->dock_flag() != 0 || _global_dock_armed.load(std::memory_order_relaxed) != 0;
  }
  return _global_dock_armed.load(std::memory_order_relaxed) != 0;
}

void PointCoordinator::arm_park(JavaThread* thread) {
  if (thread != nullptr) {
    thread->set_park_flag(1);
    OrderAccess::storestore();
  }
}

void PointCoordinator::disarm_park(JavaThread* thread) {
  if (thread != nullptr) {
    thread->set_park_flag(0);
    OrderAccess::storestore();
  }
}

void PointCoordinator::arm_dock(JavaThread* thread) {
  if (thread != nullptr) {
    thread->set_dock_flag(1);
    OrderAccess::storestore();
  }
}

void PointCoordinator::disarm_dock(JavaThread* thread) {
  if (thread != nullptr) {
    thread->set_dock_flag(0);
    OrderAccess::storestore();
  }
}

void PointCoordinator::arm_park_all() {
  _global_park_armed.store(1, std::memory_order_release);
  JavaThreadIteratorWithHandle jtiwh;
  while (JavaThread* jt = jtiwh.next()) {
    jt->set_park_flag(1);
  }
  OrderAccess::fence();
}

void PointCoordinator::disarm_park_all() {
  _global_park_armed.store(0, std::memory_order_release);
  JavaThreadIteratorWithHandle jtiwh;
  while (JavaThread* jt = jtiwh.next()) {
    jt->set_park_flag(0);
  }
  OrderAccess::fence();
}

void PointCoordinator::arm_dock_all() {
  _global_dock_armed.store(1, std::memory_order_release);
  JavaThreadIteratorWithHandle jtiwh;
  while (JavaThread* jt = jtiwh.next()) {
    jt->set_dock_flag(1);
  }
  OrderAccess::fence();
}

void PointCoordinator::disarm_dock_all() {
  _global_dock_armed.store(0, std::memory_order_release);
  JavaThreadIteratorWithHandle jtiwh;
  while (JavaThread* jt = jtiwh.next()) {
    jt->set_dock_flag(0);
  }
  OrderAccess::fence();
}

void PointCoordinator::on_jni_park_enter(JavaThread* thread) {
  _total_jni_transitions.fetch_add(1, std::memory_order_relaxed);
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::on_jni_park_enter(thread);
    return;
  }
#endif
  if (thread != nullptr) {
    thread->set_park_flag(1);
    OrderAccess::storestore();
  }
}

void PointCoordinator::on_jni_park_exit(JavaThread* thread) {
  _total_jni_transitions.fetch_add(1, std::memory_order_relaxed);
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::on_jni_park_exit(thread);
    return;
  }
#endif
  if (thread != nullptr) {
    thread->set_park_flag(0);
    OrderAccess::storestore();
  }
}

void PointCoordinator::on_park_point_hit(JavaThread* thread, ParkReason reason) {
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::on_park_point_hit(thread, static_cast<int>(reason));
    return;
  }
#endif
  if (thread != nullptr && thread->park_flag() != 0) {
    thread->set_park_flag(0);
    OrderAccess::fence();
  }
}

void PointCoordinator::on_dock_point_hit(JavaThread* thread, DockReason reason) {
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::on_dock_point_hit(thread, static_cast<int>(reason));
    return;
  }
#endif
  if (thread != nullptr && thread->dock_flag() != 0) {
    thread->set_dock_flag(0);
    OrderAccess::fence();
  }
}

void PointCoordinator::synchronize_park() {
  _total_park_synchronizations.fetch_add(1, std::memory_order_relaxed);
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    RedjetPointCoordinator::sync_point();
    return;
  }
#endif
  hardware_fence();
}

void PointCoordinator::desynchronize_park() {
  disarm_park_all();
}

void PointCoordinator::hardware_fence() {
  OrderAccess::fence();
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  _mm_mfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
  __dmb(_ARM64_BARRIER_SY);
#else
  __asm__ __volatile__("dmb sy" ::: "memory");
#endif
#elif defined(__riscv)
  __asm__ __volatile__("fence rw,rw" ::: "memory");
#endif
}

bool PointCoordinator::coordinate_slot16_hotswap(void* slot16_addr, void* new_target_slot) {
  _total_slot16_hotswaps.fetch_add(1, std::memory_order_relaxed);
#if INCLUDE_REDJETGC
  if (UseRedjetGC) {
    return RedjetPointCoordinator::coordinate_slot16_hotswap(slot16_addr, new_target_slot);
  }
#endif
  if (slot16_addr == nullptr || new_target_slot == nullptr) return false;

  int32_t rel_offset = static_cast<int32_t>(
      reinterpret_cast<intptr_t>(new_target_slot) - (reinterpret_cast<intptr_t>(slot16_addr) + 5));
  uint64_t patch = 0;
  uint8_t* p = reinterpret_cast<uint8_t*>(&patch);
  p[0] = 0xE9; // JMP rel32 opcode
  std::memcpy(&p[1], &rel_offset, 4);
  p[5] = 0x90; // NOP
  p[6] = 0x90; // NOP
  p[7] = 0x90; // NOP

  Atomic::store(reinterpret_cast<volatile uint64_t*>(slot16_addr), patch);
  hardware_fence();
  return true;
}

void PointCoordinator::print_on(outputStream* st) {
  st->print_cr("PointCoordinator Status:");
  st->print_cr("  Initialized: %s", _initialized.load(std::memory_order_relaxed) ? "true" : "false");
  st->print_cr("  Global Park Armed: %u", _global_park_armed.load(std::memory_order_relaxed));
  st->print_cr("  Global Dock Armed: %u", _global_dock_armed.load(std::memory_order_relaxed));
  st->print_cr("  Total Park Polls: " UINT64_FORMAT, total_park_polls());
  st->print_cr("  Total Dock Checks: " UINT64_FORMAT, total_dock_checks());
  st->print_cr("  Total JNI Transitions: " UINT64_FORMAT, total_jni_transitions());
  st->print_cr("  Total Slot16 Hotswaps: " UINT64_FORMAT, total_slot16_hotswaps());
  st->print_cr("  Total Park Synchronizations: " UINT64_FORMAT, total_park_synchronizations());
}
