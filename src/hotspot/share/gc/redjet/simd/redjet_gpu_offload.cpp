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
// Intel Level Zero DDI Signatures & Dynamic Resolution
// ============================================================================

typedef int ze_result_t;
#define ZE_RESULT_SUCCESS 0

enum {
  ZE_STRUCTURE_TYPE_CONTEXT_DESC = 0x1,
  ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC = 0x2,
  ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC = 0x3
};

struct ze_context_desc_t {
  uint32_t stype;
  const void* pNext;
  uint32_t flags;
};

struct ze_command_queue_desc_t {
  uint32_t stype;
  const void* pNext;
  uint32_t ordinal;
  uint32_t index;
  uint32_t flags;
  int mode;
  int priority;
};

struct ze_group_count_t {
  uint32_t groupCountX;
  uint32_t groupCountY;
  uint32_t groupCountZ;
};

typedef ze_result_t (*pfn_zeInit)(uint32_t flags);
typedef ze_result_t (*pfn_zeDriverGet)(uint32_t* pCount, void** phDrivers);
typedef ze_result_t (*pfn_zeDeviceGet)(void* hDriver, uint32_t* pCount, void** phDevices);
typedef ze_result_t (*pfn_zeContextCreate)(void* hDriver, const void* desc, void** phContext);
typedef ze_result_t (*pfn_zeContextDestroy)(void* hContext);
typedef ze_result_t (*pfn_zeCommandQueueCreate)(void* hContext, void* hDevice, const void* desc, void** phCommandQueue);
typedef ze_result_t (*pfn_zeCommandQueueDestroy)(void* hCommandQueue);
typedef ze_result_t (*pfn_zeKernelSetArgumentValue)(void* hKernel, uint32_t argIndex, size_t argSize, const void* pArgs);
typedef ze_result_t (*pfn_zeCommandListAppendLaunchKernel)(void* hCommandList, void* hKernel, const ze_group_count_t* pLaunchFuncArgs, void* hSignalEvent, uint32_t numWaitEvents, void** phWaitEvents);

static pfn_zeInit ze_init_fn = nullptr;
static pfn_zeDriverGet ze_driver_get_fn = nullptr;
static pfn_zeDeviceGet ze_device_get_fn = nullptr;
static pfn_zeContextCreate ze_context_create_fn = nullptr;
static pfn_zeContextDestroy ze_context_destroy_fn = nullptr;
static pfn_zeCommandQueueCreate ze_cmd_queue_create_fn = nullptr;
static pfn_zeCommandQueueDestroy ze_cmd_queue_destroy_fn = nullptr;
static pfn_zeKernelSetArgumentValue ze_kernel_set_arg_fn = nullptr;
static pfn_zeCommandListAppendLaunchKernel ze_cmd_list_launch_fn = nullptr;

// ============================================================================
// AMD HIP Signatures & Dynamic Resolution
// ============================================================================

typedef int hipError_t;
#define HIP_SUCCESS 0

typedef hipError_t (*pfn_hipInit)(unsigned int flags);
typedef hipError_t (*pfn_hipGetDeviceCount)(int* count);
typedef hipError_t (*pfn_hipSetDevice)(int deviceId);
typedef hipError_t (*pfn_hipStreamCreate)(void** pStream);
typedef hipError_t (*pfn_hipStreamDestroy)(void* stream);
typedef hipError_t (*pfn_hipStreamSynchronize)(void* stream);

static pfn_hipInit hip_init_fn = nullptr;
static pfn_hipGetDeviceCount hip_device_count_fn = nullptr;
static pfn_hipSetDevice hip_set_device_fn = nullptr;
static pfn_hipStreamCreate hip_stream_create_fn = nullptr;
static pfn_hipStreamDestroy hip_stream_destroy_fn = nullptr;
static pfn_hipStreamSynchronize hip_stream_sync_fn = nullptr;

// ============================================================================
// Intel Level Zero Scanner Implementation (Dynamic Loading)
// ============================================================================

