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

#ifndef SHARE_GC_REDLET_REDLETBATCHSCOPE_HPP
#define SHARE_GC_REDLET_REDLETBATCHSCOPE_HPP

/*
 * Strictly C++14-compatible header so standard HotSpot translation units
 * (C1, C2, interpreter, runtime, classFileParser, nativeLookup) can include
 * this header cleanly alongside precompiled.hpp and forbiddenFunctions.hpp.
 */

#ifdef REDLET_STANDALONE_BUILD
#include <stddef.h>
#include <stdint.h>
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include <stddef.h>
#include <stdint.h>
#endif

static const uint32_t REDLET_MAX_BATCH_DEPTH = 16u;
static const uint32_t REDLET_MAX_STAGED_EGGS = 32u;

struct RedletBatchFrame {
  uint32_t batch_id;
  uint32_t group_hex_id;
  uint32_t priority;
  bool     bypass_warmup;
  bool     nested_allowed;
  bool     is_group_scope;
  bool     aborted;
  size_t   allocated_bytes;
  uint32_t egg_count;
  uint64_t enter_cycle;
  void*    staged_eggs[32];
};

#ifdef REDLET_STANDALONE_BUILD
class RedletBatchScopeManager {
#else
class RedletBatchScopeManager : public AllStatic {
#endif
public:
  static uint64_t enter_batch(uint32_t priority, bool bypass_warmup, bool nested_allowed = true);
  static void     exit_batch(uint64_t scope_handle);
  static void     abort_batch(uint64_t scope_handle);

  static uint64_t enter_group(uint32_t group_hex_id, uint32_t priority, bool bypass_warmup);
  static void     exit_group(uint64_t scope_handle);

  static uint32_t current_depth();
  static uint32_t current_priority();
  static uint32_t current_group_hex_id();
  static uint32_t current_batch_id();

  static bool     is_batch_active();
  static bool     is_bypass_warmup_active();
  static bool     is_nested_active();

  static const RedletBatchFrame* current_frame();

  static void     record_allocation(size_t bytes, void* live_hdr = NULL);
  static size_t   batch_optimize_egg_objects(void** egg_hdrs, size_t count);
};

#ifdef REDLET_STANDALONE_BUILD
class RedletBatchGuard {
#else
class RedletBatchGuard : public StackObj {
#endif
private:
  uint64_t _handle;
  bool     _closed;

  RedletBatchGuard(const RedletBatchGuard&);
  RedletBatchGuard& operator=(const RedletBatchGuard&);

public:
  explicit RedletBatchGuard(uint32_t priority = 5u,
                            bool bypass_warmup = false,
                            bool nested_allowed = true)
    : _handle(RedletBatchScopeManager::enter_batch(priority, bypass_warmup, nested_allowed)),
      _closed(false) {}

  ~RedletBatchGuard() {
    if (!_closed && _handle != 0ULL) {
      RedletBatchScopeManager::exit_batch(_handle);
      _closed = true;
    }
  }

  void commit() {
    if (!_closed && _handle != 0ULL) {
      RedletBatchScopeManager::exit_batch(_handle);
      _closed = true;
    }
  }

  void abort() {
    if (!_closed && _handle != 0ULL) {
      RedletBatchScopeManager::abort_batch(_handle);
      _closed = true;
    }
  }

  uint64_t handle() const { return _handle; }
  uint32_t batch_id() const { return static_cast<uint32_t>(_handle & 0xFFFFFFFFULL); }
};

#ifdef REDLET_STANDALONE_BUILD
class RedletGroupGuard {
#else
class RedletGroupGuard : public StackObj {
#endif
private:
  uint64_t _handle;
  bool     _closed;

  RedletGroupGuard(const RedletGroupGuard&);
  RedletGroupGuard& operator=(const RedletGroupGuard&);

public:
  RedletGroupGuard(uint32_t group_hex_id, uint32_t priority = 5u, bool bypass_warmup = false)
    : _handle(RedletBatchScopeManager::enter_group(group_hex_id, priority, bypass_warmup)),
      _closed(false) {}

  ~RedletGroupGuard() {
    if (!_closed && _handle != 0ULL) {
      RedletBatchScopeManager::exit_group(_handle);
      _closed = true;
    }
  }

  void exit() {
    if (!_closed && _handle != 0ULL) {
      RedletBatchScopeManager::exit_group(_handle);
      _closed = true;
    }
  }

  uint64_t handle() const { return _handle; }
  uint32_t group_hex_id() const { return static_cast<uint32_t>(_handle & 0xFFFFFFFFULL); }
};

#ifdef __cplusplus
extern "C" {
#endif

uint64_t redlet_rt_batch_enter(uint32_t priority, bool bypass_warmup);
void     redlet_rt_batch_exit(uint64_t handle);
void     redlet_rt_batch_abort(uint64_t handle);
uint64_t redlet_rt_group_enter(uint32_t group_hex_id, uint32_t priority, bool bypass_warmup);
void     redlet_rt_group_exit(uint64_t handle);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETBATCHSCOPE_HPP
