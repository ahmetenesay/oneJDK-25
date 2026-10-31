/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETMEMORYPOOL_HPP
#define SHARE_GC_REDLET_REDLETMEMORYPOOL_HPP

#include "services/memoryPool.hpp"
#include "services/memoryUsage.hpp"

class RedletHeap;

class RedletMemoryPool : public CollectedMemoryPool {
private:
  RedletHeap* _heap;

public:
  RedletMemoryPool(RedletHeap* heap);
  size_t committed_in_bytes() override;
  size_t used_in_bytes()      override;
  size_t max_size()     const override;
  MemoryUsage get_memory_usage() override;
};

#endif // SHARE_GC_REDLET_REDLETMEMORYPOOL_HPP
