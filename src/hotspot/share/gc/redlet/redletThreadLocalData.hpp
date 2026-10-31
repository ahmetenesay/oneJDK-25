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

#ifndef SHARE_GC_REDLET_REDLETTHREADLOCALDATA_HPP
#define SHARE_GC_REDLET_REDLETTHREADLOCALDATA_HPP

#include "gc/shared/gc_globals.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/debug.hpp"

class RedletThreadLocalData {
private:
  size_t  _ergo_tlab_size;
  int64_t _last_tlab_time;
  void*   _tbb_task_arena_ctx;

  RedletThreadLocalData() :
    _ergo_tlab_size(0),
    _last_tlab_time(0),
    _tbb_task_arena_ctx(nullptr) {}

  static RedletThreadLocalData* data(Thread* thread) {
    assert(UseRedletGC, "Sanity");
    return thread->gc_data<RedletThreadLocalData>();
  }

public:
  static void create(Thread* thread) {
    new (data(thread)) RedletThreadLocalData();
  }

  static void destroy(Thread* thread) {
    data(thread)->~RedletThreadLocalData();
  }

  static size_t ergo_tlab_size(Thread* thread) {
    return data(thread)->_ergo_tlab_size;
  }

  static void set_ergo_tlab_size(Thread* thread, size_t val) {
    data(thread)->_ergo_tlab_size = val;
  }

  static int64_t last_tlab_time(Thread* thread) {
    return data(thread)->_last_tlab_time;
  }

  static void set_last_tlab_time(Thread* thread, int64_t val) {
    data(thread)->_last_tlab_time = val;
  }

  static void* tbb_task_arena_ctx(Thread* thread) {
    return data(thread)->_tbb_task_arena_ctx;
  }

  static void set_tbb_task_arena_ctx(Thread* thread, void* ctx) {
    data(thread)->_tbb_task_arena_ctx = ctx;
  }
};

#endif // SHARE_GC_REDLET_REDLETTHREADLOCALDATA_HPP
