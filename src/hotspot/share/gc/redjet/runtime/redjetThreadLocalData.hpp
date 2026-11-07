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

#ifndef SHARE_GC_REDJET_REDJETTHREADLOCALDATA_HPP
#define SHARE_GC_REDJET_REDJETTHREADLOCALDATA_HPP

#include "gc/shared/gcThreadLocalData.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "gc/redjet/api/redjetTailTraces.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/sizes.hpp"

enum RedjetPointFlagBits : uint32_t {
  REDJET_PT_PARKPOINT_ACTIVE  = 1u << 0,
  REDJET_PT_DOCKPOINT_ACTIVE  = 1u << 1,
  REDJET_PT_BYPASS_WARMUP     = 1u << 2,
  REDJET_PT_BATCH_ACTIVE      = 1u << 3,
  REDJET_PT_EXEMPT_PIN        = 1u << 4,
  REDJET_PT_NESTED_ACTIVE     = 1u << 5,

  // Compatibility aliases
  REDJET_PT_SAFEPOINT_ACTIVE  = REDJET_PT_PARKPOINT_ACTIVE,
  REDJET_PT_PAUSEPOINT_ACTIVE = REDJET_PT_PARKPOINT_ACTIVE,
  REDJET_PT_HOOKPOINT_ACTIVE  = REDJET_PT_DOCKPOINT_ACTIVE
};

struct RedjetBatchFrameCompact {
  uint32_t batch_id;
  uint16_t priority;
  uint8_t  flags; // bit0=bypass_warmup, bit1=nested_allowed, bit2=group_scope
  uint8_t  depth;
};

class RedjetThreadLocalData {
public:
  static const uint32_t MAX_INLINE_PINNED_HDRS = 8;
  static const uint32_t MAX_BATCH_STACK_DEPTH  = 16;

private:
  // Hot JIT-accessed fields placed first for compact instruction offsets from Thread*
  volatile uint32_t       _point_flags;
  volatile uint32_t       _parkpoint_epoch;
  void*                   _active_dock_hdr;
  uint64_t                _dock_enter_tsc;
  uint32_t                _batch_depth;
  uint16_t                _active_priority;
  uint16_t                _preferred_core_id;
  uint32_t                _active_group_hex_id;
  uint32_t                _pinned_count;
  size_t                  _ergo_tlab_size;
  int64_t                 _last_tlab_time;
  void*                   _tbb_task_arena_ctx;
  uint64_t                _dock_bytes_since_check;
  uint64_t                _park_polls;
  uint64_t                _park_yields;
  uint64_t                _dock_triggers;
  uint64_t                _dock_checks;
  void*                   _inline_pinned_hdrs[MAX_INLINE_PINNED_HDRS];
  RedjetBatchFrameCompact _batch_stack[MAX_BATCH_STACK_DEPTH];
  void*                   _ext_point_ctx;
  volatile int            _nmethod_disarmed;

  RedjetThreadLocalData() :
    _point_flags(0),
    _parkpoint_epoch(0),
    _active_dock_hdr(nullptr),
    _dock_enter_tsc(0),
    _batch_depth(0),
    _active_priority(5),
    _preferred_core_id(0xFFFFu),
    _active_group_hex_id(0),
    _pinned_count(0),
    _ergo_tlab_size(0),
    _last_tlab_time(0),
    _tbb_task_arena_ctx(nullptr),
    _dock_bytes_since_check(0),
    _park_polls(0),
    _park_yields(0),
    _dock_triggers(0),
    _dock_checks(0),
    _ext_point_ctx(nullptr),
    _nmethod_disarmed(0) {
    for (uint32_t i = 0; i < MAX_INLINE_PINNED_HDRS; ++i) {
      _inline_pinned_hdrs[i] = nullptr;
    }
    for (uint32_t i = 0; i < MAX_BATCH_STACK_DEPTH; ++i) {
      _batch_stack[i].batch_id = 0;
      _batch_stack[i].priority = 5;
      _batch_stack[i].flags    = 0;
      _batch_stack[i].depth    = 0;
    }
  }

  static RedjetThreadLocalData* data(Thread* thread) {
    assert(UseRedjetGC, "Sanity");
    return reinterpret_cast<RedjetThreadLocalData*>(thread->gc_data());
  }

