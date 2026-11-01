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

#ifndef SHARE_REDJET_NESTED_REDJETNESTEDEXEC_HPP
#define SHARE_REDJET_NESTED_REDJETNESTEDEXEC_HPP

/*
 * Architectural Facade for Nested Cooperative Execution & 4-Points Coordination
 * (Safepoint / Controlpoint / Hookpoint / Pausepoint + Nested Batch Stack).
 * Strictly C++14-safe for inclusion across standard HotSpot translation units.
 */

#include "gc/redjet/rjspcf/redjetBatchScope.hpp"
#include "gc/redjet/rjnrml/redjetPointCoordinator.hpp"

class Thread;

#ifdef REDJET_STANDALONE_BUILD
class RedjetNestedExec {
#else
class RedjetNestedExec : public AllStatic {
#endif
public:
  static inline bool is_nested_scope_active() {
    return RedjetBatchScopeManager::is_nested_active();
  }

  static inline uint32_t nesting_depth() {
    return RedjetBatchScopeManager::current_depth();
  }

  static inline uint64_t enter_nested_batch(uint32_t priority,
                                            bool bypass_warmup = false,
                                            bool allow_deeper_nesting = true) {
    return RedjetBatchScopeManager::enter_batch(priority, bypass_warmup, allow_deeper_nesting);
  }

  static inline void exit_nested_batch(uint64_t handle) {
    RedjetBatchScopeManager::exit_batch(handle);
  }

  static inline void request_slot_pause(void* target_slot) {
    RedjetPointCoordinator::request_object_pause(target_slot);
  }

  static inline void release_slot_pause(void* target_slot) {
    RedjetPointCoordinator::release_object_pause(target_slot);
  }

  static inline void poll_pausepoint(Thread* thread, void* target_slot = NULL) {
    RedjetPointCoordinator::pausepoint_poll(thread, target_slot);
  }

  static inline void check_controlpoint(Thread* thread, size_t alloc_bytes, void* cold_target_hdr = NULL) {
    (void)RedjetPointCoordinator::controlpoint_check(thread, alloc_bytes, cold_target_hdr);
  }
};

#ifdef REDJET_STANDALONE_BUILD
class RedjetNestedSlotPauseGuard {
#else
class RedjetNestedSlotPauseGuard : public StackObj {
#endif
private:
  void* _slot;
  bool  _active;

  RedjetNestedSlotPauseGuard(const RedjetNestedSlotPauseGuard&);
  RedjetNestedSlotPauseGuard& operator=(const RedjetNestedSlotPauseGuard&);

public:
  explicit RedjetNestedSlotPauseGuard(void* target_slot)
    : _slot(target_slot), _active(target_slot != NULL) {
    if (_active) {
      RedjetPointCoordinator::request_object_pause(_slot);
    }
  }

  ~RedjetNestedSlotPauseGuard() {
    release();
  }

  void release() {
    if (_active && _slot != NULL) {
      RedjetPointCoordinator::release_object_pause(_slot);
      _active = false;
    }
  }
};

#endif // SHARE_REDJET_NESTED_REDJETNESTEDEXEC_HPP
