/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETEMPTYBOX_HPP
#define SHARE_GC_REDLET_REDLETEMPTYBOX_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>

//
// RedletEmptyBox: Ultra-fast reusable hole header for Falline Snatching.
//
struct RedletEmptyBox {
  RedletEmptyBox* next;
  RedletEmptyBox* prev;
  size_t          aligned_hole_size;
  size_t          prev_payload_size;
  uint32_t        prev_type_id;
  uint32_t        is_decommitted : 1;
  uint32_t        decommitted_kb : 31;
  uint64_t        created_sweep_no;
  std::atomic<void*> forwarding_address;

  void init(uint32_t prev_tid, size_t prev_sz, size_t aligned_sz, void* fwd = nullptr, uint64_t sweep_no = 0) {
    next = nullptr;
    prev = nullptr;
    aligned_hole_size = aligned_sz;
    prev_payload_size = prev_sz;
    prev_type_id = prev_tid;
    is_decommitted = 0;
    decommitted_kb = 0;
    created_sweep_no = sweep_no;
    forwarding_address.store(fwd, std::memory_order_relaxed);
  }
};

#endif // SHARE_GC_REDLET_REDLETEMPTYBOX_HPP
