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

#ifndef SHARE_GC_REDJET_API_REDJETOFFHEAPRECLAIMER_HPP
#define SHARE_GC_REDJET_API_REDJETOFFHEAPRECLAIMER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/api/redjetRJGC.hpp"
#include "gc/redjet/api/redjetNativeSDK.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// @RJGC: Automated Off-Heap Scanner and Reclaimer (PLAN.md Section 10.2)
//
// Governs off-heap memory lifetimes driven by @RJGC annotations:
// 1. @RJGC(Mark = "ImmediateEviction"):
//    Short-lived native buffers allocated in method scope are deterministically
//    evicted and returned to the OS / slab allocator upon scope termination.
//
// 2. @RJGC(Mark = "ContinuousFlowWatch"):
//    Long-lived, streaming, or asynchronous off-heap buffers are registered in
//    a continuous flow table. Background E-Core worker threads execute low-power
//    sweeps to evict memory once reference counts reach zero or canary epochs expire.
// ============================================================================

enum class RedjetOffHeapMarkPolicy : uint8_t {
  IMMEDIATE_EVICTION    = 0,
  CONTINUOUS_FLOW_WATCH = 1
};

enum class RedjetOffHeapScannerState : uint8_t {
  STOPPED  = 0,
  RUNNING  = 1,
  PAUSED   = 2,
  SWEEPING = 3
};

struct alignas(64) RedjetOffHeapDescriptor {
  void*                         buffer_address{ nullptr };
  size_t                        size_bytes{ 0 };
  uint32_t                      alignment{ 0 };
  RedjetOffHeapMarkPolicy       mark_policy{ RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION };
  uint32_t                      owner_thread_id{ 0 };
  uint64_t                      registered_epoch{ 0 };
  std::atomic<uint64_t>         last_accessed_timestamp_ns{ 0 };
  std::atomic<int32_t>          flow_ref_count{ 0 };
  std::atomic<bool>             is_evicted{ false };
  uint64_t                      canary_magic{ 0x524A47434F464648ULL }; // "RJGC_OFFH"
  void                        (*custom_free_func)(void*, size_t){ nullptr };
  RJGCTrackedHandle*            rjgc_handle{ nullptr };
  RedjetOffHeapDescriptor*      next{ nullptr };
  bool                          in_pool{ false };

  RedjetOffHeapDescriptor() = default;
};

struct RedjetOffHeapReclaimerStats {
  uint64_t total_immediate_evictions{ 0 };
  uint64_t total_continuous_registrations{ 0 };
  uint64_t total_continuous_reclamations{ 0 };
  uint64_t total_bytes_reclaimed{ 0 };
  uint64_t total_sweep_cycles{ 0 };
  uint64_t last_sweep_duration_ns{ 0 };
  uint64_t active_descriptors_count{ 0 };
  uint64_t canary_faults_detected{ 0 };
};

class RedjetOffHeapReclaimer {
public:
  static constexpr size_t BUCKET_COUNT = 1024;
  static constexpr size_t DESCRIPTOR_POOL_CAPACITY = 16384;
  static constexpr uint64_t CANARY_EXPECTED = 0x524A47434F464648ULL;

private:
  static RedjetOffHeapDescriptor                     _descriptor_pool[DESCRIPTOR_POOL_CAPACITY];
  static std::atomic<RedjetOffHeapDescriptor*>       _pool_free_head;
  static hs_ticket_lock_t                            _pool_lock;
  static std::atomic<bool>                           _initialized;
  static std::atomic<RedjetOffHeapDescriptor*>       _buckets[BUCKET_COUNT];
  static hs_ticket_lock_t                            _bucket_locks[BUCKET_COUNT];

  // Telemetry & Metrics
  static std::atomic<uint64_t>                       _total_immediate_evictions;
  static std::atomic<uint64_t>                       _total_continuous_registrations;
  static std::atomic<uint64_t>                       _total_continuous_reclamations;
  static std::atomic<uint64_t>                       _total_bytes_reclaimed;
  static std::atomic<uint64_t>                       _total_sweep_cycles;
  static std::atomic<uint64_t>                       _last_sweep_duration_ns;
  static std::atomic<uint64_t>                       _active_descriptors_count;
  static std::atomic<uint64_t>                       _canary_faults_detected;

