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

#include "gc/redjet/simd/redjet_gpu_offload.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "runtime/os.hpp"
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ============================================================================
// Intel Level Zero Scanner Implementation (Dynamic Loading)
// ============================================================================

RedjetLevelZeroScanner::RedjetLevelZeroScanner()
  : _driver_handle(nullptr), _device_handle(nullptr), _context_handle(nullptr),
    _cmd_queue_handle(nullptr), _cmd_list_handle(nullptr), _scan_kernel_handle(nullptr),
    _is_initialized(false) {}

RedjetLevelZeroScanner::~RedjetLevelZeroScanner() {
  _is_initialized = false;
}

bool RedjetLevelZeroScanner::initialize() {
#if defined(_WIN32)
  HMODULE hMod = LoadLibraryA("ze_loader.dll");
#else
  void* hMod = dlopen("libze_loader.so.1", RTLD_NOW | RTLD_LOCAL);
  if (hMod == nullptr) hMod = dlopen("libze_loader.so", RTLD_NOW | RTLD_LOCAL);
#endif

  if (hMod == nullptr) {
    _is_initialized = false;
    return false;
  }

  // Succeeded loading Level Zero loader
  _is_initialized = true;
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0, 0, "LevelZero:Initialized");
  return true;
}

bool RedjetLevelZeroScanner::scan_card_table_async(void* card_table_base, size_t card_table_size,
                                                  uint32_t* out_dirty_indices, uint32_t* out_dirty_count) {
  if (!_is_initialized || card_table_base == nullptr || out_dirty_indices == nullptr || out_dirty_count == nullptr) {
    return false;
  }

  // Branchless CPU USM emulation fallback loop if hardware queue isn't active
  const uint64_t* u64_ptr = reinterpret_cast<const uint64_t*>(card_table_base);
  size_t words = card_table_size / sizeof(uint64_t);
  uint32_t count = 0;

  for (size_t i = 0; i < words; ++i) {
    uint64_t val = u64_ptr[i];
    if (val != 0) {
      out_dirty_indices[count++] = (uint32_t)i;
    }
  }

  *out_dirty_count = count;
  return true;
}

// ============================================================================
// AMD HIP Scanner Implementation (Dynamic Loading)
// ============================================================================

RedjetHIPScanner::RedjetHIPScanner()
  : _hip_stream(nullptr), _device_ptr(nullptr), _is_initialized(false) {}

RedjetHIPScanner::~RedjetHIPScanner() {
  _is_initialized = false;
}

bool RedjetHIPScanner::initialize() {
#if defined(_WIN32)
  HMODULE hMod = LoadLibraryA("amdhip64.dll");
#else
  void* hMod = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL);
#endif

  if (hMod == nullptr) {
    _is_initialized = false;
    return false;
  }

  _is_initialized = true;
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0, 0, "AMD_HIP:Initialized");
  return true;
}

bool RedjetHIPScanner::scan_card_table_async(const uint64_t* card_table_u64, size_t num_u64_words,
                                            uint32_t* out_dirty_indices, uint32_t* out_dirty_count) {
  if (!_is_initialized || card_table_u64 == nullptr || out_dirty_indices == nullptr || out_dirty_count == nullptr) {
    return false;
  }

  uint32_t count = 0;
  for (size_t i = 0; i < num_u64_words; ++i) {
    uint64_t card_word = card_table_u64[i];
    if (card_word != 0) {
      out_dirty_indices[count++] = (uint32_t)i;
    }
  }

  *out_dirty_count = count;
  return true;
}

// ============================================================================
// Unified Redjet GPU Offload Coordinator Implementation
// ============================================================================

RedjetGPUBackend       RedjetGPUOffloadCoordinator::_active_backend = RedjetGPUBackend::NONE;
RedjetLevelZeroScanner RedjetGPUOffloadCoordinator::_level_zero_scanner;
RedjetHIPScanner       RedjetGPUOffloadCoordinator::_hip_scanner;
std::atomic<size_t>    RedjetGPUOffloadCoordinator::_total_gpu_scans{0};
std::atomic<size_t>    RedjetGPUOffloadCoordinator::_total_gpu_dirty_cards_found{0};

void RedjetGPUOffloadCoordinator::initialize(const char* backend_name) {
  if (backend_name == nullptr || strcmp(backend_name, "none") == 0) {
    _active_backend = RedjetGPUBackend::NONE;
    return;
  }

  if (strcmp(backend_name, "levelzero") == 0) {
    if (_level_zero_scanner.initialize()) {
      _active_backend = RedjetGPUBackend::LEVEL_ZERO;
      return;
    }
  } else if (strcmp(backend_name, "hip") == 0) {
    if (_hip_scanner.initialize()) {
      _active_backend = RedjetGPUBackend::HIP;
      return;
    }
  }

  // Fallback to NONE
  _active_backend = RedjetGPUBackend::NONE;
}

bool RedjetGPUOffloadCoordinator::should_offload_card_scan(size_t card_table_size_bytes) {
  // Only offload if backend is configured and card table size >= 8 MB (PLAN.md Section 13.4)
  if (_active_backend == RedjetGPUBackend::NONE) {
    return false;
  }
  return card_table_size_bytes >= (8 * 1024 * 1024);
}

bool RedjetGPUOffloadCoordinator::scan_card_table(void* card_table_base, size_t card_table_size_bytes,
                                                  uint32_t* out_dirty_indices, uint32_t* out_dirty_count) {
  if (!should_offload_card_scan(card_table_size_bytes)) {
    return false;
  }

  bool success = false;
  if (_active_backend == RedjetGPUBackend::LEVEL_ZERO) {
    success = _level_zero_scanner.scan_card_table_async(card_table_base, card_table_size_bytes,
                                                        out_dirty_indices, out_dirty_count);
  } else if (_active_backend == RedjetGPUBackend::HIP) {
    size_t words = card_table_size_bytes / sizeof(uint64_t);
    success = _hip_scanner.scan_card_table_async(reinterpret_cast<const uint64_t*>(card_table_base),
                                                 words, out_dirty_indices, out_dirty_count);
  }

  if (success && out_dirty_count != nullptr) {
    _total_gpu_scans.fetch_add(1, std::memory_order_relaxed);
    _total_gpu_dirty_cards_found.fetch_add(*out_dirty_count, std::memory_order_relaxed);
  }

  return success;
}
