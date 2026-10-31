/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_VFS_REDLETVFSNODE_HPP
#define SHARE_GC_REDLET_VFS_REDLETVFSNODE_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include <atomic>
#include <cstring>

// Forward declaration
class RedletVFSNode;

class RedletVFSNode : public CHeapObj<mtGC> {
public:
  char                   _name[64];
  char                   _full_path[192];
  size_t                 _quota_bytes;
  std::atomic<size_t>    _used_bytes;

  RedletVFSNode*         _parent;
  GrowableArray<RedletVFSNode*> _children;

  RedletVFSNode(const char* name = "", const char* full_path = "", RedletVFSNode* parent = nullptr, size_t quota = 0) :
    _quota_bytes(quota),
    _used_bytes(0),
    _parent(parent),
    _children(0) {
    strncpy(_name, name, sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
    strncpy(_full_path, full_path, sizeof(_full_path) - 1);
    _full_path[sizeof(_full_path) - 1] = '\0';
  }

  ~RedletVFSNode() {
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

#endif // SHARE_GC_REDLET_VFS_REDLETVFSNODE_HPP
