/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 */

#ifndef SHARE_GC_REDLET_REDLETFALLINE_HPP
#define SHARE_GC_REDLET_REDLETFALLINE_HPP

#include "gc/redlet/redletEmptyBox.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include <atomic>

class RedletFalline : public CHeapObj<mtGC> {
public:
  static constexpr size_t NUM_EMPTYBOX_BINS = 8;
  static constexpr size_t EMPTYBOX_BIN_SIZES[NUM_EMPTYBOX_BINS] = {
    64,
    256,
    1024,
    4096,
    16384,
    65536,
    262144,
    SIZE_MAX
  };

  static inline int find_emptybox_bin(size_t sz) {
    for (int i = 0; i < (int)NUM_EMPTYBOX_BINS - 1; ++i) {
      if (sz <= EMPTYBOX_BIN_SIZES[i]) return i;
    }
    return (int)NUM_EMPTYBOX_BINS - 1;
  }

private:
  RedletEmptyBox*     _bin_heads[NUM_EMPTYBOX_BINS];
  size_t              _bin_counts[NUM_EMPTYBOX_BINS];
  std::atomic<bool>   _bin_locks[NUM_EMPTYBOX_BINS];
  std::atomic<size_t> _emptybox_count;
  std::atomic<bool>   _is_compacting;

  std::atomic<uint64_t> _snatch_hit_count;
  std::atomic<uint64_t> _snatch_miss_count;
  std::atomic<uint64_t> _split_count;
  std::atomic<uint64_t> _coalesced_merge_count;

  inline void lock_bin(int bin) {
    while (_bin_locks[bin].exchange(true, std::memory_order_acquire)) {
      os::naked_pause();
    }
  }

  inline void unlock_bin(int bin) {
    _bin_locks[bin].store(false, std::memory_order_release);
  }

public:
  RedletFalline();
  ~RedletFalline();

  void register_emptybox(size_t aligned_size, RedletEmptyBox* eb, uint64_t sweep_no = 0);
  RedletEmptyBox* snatch_emptybox(size_t needed_bytes, uint64_t current_sweep = 0);

  size_t emptybox_count() const { return _emptybox_count.load(std::memory_order_relaxed); }
  uint64_t snatch_hits()  const { return _snatch_hit_count.load(std::memory_order_relaxed); }
  uint64_t snatch_misses() const { return _snatch_miss_count.load(std::memory_order_relaxed); }
  uint64_t split_count()   const { return _split_count.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDLET_REDLETFALLINE_HPP
