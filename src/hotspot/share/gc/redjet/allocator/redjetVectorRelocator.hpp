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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETVECTORRELOCATOR_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETVECTORRELOCATOR_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
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
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// Redjet Vectorized Object Relocator & Forwarding Patcher (PLAN.md Section 4.3, 7.3)
// - High-speed vectorized object copy pipeline via AVX-512 / AVX2 / Non-Temporal Stores
// - Atomic Forwarding Pointer Installation with CAS & lockless self-forward detection
// - TailTraces Clasp Relinker for evacuated references
// - Batch Evacuation Queue (up to 64 references in flight)
// - 64-byte vector alignment hazard enforcement
// ============================================================================

enum class RedjetRelocateResult : uint8_t {
  RELOCATE_SUCCESS_COPIED         = 0,
  RELOCATE_ALREADY_FORWARDED      = 1,
  RELOCATE_FAILED_DEST_EXHAUSTED  = 2,
  RELOCATE_FAILED_PINNED_OBJECT   = 3,
  RELOCATE_FAILED_CONCURRENT_LOST = 4
};

struct RedjetRelocationTask {
  void*    src_obj{ nullptr };
  void*    dest_obj{ nullptr };
  size_t   size_bytes{ 0 };
  uint32_t thread_id{ 0 };
  bool     has_clasp{ false };
  uint32_t clasp_id{ 0 };
};

struct RedjetRelocationReport {
  size_t   objects_evacuated{ 0 };
  size_t   bytes_evacuated{ 0 };
  size_t   non_temporal_bytes{ 0 };
  size_t   forwarding_conflicts_resolved{ 0 };
  size_t   tailtraces_clasps_relinked{ 0 };
  uint64_t duration_tsc{ 0 };
};

class RedjetVectorRelocator {
public:
  static constexpr size_t MAX_BATCH_CAPACITY       = 64;
  static constexpr size_t NON_TEMPORAL_THRESHOLD   = 1024; // 1 KB+ uses streaming non-temporal stores
  static constexpr uintptr_t FORWARD_BIT_MASK      = 0x1ULL; // Bit 0 indicates forwarded pointer

private:
  static RedjetVectorRelocator* _instance;
  static std::atomic<bool>      _initialized;

  hs_ticket_lock_t              _relocator_lock;

  // Batch queue
  RedjetRelocationTask          _batch_queue[MAX_BATCH_CAPACITY];
  size_t                        _batch_count{ 0 };

  // Telemetry counters
  std::atomic<uint64_t>         _total_objects_relocated{ 0 };
  std::atomic<uint64_t>         _total_bytes_relocated{ 0 };
  std::atomic<uint64_t>         _total_nt_bytes_written{ 0 };
  std::atomic<uint64_t>         _total_forwarding_races{ 0 };
  std::atomic<uint64_t>         _total_clasps_updated{ 0 };

  // Internal vectorized copy routines
  static void copy_payload_avx512(void* dest, const void* src, size_t bytes) noexcept;
  static void copy_payload_avx2(void* dest, const void* src, size_t bytes) noexcept;
  static void copy_payload_streaming(void* dest, const void* src, size_t bytes) noexcept;

public:
  RedjetVectorRelocator();
  ~RedjetVectorRelocator();

  static void initialize();
  static void destroy();
  static RedjetVectorRelocator* get() noexcept { return _instance; }

  // 1. Single Object Relocation with Atomic Forwarding Installation
  RJ_API RedjetRelocateResult relocate_object(void* src_obj,
                                              void* dest_buf,
                                              size_t obj_size,
                                              void** out_forwarded_addr);

  // 2. Query Forwarded Address (Lock-free fast-path)
  RJ_API static inline void* get_forwarded_address(void* obj_addr) noexcept {
    if (obj_addr == nullptr) return nullptr;
    uintptr_t header = *reinterpret_cast<const volatile uintptr_t*>(obj_addr);
    if ((header & FORWARD_BIT_MASK) != 0) {
      return reinterpret_cast<void*>(header & ~FORWARD_BIT_MASK);
    }
    return nullptr; // Not yet forwarded
  }

  // 3. Batch Enqueue & Dispatch
  RJ_API bool enqueue_relocation(const RedjetRelocationTask& task);
  RJ_API RedjetRelocationReport flush_batch();

  // 4. Memory Stream Drain (ensures non-temporal stores are visible across CCDs)
  RJ_API static void sfence_barrier() noexcept;

  // Telemetry queries
  uint64_t objects_relocated() const noexcept { return _total_objects_relocated.load(std::memory_order_relaxed); }
  uint64_t bytes_relocated()   const noexcept { return _total_bytes_relocated.load(std::memory_order_relaxed); }
  uint64_t nt_bytes_written()  const noexcept { return _total_nt_bytes_written.load(std::memory_order_relaxed); }
  uint64_t forwarding_races()  const noexcept { return _total_forwarding_races.load(std::memory_order_relaxed); }
  uint64_t clasps_updated()    const noexcept { return _total_clasps_updated.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETVECTORRELOCATOR_HPP
