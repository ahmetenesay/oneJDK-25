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

#ifndef ONEJDK_NATIVE_SDK_ONEJDK_NATIVE_H
#define ONEJDK_NATIVE_SDK_ONEJDK_NATIVE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ONEJDK_API
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(ONEJDK_STATIC_BUILD)
#define ONEJDK_API
#elif defined(ONEJDK_BUILD_LIBRARY)
#define ONEJDK_API __declspec(dllexport)
#else
#define ONEJDK_API __declspec(dllimport)
#endif
#else
#define ONEJDK_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// oneJDK Native SDK C-Linkage Foundation (PLAN.md Section 10.5 & 10.6)
// Provides high-performance, zero-copy native interaction with Redjet GC:
// - Bidirectional live_ref tracking (Java relocations update native pointers)
// - Reverse native-to-Java updates (Native realloc updates Java MemorySegment)
// - Zero-copy logical pinning in /heap/on_heap/park_arena
// ============================================================================

#define ONEJDK_NATIVE_SDK_VERSION_MAJOR 25
#define ONEJDK_NATIVE_SDK_VERSION_MINOR 1
#define ONEJDK_NATIVE_SDK_MAGIC         0xCAFEF00D20262027ULL

typedef struct onejdk_ref_slot_t {
  void*    volatile* slot_address;
  uintptr_t          original_target;
  uint32_t           thread_id;
  uint32_t           flags;
  uint64_t           registration_epoch;
} onejdk_ref_slot_t;

// Query oneJDK Native SDK version and capabilities
ONEJDK_API uint64_t onejdk_get_version(void);
ONEJDK_API const char* onejdk_get_build_info(void);
ONEJDK_API bool onejdk_is_redjet_gc_active(void);

// Registration of live references with thread-local and global Redjet trackers
ONEJDK_API uint64_t onejdk_register_native_ref(void* volatile* slot_addr, const void* target_obj);
ONEJDK_API void     onejdk_release_native_ref(uint64_t tracker_id);

// Query current live target address for a registered slot (guaranteed atomic)
ONEJDK_API void*    onejdk_get_live_ref_target(uint64_t tracker_id);

// Reverse update: Native side reallocated buffer -> notify Redjet GC to update Java MemorySegment
ONEJDK_API bool     onejdk_update_native_ref(const void* old_native_ptr, void* new_native_ptr, size_t new_size_bytes);

// Logical zero-copy pinning in VFS /heap/on_heap/park_arena
ONEJDK_API bool     onejdk_pin_live_ref(const void* obj_ptr);
ONEJDK_API bool     onejdk_unpin_live_ref(const void* obj_ptr);

// Batch registration of multiple live reference slots for heavy vector loops
ONEJDK_API size_t   onejdk_register_native_ref_batch(const onejdk_ref_slot_t* slots, size_t count, uint64_t* out_tracker_ids);
ONEJDK_API void     onejdk_release_native_ref_batch(const uint64_t* tracker_ids, size_t count);

// Hardware execution hints (P-Core affinity and Turbo frequency hints)
ONEJDK_API void     onejdk_hint_performance_core(bool prioritize_p_core);
ONEJDK_API void     onejdk_hint_enter_critical_compute(void);
ONEJDK_API void     onejdk_hint_exit_critical_compute(void);

#ifdef __cplusplus
}
#endif

#endif // ONEJDK_NATIVE_SDK_ONEJDK_NATIVE_H
