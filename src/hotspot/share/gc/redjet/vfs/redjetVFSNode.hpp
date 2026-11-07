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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSNODE_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSNODE_HPP

#ifdef REDJET_STANDALONE_BUILD
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
#include <atomic>

#ifndef CHEAPOBJ_FALLBACK_DEFINED
#define CHEAPOBJ_FALLBACK_DEFINED
template <typename T> class CHeapObjDummy {};
#define CHeapObj CHeapObjDummy
#define mtGC int
#endif

template<typename E>
class GrowableArray {
  std::vector<E> _v;
public:
  GrowableArray(int = 0) {}
  int length() const { return static_cast<int>(_v.size()); }
  void append(const E& elem) { _v.push_back(elem); }
  E& at(int i) { return _v[i]; }
  const E& at(int i) const { return _v[i]; }
  void clear() { _v.clear(); }
};
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include <atomic>
#include <cstring>
#endif

// Logical memory ownership domain across both on_heap and off_heap VFS subtrees
enum class RedjetMemoryDomain : uint8_t {
  JAVA_MEMORY    = 0, // User objects (motherline, hotfield, park_arena, FFM reserved_boot)
  HOTSPOT_MEMORY = 1  // VM/GC/JIT internals (code_boot, header_pane, log_boot, rpss, puzzle_arena)
};

// Forward declaration
class RedjetVFSNode;

class RedjetVFSNode : public CHeapObj<mtGC> {
public:
  char                   _name[64];
  char                   _full_path[192];
  size_t                 _quota_bytes;
  std::atomic<size_t>    _used_bytes;
  RedjetMemoryDomain     _domain;
  bool                   _is_on_heap;

  RedjetVFSNode*         _parent;
  GrowableArray<RedjetVFSNode*> _children;

  static RedjetMemoryDomain classify_domain_for_path(const char* path) noexcept {
    if (path == nullptr) return RedjetMemoryDomain::JAVA_MEMORY;
    if (strstr(path, "code_boot") != nullptr ||
        strstr(path, "header_pane") != nullptr ||
        strstr(path, "log_boot") != nullptr ||
        strstr(path, "rpss") != nullptr ||
        strstr(path, "puzzle_arena") != nullptr) {
      return RedjetMemoryDomain::HOTSPOT_MEMORY;
    }
    return RedjetMemoryDomain::JAVA_MEMORY;
  }

  static bool classify_on_heap_for_path(const char* path) noexcept {
    if (path == nullptr) return true;
    return strstr(path, "/off_heap") == nullptr;
  }

  RedjetVFSNode(const char* name = "", const char* full_path = "", RedjetVFSNode* parent = nullptr, size_t quota = 0) :
    _quota_bytes(quota),
    _used_bytes(0),
    _domain(classify_domain_for_path(full_path)),
    _is_on_heap(classify_on_heap_for_path(full_path)),
    _parent(parent),
    _children(0) {
    strncpy(_name, name, sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
    strncpy(_full_path, full_path, sizeof(_full_path) - 1);
    _full_path[sizeof(_full_path) - 1] = '\0';
  }

  virtual ~RedjetVFSNode() {
    for (int i = 0; i < _children.length(); ++i) {
      delete _children.at(i);
    }
    _children.clear();
  }

  const char* name()      const { return _name; }
  const char* full_path() const { return _full_path; }
  size_t      quota()     const { return _quota_bytes; }
  size_t      used()      const { return _used_bytes.load(std::memory_order_relaxed); }

  void add_used(size_t bytes) {
    _used_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }

  void sub_used(size_t bytes) {
    _used_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSNODE_HPP