  static const RedjetThreadLocalData* data(const Thread* thread) {
    assert(UseRedjetGC, "Sanity");
    return reinterpret_cast<const RedjetThreadLocalData*>(thread->gc_data());
  }

public:
  static void initialize(Thread* thread) {
    new (thread->gc_data()) RedjetThreadLocalData();
    if (RedjetGCUseDeadReferenceSignals) {
      g_redjet_tail_traces.register_thread_clasp(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread)));
    }
  }

  static void create(Thread* thread) {
    initialize(thread);
  }

  static void destroy(Thread* thread) {
    data(thread)->~RedjetThreadLocalData();
  }

  // --- Point Flags ---
  static uint32_t point_flags(const Thread* thread) {
    return data(thread)->_point_flags;
  }

  static void set_point_flags(Thread* thread, uint32_t flags) {
    data(thread)->_point_flags = flags;
  }

  static void add_point_flags(Thread* thread, uint32_t flags) {
    data(thread)->_point_flags |= flags;
  }

  static void clear_point_flags(Thread* thread, uint32_t flags) {
    data(thread)->_point_flags &= ~flags;
  }

  static bool test_point_flag(const Thread* thread, RedjetPointFlagBits bit) {
    return (data(thread)->_point_flags & static_cast<uint32_t>(bit)) != 0;
  }

  // --- Parkpoint (Safepoint & Pausepoint) ---
  static bool is_parkpoint_active(const Thread* thread) {
    return test_point_flag(thread, REDJET_PT_PARKPOINT_ACTIVE);
  }

  static void set_parkpoint_active(Thread* thread, bool active) {
    if (active) add_point_flags(thread, REDJET_PT_PARKPOINT_ACTIVE);
    else        clear_point_flags(thread, REDJET_PT_PARKPOINT_ACTIVE);
  }

  static bool is_safepoint_active(const Thread* thread)  { return is_parkpoint_active(thread); }
  static void set_safepoint_active(Thread* thread, bool a){ set_parkpoint_active(thread, a); }
  static bool is_pausepoint_active(const Thread* thread) { return is_parkpoint_active(thread); }
  static void set_pausepoint_active(Thread* thread, bool a){ set_parkpoint_active(thread, a); }

  static uint32_t parkpoint_epoch(const Thread* thread) {
    return data(thread)->_parkpoint_epoch;
  }

  static void set_parkpoint_epoch(Thread* thread, uint32_t epoch) {
    data(thread)->_parkpoint_epoch = epoch;
  }

  // --- Dockpoint (Hookpoint & Controlpoint) ---
  static bool is_dockpoint_active(const Thread* thread) {
    return test_point_flag(thread, REDJET_PT_DOCKPOINT_ACTIVE);
  }

  static void set_dockpoint_active(Thread* thread, bool active) {
    if (active) add_point_flags(thread, REDJET_PT_DOCKPOINT_ACTIVE);
    else        clear_point_flags(thread, REDJET_PT_DOCKPOINT_ACTIVE);
  }

  static bool is_hookpoint_active(const Thread* thread)  { return is_dockpoint_active(thread); }
  static void set_hookpoint_active(Thread* thread, bool a){ set_dockpoint_active(thread, a); }

  static void* active_dock_hdr(const Thread* thread) {
    return data(thread)->_active_dock_hdr;
  }

  static void set_active_dock_hdr(Thread* thread, void* hdr) {
    data(thread)->_active_dock_hdr = hdr;
  }

  static void* active_hook_hdr(const Thread* thread)       { return active_dock_hdr(thread); }
  static void  set_active_hook_hdr(Thread* thread, void* h){ set_active_dock_hdr(thread, h); }

  static uint64_t dock_enter_tsc(const Thread* thread) {
    return data(thread)->_dock_enter_tsc;
  }

  static void set_dock_enter_tsc(Thread* thread, uint64_t tsc) {
    data(thread)->_dock_enter_tsc = tsc;
  }

  static uint64_t hook_enter_tsc(const Thread* thread)       { return dock_enter_tsc(thread); }
  static void     set_hook_enter_tsc(Thread* thread, uint64_t t){ set_dock_enter_tsc(thread, t); }

  // --- Batch & Group Scopes ---
  static uint32_t batch_depth(const Thread* thread) {
    return data(thread)->_batch_depth;
  }

  static void set_batch_depth(Thread* thread, uint32_t depth) {
    data(thread)->_batch_depth = depth;
    if (depth > 0) add_point_flags(thread, REDJET_PT_BATCH_ACTIVE);
    else           clear_point_flags(thread, REDJET_PT_BATCH_ACTIVE);
  }

  static uint16_t active_priority(const Thread* thread) {
    return data(thread)->_active_priority;
  }

  static void set_active_priority(Thread* thread, uint16_t prio) {
    data(thread)->_active_priority = prio;
  }

  static uint16_t preferred_core_id(const Thread* thread) {
    return data(thread)->_preferred_core_id;
  }

  static void set_preferred_core_id(Thread* thread, uint16_t core_id) {
    data(thread)->_preferred_core_id = core_id;
  }

  static uint32_t active_group_hex_id(const Thread* thread) {
    return data(thread)->_active_group_hex_id;
  }

  static void set_active_group_hex_id(Thread* thread, uint32_t gid) {
    data(thread)->_active_group_hex_id = gid;
  }

  static bool is_bypass_warmup(const Thread* thread) {
    return test_point_flag(thread, REDJET_PT_BYPASS_WARMUP);
  }

  static void set_bypass_warmup(Thread* thread, bool bypass) {
    if (bypass) add_point_flags(thread, REDJET_PT_BYPASS_WARMUP);
    else        clear_point_flags(thread, REDJET_PT_BYPASS_WARMUP);
  }

  static bool is_exempt_pin(const Thread* thread) {
    return test_point_flag(thread, REDJET_PT_EXEMPT_PIN);
  }

  static void set_exempt_pin(Thread* thread, bool exempt) {
    if (exempt) add_point_flags(thread, REDJET_PT_EXEMPT_PIN);
    else        clear_point_flags(thread, REDJET_PT_EXEMPT_PIN);
  }

  // --- Object Pinning ---
  static uint32_t pinned_count(const Thread* thread) {
    return data(thread)->_pinned_count;
  }

  static bool add_pinned_hdr(Thread* thread, void* hdr) {
    RedjetThreadLocalData* d = data(thread);
    if (d->_pinned_count < MAX_INLINE_PINNED_HDRS) {
      d->_inline_pinned_hdrs[d->_pinned_count++] = hdr;
      return true;
    }
    return false;
  }

  static bool remove_pinned_hdr(Thread* thread, void* hdr) {
    RedjetThreadLocalData* d = data(thread);
    for (uint32_t i = 0; i < d->_pinned_count; ++i) {
      if (d->_inline_pinned_hdrs[i] == hdr) {
        d->_inline_pinned_hdrs[i] = d->_inline_pinned_hdrs[--d->_pinned_count];
        d->_inline_pinned_hdrs[d->_pinned_count] = nullptr;
        return true;
      }
    }
    return false;
  }

  // --- Dockpoint Memory Pacing & Check ---
  static uint64_t dock_bytes_since_check(const Thread* thread) {
    return data(thread)->_dock_bytes_since_check;
  }

  static void set_dock_bytes_since_check(Thread* thread, uint64_t bytes) {
    data(thread)->_dock_bytes_since_check = bytes;
  }

  static uint64_t add_dock_bytes(Thread* thread, size_t bytes) {
    data(thread)->_dock_bytes_since_check += bytes;
    return data(thread)->_dock_bytes_since_check;
  }

  static uint64_t cp_bytes_since_check(const Thread* thread)      { return dock_bytes_since_check(thread); }
  static void     set_cp_bytes_since_check(Thread* t, uint64_t b) { set_dock_bytes_since_check(t, b); }

  // --- Ergo TLAB & Arena Context ---
  static size_t ergo_tlab_size(Thread* thread) {
    return data(thread)->_ergo_tlab_size;
  }

  static void set_ergo_tlab_size(Thread* thread, size_t sz) {
    data(thread)->_ergo_tlab_size = sz;
  }

  static int64_t last_tlab_time(Thread* thread) {
    return data(thread)->_last_tlab_time;
  }

  static void set_last_tlab_time(Thread* thread, int64_t t) {
    data(thread)->_last_tlab_time = t;
  }

  static void* tbb_task_arena_ctx(Thread* thread) {
    return data(thread)->_tbb_task_arena_ctx;
  }

  static void set_tbb_task_arena_ctx(Thread* thread, void* ctx) {
    data(thread)->_tbb_task_arena_ctx = ctx;
  }

  // JIT Instruction Offset Helpers
  static ByteSize point_flags_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _point_flags));
  }

  static ByteSize parkpoint_epoch_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _parkpoint_epoch));
  }

  static ByteSize active_dock_hdr_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _active_dock_hdr));
  }

  static ByteSize dock_enter_tsc_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _dock_enter_tsc));
  }

  static ByteSize batch_depth_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _batch_depth));
  }

  static ByteSize active_priority_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _active_priority));
  }

  static ByteSize dock_bytes_since_check_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _dock_bytes_since_check));
  }

  // Compatibility offsets
  static ByteSize pausepoint_epoch_offset()    { return parkpoint_epoch_offset(); }
  static ByteSize active_hook_hdr_offset()     { return active_dock_hdr_offset(); }
  static ByteSize hook_enter_tsc_offset()      { return dock_enter_tsc_offset(); }
  static ByteSize cp_bytes_since_check_offset(){ return dock_bytes_since_check_offset(); }

  // HotSpot Standard nmethod entry barrier support
  static ByteSize nmethod_disarmed_offset() {
    return Thread::gc_data_offset() + in_ByteSize(offset_of(RedjetThreadLocalData, _nmethod_disarmed));
  }

  static int nmethod_disarmed_value(const Thread* thread) {
    return data(thread)->_nmethod_disarmed;
  }

  static void set_nmethod_disarmed_value(Thread* thread, int val) {
    data(thread)->_nmethod_disarmed = val;
  }
};

STATIC_ASSERT(sizeof(RedjetThreadLocalData) <= sizeof(GCThreadLocalData));

#endif // SHARE_GC_REDJET_REDJETTHREADLOCALDATA_HPP
