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

#ifndef SHARE_REDJET_BATCH_REDJETBATCHEXEC_HPP
#define SHARE_REDJET_BATCH_REDJETBATCHEXEC_HPP

/*
 * Architectural Facade for Redjet 16-Deep BatchScope & GroupScope Execution.
 * Strictly C++14-safe for inclusion across HotSpot C1, C2, interpreter, and runtime.
 */

#include "gc/redjet/rjspcf/redjetBatchScope.hpp"

#ifdef REDJET_STANDALONE_BUILD
class RedjetBatchExec {
#else
class RedjetBatchExec : public AllStatic {
#endif
public:
  typedef RedjetBatchGuard ScopedBatch;
  typedef RedjetGroupGuard ScopedGroup;

  static inline uint64_t begin_batch(uint32_t priority = 5u,
                                     bool bypass_warmup = false,
                                     bool nested_allowed = true) {
    return RedjetBatchScopeManager::enter_batch(priority, bypass_warmup, nested_allowed);
  }

  static inline void end_batch(uint64_t scope_handle) {
    RedjetBatchScopeManager::exit_batch(scope_handle);
  }

  static inline void abort_batch(uint64_t scope_handle) {
    RedjetBatchScopeManager::abort_batch(scope_handle);
  }

  static inline uint64_t begin_group(uint32_t group_hex_id,
                                     uint32_t priority = 5u,
                                     bool bypass_warmup = false) {
    return RedjetBatchScopeManager::enter_group(group_hex_id, priority, bypass_warmup);
  }

  static inline void end_group(uint64_t scope_handle) {
    RedjetBatchScopeManager::exit_group(scope_handle);
  }

  static inline uint32_t depth() {
    return RedjetBatchScopeManager::current_depth();
  }

  static inline uint32_t priority() {
    return RedjetBatchScopeManager::current_priority();
  }

  static inline uint32_t group_hex_id() {
    return RedjetBatchScopeManager::current_group_hex_id();
  }

  static inline uint32_t batch_id() {
    return RedjetBatchScopeManager::current_batch_id();
  }

  static inline bool is_active() {
    return RedjetBatchScopeManager::is_batch_active();
  }

  static inline bool is_bypass_warmup() {
    return RedjetBatchScopeManager::is_bypass_warmup_active();
  }

  static inline void on_object_allocated(size_t bytes, void* live_hdr = NULL) {
    RedjetBatchScopeManager::record_allocation(bytes, live_hdr);
  }

  static inline size_t flush_staged_eggs(void** egg_hdrs, size_t count) {
    return RedjetBatchScopeManager::batch_optimize_egg_objects(egg_hdrs, count);
  }
};

#endif // SHARE_REDJET_BATCH_REDJETBATCHEXEC_HPP
