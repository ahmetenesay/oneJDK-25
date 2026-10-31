/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_VFS_REDLETVFSTREE_HPP
#define SHARE_GC_REDLET_VFS_REDLETVFSTREE_HPP

#include "gc/redlet/vfs/redletVFSNode.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/ostream.hpp"
#include <atomic>

class RedletVFSTree : public CHeapObj<mtGC> {
private:
  RedletVFSNode*    _root;
  std::atomic<bool> _lock;

  inline void lock() {
    while (_lock.exchange(true, std::memory_order_acquire)) {
      os::naked_pause();
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