  // Scanner background control
  static std::atomic<uint8_t>                        _scanner_state;
  static std::atomic<uint32_t>                       _sweep_interval_ms;
  static std::atomic<uint64_t>                       _global_reclaimer_epoch;

  static void ensure_pool_initialized();
  static RedjetOffHeapDescriptor* acquire_descriptor();
  static void release_descriptor(RedjetOffHeapDescriptor* desc);
  static inline size_t hash_address(const void* ptr) {
    uintptr_t u = reinterpret_cast<uintptr_t>(ptr);
    u ^= (u >> 16);
    u *= 0x85ebca6bULL;
    u ^= (u >> 13);
    u *= 0xc2b2ae35ULL;
    u ^= (u >> 16);
    return static_cast<size_t>(u & (BUCKET_COUNT - 1));
  }

public:
  // Lifecycle Management
  RJ_API static void initialize();
  RJ_API static void shutdown();

  // Allocation Registration
  RJ_API static RedjetOffHeapDescriptor* register_allocation(
      void* ptr,
      size_t size,
      RedjetOffHeapMarkPolicy policy,
      void (*dtor)(void*, size_t) = nullptr,
      uint32_t owner_thread_id = 0,
      uint32_t alignment = 64);

  RJ_API static RedjetOffHeapDescriptor* register_by_mark_string(
      void* ptr,
      size_t size,
      const char* mark_str,
      void (*dtor)(void*, size_t) = nullptr,
      uint32_t owner_thread_id = 0);

  // Eviction APIs
  RJ_API static bool evict_immediate(void* ptr);
  RJ_API static bool evict_descriptor(RedjetOffHeapDescriptor* desc);

  // Continuous Flow Reference Management
  RJ_API static void retain_flow_ref(void* ptr);
  RJ_API static void release_flow_ref(void* ptr);
  RJ_API static void touch_flow_ref(void* ptr);
  RJ_API static int32_t get_flow_ref_count(const void* ptr);

  // Automated Scanner Engine (Sweeps on Background E-Cores)
  RJ_API static void start_scanner_worker();
  RJ_API static void stop_scanner_worker();
  RJ_API static size_t run_continuous_sweep_cycle();
  RJ_API static bool is_scanner_running();
  RJ_API static void set_sweep_interval_ms(uint32_t interval_ms);
  RJ_API static uint32_t get_sweep_interval_ms();

  // Telemetry & Diagnostics
  RJ_API static RedjetOffHeapReclaimerStats get_stats();
  RJ_API static void reset_stats();

  // RAII Guard for deterministic ImmediateEviction scope exit
  class ScopedImmediateEvictor {
  private:
    void* _ptr{ nullptr };
    size_t _size{ 0 };
    void (*_dtor)(void*, size_t){ nullptr };

  public:
    inline ScopedImmediateEvictor(void* ptr, size_t size, void (*dtor)(void*, size_t) = nullptr)
      : _ptr(ptr), _size(size), _dtor(dtor) {
      if (_ptr && _size > 0) {
        RedjetOffHeapReclaimer::register_allocation(
            _ptr, _size, RedjetOffHeapMarkPolicy::IMMEDIATE_EVICTION, _dtor);
      }
    }

    inline ~ScopedImmediateEvictor() {
      if (_ptr) {
        RedjetOffHeapReclaimer::evict_immediate(_ptr);
      }
    }

    ScopedImmediateEvictor(const ScopedImmediateEvictor&) = delete;
    ScopedImmediateEvictor& operator=(const ScopedImmediateEvictor&) = delete;

    inline void cancel() {
      _ptr = nullptr;
    }
  };
};

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void*  hs_offheap_register(void* ptr, size_t size, const char* mark_policy_str);
RJ_API bool   hs_offheap_evict_immediate(void* ptr);
RJ_API void   hs_offheap_retain(void* ptr);
RJ_API void   hs_offheap_release(void* ptr);
RJ_API size_t hs_offheap_sweep_cycle(void);
RJ_API void   hs_offheap_scanner_start(void);
RJ_API void   hs_offheap_scanner_stop(void);
RJ_API size_t hs_offheap_get_reclaimed_bytes(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_API_REDJETOFFHEAPRECLAIMER_HPP
