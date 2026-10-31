/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
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
