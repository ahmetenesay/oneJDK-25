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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETAOSSOAMORPH_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETAOSSOAMORPH_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif
#include "utilities/globalDefinitions.hpp"
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
// AOS -> SOA Dynamic Shadow Slicing Engine (PLAN.md Section 8.8)
// Converts Array-of-Structures (Java object layout) into Structure-of-Arrays
// (contiguous SIMD vector slices) in Runway/Pitstop with bidirectional write-back.
// ============================================================================

enum class RedjetFieldType : uint8_t {
  FIELD_INT8    = 0, // 1 byte
  FIELD_INT16   = 1, // 2 bytes
  FIELD_INT32   = 2, // 4 bytes
  FIELD_FLOAT32 = 3, // 4 bytes
  FIELD_INT64   = 4, // 8 bytes
  FIELD_FLOAT64 = 5, // 8 bytes
  FIELD_OOP     = 6  // 8-byte reference (or 4-byte compressed oop)
};

// Layout descriptor for a single field inside an AOS structure
struct RedjetFieldDescriptor {
  uint32_t        field_offset{ 0 };
  uint16_t        field_size{ 4 };
  RedjetFieldType field_type{ RedjetFieldType::FIELD_INT32 };
  uint8_t         is_hot{ 1 };
};

// Represents a morphed SOA slice plane residing in Runway/Pitstop
struct alignas(64) RedjetSoaSlicePlane {
  uint8_t*                  slice_buffer{ nullptr };
  size_t                    element_count{ 0 };
  size_t                    element_size{ 4 };
  size_t                    capacity_bytes{ 0 };
  uint32_t                  field_offset_in_aos{ 0 };
  std::atomic<bool>         is_dirty{ false };
  std::atomic<uint64_t>     last_sync_tsc{ 0 };

  inline void mark_dirty() noexcept {
    is_dirty.store(true, std::memory_order_release);
  }

  inline bool check_and_clear_dirty() noexcept {
    return is_dirty.exchange(false, std::memory_order_acq_rel);
  }
};

// Comprehensive Morph Session tracking an AOS array to SOA slice conversion
struct alignas(64) RedjetAosSoaSession {
  static constexpr size_t MAX_SLICES = 16;

  uintptr_t             aos_base_address{ 0 };
  size_t                total_elements{ 0 };
  size_t                aos_element_stride{ 0 };
  uint32_t              slice_count{ 0 };
  RedjetSoaSlicePlane   slices[MAX_SLICES];
  hs_ticket_lock_t      session_lock;
  std::atomic<bool>     is_active{ false };
  std::atomic<uint64_t> total_mutations{ 0 };
  std::atomic<uint64_t> total_scatters{ 0 };

  void reset() noexcept {
    aos_base_address = 0;
    total_elements = 0;
    aos_element_stride = 0;
    slice_count = 0;
    for (size_t i = 0; i < MAX_SLICES; ++i) {
      slices[i].slice_buffer = nullptr;
      slices[i].element_count = 0;
      slices[i].element_size = 0;
      slices[i].capacity_bytes = 0;
      slices[i].field_offset_in_aos = 0;
      slices[i].is_dirty.store(false, std::memory_order_relaxed);
      slices[i].last_sync_tsc.store(0, std::memory_order_relaxed);
    }
    is_active.store(false, std::memory_order_relaxed);
    total_mutations.store(0, std::memory_order_relaxed);
    total_scatters.store(0, std::memory_order_relaxed);
  }
};

class RedjetAosSoaMorph : public CHeapObj<mtGC> {
public:
  static constexpr size_t MAX_CONCURRENT_SESSIONS = 32;
  static constexpr size_t DEFAULT_SLICE_BUFFER_SIZE = 1024 * 1024; // 1 MB slice buffer

private:
  RedjetAosSoaSession   _sessions[MAX_CONCURRENT_SESSIONS];
  uint8_t*              _pitstop_buffer_pool{ nullptr };
  size_t                _pool_capacity{ 0 };
  std::atomic<size_t>   _pool_offset{ 0 };
  hs_ticket_lock_t      _morph_lock;

  // Telemetry
  std::atomic<uint64_t> _total_aos_to_soa_transforms{ 0 };
  std::atomic<uint64_t> _total_elements_sliced{ 0 };
  std::atomic<uint64_t> _total_mutations_written_back{ 0 };
  std::atomic<uint64_t> _total_scatter_operations{ 0 };
  std::atomic<uint64_t> _total_bytes_transferred{ 0 };

  uint8_t* allocate_slice_storage(size_t bytes);

public:
  RedjetAosSoaMorph();
  ~RedjetAosSoaMorph();

  void init(void* pitstop_pool, size_t pool_bytes);
  void destroy();

  // =========================================================================
  // 1. Transform AOS Array to SOA Slice Planes (Dynamic Shadow Slicing)
  // Non-blocking shadow copy of target fields into contiguous SIMD planes
  // =========================================================================
  RJ_API uint32_t slice_aos_to_soa(uintptr_t aos_base,
                                   size_t element_count,
                                   size_t aos_stride,
                                   const RedjetFieldDescriptor* fields,
                                   size_t field_count);

  // =========================================================================
  // 2. Direct Slice Accessors for SIMD / Puzzler Execution
  // =========================================================================
  RJ_API void* get_slice_pointer(uint32_t session_id, uint32_t field_idx) const noexcept;
  RJ_API size_t get_slice_element_count(uint32_t session_id) const noexcept;

  // =========================================================================
  // 3. Shadow Write-Back on Mutation (512-Bit SIMD Scatter / Block Write)
  // Scatters updated slice values back to the canonical AOS heap memory
  // =========================================================================
  RJ_API bool write_back_slice(uint32_t session_id, uint32_t field_idx);
  RJ_API bool write_back_all(uint32_t session_id);

  // Single-element mutation with immediate or deferred write-back
  RJ_API bool mutate_slice_element(uint32_t session_id,
                                   uint32_t field_idx,
                                   size_t element_index,
                                   const void* new_value,
                                   bool immediate_write_back = false);

  // =========================================================================
  // 4. Session Teardown & Release
  // =========================================================================
  RJ_API void release_session(uint32_t session_id);

  // Telemetry accessors
  uint64_t total_transforms() const noexcept { return _total_aos_to_soa_transforms.load(std::memory_order_relaxed); }
  uint64_t total_sliced_elements() const noexcept { return _total_elements_sliced.load(std::memory_order_relaxed); }
  uint64_t total_mutations_written_back() const noexcept { return _total_mutations_written_back.load(std::memory_order_relaxed); }
  uint64_t total_scatter_operations() const noexcept { return _total_scatter_operations.load(std::memory_order_relaxed); }
  uint64_t total_bytes_transferred() const noexcept { return _total_bytes_transferred.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETAOSSOAMORPH_HPP
