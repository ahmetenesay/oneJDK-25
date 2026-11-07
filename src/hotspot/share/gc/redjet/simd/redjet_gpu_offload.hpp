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

#ifndef SHARE_GC_REDJET_SIMD_REDJET_GPU_OFFLOAD_HPP
#define SHARE_GC_REDJET_SIMD_REDJET_GPU_OFFLOAD_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/globalDefinitions.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

enum class RedjetGPUBackend : uint8_t {
  NONE       = 0,
  LEVEL_ZERO = 1, // Intel Core Ultra (Xe / Arc iGPU)
  HIP        = 2  // AMD Ryzen (RDNA3 / RDNA3.5 iGPU)
};

// ============================================================================
// Intel Level Zero iGPU Card Table Scanner (PLAN.md Section 13.2)
// ============================================================================

class RedjetLevelZeroScanner {
private:
  void* _driver_handle;
  void* _device_handle;
  void* _context_handle;
  void* _cmd_queue_handle;
  void* _cmd_list_handle;
  void* _scan_kernel_handle;
  bool  _is_initialized;

public:
  RedjetLevelZeroScanner();
  ~RedjetLevelZeroScanner();

  RJ_API bool initialize();
  RJ_API bool scan_card_table_async(void* card_table_base, size_t card_table_size,
                                    uint32_t* out_dirty_indices, uint32_t* out_dirty_count);
  RJ_API bool is_available() const { return _is_initialized; }
};

// ============================================================================
// AMD HIP iGPU Card Table Scanner (PLAN.md Section 13.3)
// ============================================================================

class RedjetHIPScanner {
private:
  void* _hip_stream;
  void* _device_ptr;
  bool  _is_initialized;

public:
  RedjetHIPScanner();
  ~RedjetHIPScanner();

  RJ_API bool initialize();
  RJ_API bool scan_card_table_async(const uint64_t* card_table_u64, size_t num_u64_words,
                                    uint32_t* out_dirty_indices, uint32_t* out_dirty_count);
  RJ_API bool is_available() const { return _is_initialized; }
};

// ============================================================================
// Unified Redjet GPU Offload Coordinator (PLAN.md Section 13.1 & 13.4)
// ============================================================================

class RedjetGPUOffloadCoordinator {
private:
  static RedjetGPUBackend       _active_backend;
  static RedjetLevelZeroScanner _level_zero_scanner;
  static RedjetHIPScanner       _hip_scanner;
  static std::atomic<size_t>    _total_gpu_scans;
  static std::atomic<size_t>    _total_gpu_dirty_cards_found;

public:
  RJ_API static void initialize(const char* backend_name);
  RJ_API static bool should_offload_card_scan(size_t card_table_size_bytes);
  RJ_API static bool scan_card_table(void* card_table_base, size_t card_table_size_bytes,
                                     uint32_t* out_dirty_indices, uint32_t* out_dirty_count);
  RJ_API static RedjetGPUBackend active_backend() { return _active_backend; }
  RJ_API static size_t total_scans() { return _total_gpu_scans.load(std::memory_order_relaxed); }
  RJ_API static size_t total_dirty_cards() { return _total_gpu_dirty_cards_found.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_SIMD_REDJET_GPU_OFFLOAD_HPP
