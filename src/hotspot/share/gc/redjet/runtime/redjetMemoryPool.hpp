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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETMEMORYPOOL_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETMEMORYPOOL_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "services/memoryPool.hpp"
#include "services/memoryUsage.hpp"
#include "utilities/ostream.hpp"
#else
class MemoryUsage {
private:
  size_t _init{0};
  size_t _used{0};
  size_t _committed{0};
  size_t _max{0};
public:
  MemoryUsage(size_t init, size_t used, size_t committed, size_t max)
    : _init(init), _used(used), _committed(committed), _max(max) {}
  size_t init_size()  const noexcept { return _init; }
  size_t used()       const noexcept { return _used; }
  size_t committed()  const noexcept { return _committed; }
  size_t max_size()   const noexcept { return _max; }
};

class CollectedMemoryPool {
public:
  CollectedMemoryPool(const char* name, size_t init, size_t max, bool support_usage_threshold) {}
  virtual size_t used_in_bytes() = 0;
  virtual size_t max_size() const = 0;
  virtual MemoryUsage get_memory_usage() = 0;
};

class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;

// ============================================================================
// 1. REDJET UNIFIED HEAP POOL (Top-Level MXBean Memory Pool)
// Represents the entire active Redjet virtual heap backing all slabs.
// ============================================================================
class RedjetUnifiedHeapPool : public CollectedMemoryPool {
private:
  RedjetHeap*           _heap;
  std::atomic<size_t>   _peak_used{ 0 };
  std::atomic<uint64_t> _usage_queries{ 0 };
  std::atomic<size_t>   _warning_threshold{ 0 };
  std::atomic<size_t>   _critical_threshold{ 0 };
  std::atomic<uint64_t> _threshold_crossings{ 0 };

public:
  explicit RedjetUnifiedHeapPool(RedjetHeap* heap);
  size_t committed_in_bytes();
  size_t used_in_bytes() override;
  size_t max_size() const override;
  MemoryUsage get_memory_usage() override;
  MemoryUsage get_peak_memory_usage();
  void reset_peak_memory_usage();

  void set_warning_threshold(size_t bytes) { _warning_threshold.store(bytes, std::memory_order_relaxed); }
  void set_critical_threshold(size_t bytes){ _critical_threshold.store(bytes, std::memory_order_relaxed); }

  uint64_t usage_queries()       const noexcept { return _usage_queries.load(std::memory_order_relaxed); }
  uint64_t threshold_crossings() const noexcept { return _threshold_crossings.load(std::memory_order_relaxed); }
};

// ============================================================================
// 2. REDJET RUNWAY MEMORY POOL (Young / Ephemeral Allocation Runway MXBean)
// Represents young thread-local buffers, bumps, and ephemeral flight corridors.
// ============================================================================
class RedjetRunwayPool : public CollectedMemoryPool {
private:
  RedjetHeap*           _heap;
  std::atomic<size_t>   _peak_used{ 0 };
  std::atomic<uint64_t> _usage_queries{ 0 };
  std::atomic<size_t>   _warning_threshold{ 0 };
  std::atomic<size_t>   _critical_threshold{ 0 };
  std::atomic<uint64_t> _threshold_crossings{ 0 };

public:
  explicit RedjetRunwayPool(RedjetHeap* heap);
  size_t committed_in_bytes();
  size_t used_in_bytes() override;
  size_t max_size() const override;
  MemoryUsage get_memory_usage() override;
  MemoryUsage get_peak_memory_usage();
  void reset_peak_memory_usage();

  void set_warning_threshold(size_t bytes) { _warning_threshold.store(bytes, std::memory_order_relaxed); }
  void set_critical_threshold(size_t bytes){ _critical_threshold.store(bytes, std::memory_order_relaxed); }

  uint64_t usage_queries()       const noexcept { return _usage_queries.load(std::memory_order_relaxed); }
  uint64_t threshold_crossings() const noexcept { return _threshold_crossings.load(std::memory_order_relaxed); }
};

// ============================================================================
// 3. REDJET HOTFIELD MEMORY POOL (Survivor / Medium-Lived Arena MXBean)
// Represents objects surviving young cycles and actively referenced during flight.
// ============================================================================
class RedjetHotfieldPool : public CollectedMemoryPool {
private:
  RedjetHeap*           _heap;
  std::atomic<size_t>   _peak_used{ 0 };
  std::atomic<uint64_t> _usage_queries{ 0 };
  std::atomic<size_t>   _warning_threshold{ 0 };
  std::atomic<size_t>   _critical_threshold{ 0 };
  std::atomic<uint64_t> _threshold_crossings{ 0 };

public:
  explicit RedjetHotfieldPool(RedjetHeap* heap);
  size_t committed_in_bytes();
  size_t used_in_bytes() override;
  size_t max_size() const override;
  MemoryUsage get_memory_usage() override;
  MemoryUsage get_peak_memory_usage();
  void reset_peak_memory_usage();

  void set_warning_threshold(size_t bytes) { _warning_threshold.store(bytes, std::memory_order_relaxed); }
  void set_critical_threshold(size_t bytes){ _critical_threshold.store(bytes, std::memory_order_relaxed); }

  uint64_t usage_queries()       const noexcept { return _usage_queries.load(std::memory_order_relaxed); }
  uint64_t threshold_crossings() const noexcept { return _threshold_crossings.load(std::memory_order_relaxed); }
};

// ============================================================================
// 4. REDJET LANDFILL MEMORY POOL (Tenured / Old Generation MXBean)
// Represents mature objects, large array chunks, and long-lived system roots.
// ============================================================================
class RedjetLandfillPool : public CollectedMemoryPool {
private:
  RedjetHeap*           _heap;
  std::atomic<size_t>   _peak_used{ 0 };
  std::atomic<uint64_t> _usage_queries{ 0 };
  std::atomic<size_t>   _warning_threshold{ 0 };
  std::atomic<size_t>   _critical_threshold{ 0 };
  std::atomic<uint64_t> _threshold_crossings{ 0 };

public:
  explicit RedjetLandfillPool(RedjetHeap* heap);
  size_t committed_in_bytes();
  size_t used_in_bytes() override;
  size_t max_size() const override;
  MemoryUsage get_memory_usage() override;
  MemoryUsage get_peak_memory_usage();
  void reset_peak_memory_usage();

  void set_warning_threshold(size_t bytes) { _warning_threshold.store(bytes, std::memory_order_relaxed); }
  void set_critical_threshold(size_t bytes){ _critical_threshold.store(bytes, std::memory_order_relaxed); }

  uint64_t usage_queries()       const noexcept { return _usage_queries.load(std::memory_order_relaxed); }
  uint64_t threshold_crossings() const noexcept { return _threshold_crossings.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETMEMORYPOOL_HPP