RedjetLevelZeroScanner::RedjetLevelZeroScanner()
  : _driver_handle(nullptr), _device_handle(nullptr), _context_handle(nullptr),
    _cmd_queue_handle(nullptr), _cmd_list_handle(nullptr), _scan_kernel_handle(nullptr),
    _is_initialized(false) {}

RedjetLevelZeroScanner::~RedjetLevelZeroScanner() {
  if (_cmd_queue_handle != nullptr && ze_cmd_queue_destroy_fn != nullptr) {
    ze_cmd_queue_destroy_fn(_cmd_queue_handle);
    _cmd_queue_handle = nullptr;
  }
  if (_context_handle != nullptr && ze_context_destroy_fn != nullptr) {
    ze_context_destroy_fn(_context_handle);
    _context_handle = nullptr;
  }
  _is_initialized = false;
}

bool RedjetLevelZeroScanner::initialize() {
#if defined(_WIN32)
  HMODULE hMod = LoadLibraryA("ze_loader.dll");
  #define GET_ZE_PROC(name) (pfn_##name)GetProcAddress(hMod, #name)
#else
  void* hMod = dlopen("libze_loader.so.1", RTLD_NOW | RTLD_LOCAL);
  if (hMod == nullptr) hMod = dlopen("libze_loader.so", RTLD_NOW | RTLD_LOCAL);
  #define GET_ZE_PROC(name) (pfn_##name)dlsym(hMod, #name)
#endif

  if (hMod == nullptr) {
    _is_initialized = false;
    return false;
  }

  ze_init_fn = GET_ZE_PROC(zeInit);
  ze_driver_get_fn = GET_ZE_PROC(zeDriverGet);
  ze_device_get_fn = GET_ZE_PROC(zeDeviceGet);
  ze_context_create_fn = GET_ZE_PROC(zeContextCreate);
  ze_context_destroy_fn = GET_ZE_PROC(zeContextDestroy);
  ze_cmd_queue_create_fn = GET_ZE_PROC(zeCommandQueueCreate);
  ze_cmd_queue_destroy_fn = GET_ZE_PROC(zeCommandQueueDestroy);
  ze_kernel_set_arg_fn = GET_ZE_PROC(zeKernelSetArgumentValue);
  ze_cmd_list_launch_fn = GET_ZE_PROC(zeCommandListAppendLaunchKernel);
#undef GET_ZE_PROC

  if (ze_init_fn == nullptr || ze_driver_get_fn == nullptr || ze_device_get_fn == nullptr ||
      ze_context_create_fn == nullptr || ze_cmd_queue_create_fn == nullptr) {
    _is_initialized = false;
    return false;
  }

  if (ze_init_fn(0) != ZE_RESULT_SUCCESS) {
    _is_initialized = false;
    return false;
  }

  uint32_t driver_count = 1;
  if (ze_driver_get_fn(&driver_count, &_driver_handle) != ZE_RESULT_SUCCESS || _driver_handle == nullptr) {
    _is_initialized = false;
    return false;
  }

  uint32_t device_count = 1;
  if (ze_device_get_fn(_driver_handle, &device_count, &_device_handle) != ZE_RESULT_SUCCESS || _device_handle == nullptr) {
    _is_initialized = false;
    return false;
  }

  ze_context_desc_t cdesc = { ZE_STRUCTURE_TYPE_CONTEXT_DESC, nullptr, 0 };
  if (ze_context_create_fn(_driver_handle, &cdesc, &_context_handle) != ZE_RESULT_SUCCESS) {
    _is_initialized = false;
    return false;
  }

  ze_command_queue_desc_t qdesc = {
    ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC, nullptr, 0, 0, 0,
    0 /* ASYNC */, 2 /* HIGH PRIORITY */
  };
  ze_cmd_queue_create_fn(_context_handle, _device_handle, &qdesc, &_cmd_queue_handle);

  _is_initialized = true;
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0, 0, "LevelZero:InitializedWithHardwareQueue");
  return true;
}

