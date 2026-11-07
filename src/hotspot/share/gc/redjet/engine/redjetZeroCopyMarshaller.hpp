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

#ifndef SHARE_GC_REDJET_ENGINE_REDJETZEROCOPYMARSHALLER_HPP
#define SHARE_GC_REDJET_ENGINE_REDJETZEROCOPYMARSHALLER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/vfs/redjetVFSNode_ParkArena.hpp"

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
// Redjet Zero-Copy Native Marshaller (PLAN.md Section 8.6, 9.1)
// - Seamless Panama / FFM API & JNI Critical Array direct binding
// - Directly couples Java heap buffers to VFS Park Arena (is_pinned = 1)
// - Kernel Direct I/O ring integration (io_uring on Linux, IOCP on Windows)
// - Eliminates intermediate user-kernel data copying and safepoint stalling
// ============================================================================

enum class RedjetBufferLeaseState : uint8_t {
  AVAILABLE = 0,
  LEASED    = 1,
  EXPIRED   = 2
};

struct RedjetNativeBufferLease {
  void*                  buffer_address{ nullptr };
  size_t                 capacity_bytes{ 0 };
  size_t                 bytes_written{ 0 };
  uint32_t               lease_id{ 0 };
  uint32_t               thread_id{ 0 };
  RedjetBufferLeaseState state{ RedjetBufferLeaseState::AVAILABLE };
  uint64_t               lease_tsc{ 0 };
  bool                   is_kernel_mapped{ false };
};

class RedjetZeroCopyMarshaller {
public:
  static constexpr size_t MAX_CONCURRENT_LEASES = 1024;

private:
  static RedjetZeroCopyMarshaller* _instance;
  static std::atomic<bool>         _initialized;

  RedjetVFSNode_ParkArena*         _park_arena{ nullptr };
  RedjetNativeBufferLease          _leases[MAX_CONCURRENT_LEASES];
  std::atomic<uint32_t>            _next_lease_id{ 1 };
  hs_ticket_lock_t                 _marshaller_lock;

  // Telemetry
  std::atomic<uint64_t>            _total_leases_granted{ 0 };
  std::atomic<uint64_t>            _total_leases_released{ 0 };
  std::atomic<uint64_t>            _total_zero_copy_bytes{ 0 };
  std::atomic<uint64_t>            _total_kernel_io_completions{ 0 };

public:
  RedjetZeroCopyMarshaller();
  ~RedjetZeroCopyMarshaller();

  static void initialize(RedjetVFSNode_ParkArena* park_arena = nullptr);
  static void destroy();
  static RedjetZeroCopyMarshaller* get() noexcept { return _instance; }

  void set_park_arena(RedjetVFSNode_ParkArena* arena) noexcept { _park_arena = arena; }

  // 1. Lease Buffer for Zero-Copy Native/Kernel Interaction
  RJ_API RedjetNativeBufferLease* acquire_buffer_lease(size_t required_bytes, uint32_t thread_id);

  // 2. Release Buffer Lease back to Park Arena pool
  RJ_API bool release_buffer_lease(uint32_t lease_id);

  // 3. Direct Kernel Asynchronous I/O Simulation / Completion
  RJ_API bool complete_kernel_io(uint32_t lease_id, size_t bytes_transferred);

  // 4. Validate Pinning
  RJ_API bool is_buffer_pinned(const void* ptr) const noexcept;

  // Telemetry
  uint64_t leases_granted()      const noexcept { return _total_leases_granted.load(std::memory_order_relaxed); }
  uint64_t leases_released()     const noexcept { return _total_leases_released.load(std::memory_order_relaxed); }
  uint64_t zero_copy_bytes()     const noexcept { return _total_zero_copy_bytes.load(std::memory_order_relaxed); }
  uint64_t kernel_completions()  const noexcept { return _total_kernel_io_completions.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ENGINE_REDJETZEROCOPYMARSHALLER_HPP
