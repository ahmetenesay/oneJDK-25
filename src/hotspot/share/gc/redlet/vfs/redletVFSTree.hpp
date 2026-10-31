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

#ifndef SHARE_GC_REDLET_VFS_REDLETVFSTREE_HPP
#define SHARE_GC_REDLET_VFS_REDLETVFSTREE_HPP

#include <atomic>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif
#ifdef REDLET_STANDALONE_BUILD
#include "gc/redlet/vfs/redletVFSNode.hpp"
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "gc/redlet/vfs/redletVFSNode.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/ostream.hpp"
#endif

class RedletVFSTree : public CHeapObj<mtGC> {
private:
  RedletVFSNode*    _root;
  std::atomic<bool> _lock;

  inline void lock() {
    while (_lock.exchange(true, std::memory_order_acquire)) {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
      _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
      __yield();
#else
      __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
    }
  }

  inline void unlock() {
    _lock.store(false, std::memory_order_release);
  }

  RedletVFSNode* _find_node_unlocked(const char* path);
  RedletVFSNode* _mkdir_p_unlocked(const char* path, size_t quota_bytes = 0);

public:
  RedletVFSTree();
  ~RedletVFSTree();

  void init();
  void destroy();

  RedletVFSNode* mkdir_p(const char* path, size_t quota_bytes = 0);
  RedletVFSNode* find_node(const char* path);
  size_t get_used_bytes(const char* path);

  void print_on(outputStream* st) const;
  void print_node_recursive(outputStream* st, RedletVFSNode* node, int indent) const;
};

#endif // SHARE_GC_REDLET_VFS_REDLETVFSTREE_HPP