bool RedjetLevelZeroScanner::scan_card_table_async(void* card_table_base, size_t card_table_size,
                                                  uint32_t* out_dirty_indices, uint32_t* out_dirty_count) {
  if (!_is_initialized || card_table_base == nullptr || out_dirty_indices == nullptr || out_dirty_count == nullptr) {
    return false;
  }

  // If JIT scan kernel is compiled, launch via hardware command list
  if (_scan_kernel_handle != nullptr && ze_kernel_set_arg_fn != nullptr && _cmd_list_handle != nullptr) {
    ze_kernel_set_arg_fn(_scan_kernel_handle, 0, sizeof(void*), &card_table_base);
    ze_kernel_set_arg_fn(_scan_kernel_handle, 1, sizeof(size_t), &card_table_size);
    ze_kernel_set_arg_fn(_scan_kernel_handle, 2, sizeof(void*), &out_dirty_indices);
    ze_kernel_set_arg_fn(_scan_kernel_handle, 3, sizeof(void*), &out_dirty_count);
    ze_group_count_t dispatch = { (uint32_t)(card_table_size / (256 * 8)), 1, 1 };
    ze_cmd_list_launch_fn(_cmd_list_handle, _scan_kernel_handle, &dispatch, nullptr, 0, nullptr);
    return true;
  }

  // Branchless CPU USM scan loop (shared physical memory bus)
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
  if (_hip_stream != nullptr && hip_stream_destroy_fn != nullptr) {
    hip_stream_destroy_fn(_hip_stream);
    _hip_stream = nullptr;
  }
  _is_initialized = false;
}

bool RedjetHIPScanner::initialize() {
#if defined(_WIN32)
  HMODULE hMod = LoadLibraryA("amdhip64.dll");
  #define GET_HIP_PROC(name) (pfn_##name)GetProcAddress(hMod, #name)
#else
  void* hMod = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL);
  #define GET_HIP_PROC(name) (pfn_##name)dlsym(hMod, #name)
#endif

  if (hMod == nullptr) {
    _is_initialized = false;
    return false;
  }

  hip_init_fn = GET_HIP_PROC(hipInit);
  hip_device_count_fn = GET_HIP_PROC(hipGetDeviceCount);
  hip_set_device_fn = GET_HIP_PROC(hipSetDevice);
  hip_stream_create_fn = GET_HIP_PROC(hipStreamCreate);
  hip_stream_destroy_fn = GET_HIP_PROC(hipStreamDestroy);
  hip_stream_sync_fn = GET_HIP_PROC(hipStreamSynchronize);
#undef GET_HIP_PROC

  if (hip_init_fn == nullptr || hip_device_count_fn == nullptr ||
      hip_set_device_fn == nullptr || hip_stream_create_fn == nullptr) {
    _is_initialized = false;
    return false;
  }

  if (hip_init_fn(0) != HIP_SUCCESS) {
    _is_initialized = false;
    return false;
  }

  int count = 0;
  if (hip_device_count_fn(&count) != HIP_SUCCESS || count <= 0) {
    _is_initialized = false;
    return false;
  }

  hip_set_device_fn(0);
  if (hip_stream_create_fn(&_hip_stream) != HIP_SUCCESS) {
    _is_initialized = false;
    return false;
  }

  _is_initialized = true;
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, 0, 0, 0, 0, 0, 0, 0, "AMD_HIP:InitializedWithHardwareStream");
  return true;
}

bool RedjetHIPScanner::scan_card_table_async(const uint64_t* card_table_u64, size_t num_u64_words,
                                            uint32_t* out_dirty_indices, uint32_t* out_dirty_count) {
  if (!_is_initialized || card_table_u64 == nullptr || out_dirty_indices == nullptr || out_dirty_count == nullptr) {
    return false;
  }

  if (_hip_stream != nullptr && hip_stream_sync_fn != nullptr) {
    hip_stream_sync_fn(_hip_stream);
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
