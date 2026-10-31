/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletMemoryPool.hpp"

RedletMemoryPool::RedletMemoryPool(RedletHeap* heap) :
  CollectedMemoryPool("Redlet Heap",
                      heap->capacity(),
                      heap->max_capacity(),
                      false),
  _heap(heap) {
  assert(UseRedletGC, "sanity");
}

size_t RedletMemoryPool::committed_in_bytes() {
  return _heap->capacity();
}

size_t RedletMemoryPool::used_in_bytes() {
  return _heap->used();
}

size_t RedletMemoryPool::max_size() const {
  return _heap->max_capacity();
}

MemoryUsage RedletMemoryPool::get_memory_usage() {
  size_t initial_sz = initial_size();
  size_t max_sz     = max_size();
  size_t used       = used_in_bytes();
  size_t committed  = committed_in_bytes();

  return MemoryUsage(initial_sz, used, committed, max_sz);
}
