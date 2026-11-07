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

#ifndef ONEJDK_NATIVE_SDK_NATIVE_SDK_HPP
#define ONEJDK_NATIVE_SDK_NATIVE_SDK_HPP

// ============================================================================
// oneJDK Consolidated Native SDK (PLAN.md Section 10.5 & 10.6)
//
// Unifies the 6 standard JDK native developer headers with modern oneJDK C++20
// extensions into a single official developer SDK:
// 1. jni.h                 - Accelerated Java Native Interface
// 2. jvmti.h               - JVM Tool Interface
// 3. jawt.h                - AWT Native Interface
// 4. jdwpTransport.h       - Debugger Transport Interface
// 5. classfile_constants.h - Class file bytecode constants
// 6. jvmticmlr.h           - Compiled Method Load Record
// 7. onejdk/onejdk_native.h - C foundation & live_ref tracking APIs
// 8. onejdk/live_ref.hpp    - C++20 zero-copy auto-relocating smart pointer
// ============================================================================

#if defined(__has_include)
#if __has_include("jni.h")
#include "jni.h"
#endif
#if __has_include("jvmti.h")
#include "jvmti.h"
#endif
#if __has_include("jawt.h")
#include "jawt.h"
#endif
#if __has_include("jdwpTransport.h")
#include "jdwpTransport.h"
#endif
#if __has_include("classfile_constants.h")
#include "classfile_constants.h"
#endif
#if __has_include("jvmticmlr.h")
#include "jvmticmlr.h"
#endif
#endif

#include "onejdk/onejdk_native.h"
#include "onejdk/live_ref.hpp"

#include <string_view>
#include <memory>
#include <chrono>

namespace onejdk {

// ============================================================================
// RAII Execution Scope for Critical Low-Latency Native Compute
// Protects the native thread from OS thread migration and requests P-Core frequency boost.
// ============================================================================
class critical_compute_scope {
private:
  bool _active{ false };

public:
  explicit critical_compute_scope(bool boost_p_core = true) noexcept : _active(true) {
    if (boost_p_core) {
      onejdk_hint_performance_core(true);
    }
    onejdk_hint_enter_critical_compute();
  }

  ~critical_compute_scope() noexcept {
    if (_active) {
      onejdk_hint_exit_critical_compute();
      _active = false;
    }
  }

  critical_compute_scope(const critical_compute_scope&) = delete;
  critical_compute_scope& operator=(const critical_compute_scope&) = delete;
};

// ============================================================================
// RAII Scoped Object Pinning Guard (Binds to /heap/on_heap/park_arena)
// ============================================================================
class scoped_pin_guard {
private:
  const void* _target{ nullptr };
  bool        _is_pinned{ false };

public:
  explicit scoped_pin_guard(const void* obj_ptr) noexcept : _target(obj_ptr) {
    if (_target != nullptr) {
      _is_pinned = onejdk_pin_live_ref(_target);
    }
  }

  template <typename T>
  explicit scoped_pin_guard(const live_ref<T>& ref) noexcept : _target(ref.get()) {
    if (_target != nullptr) {
      _is_pinned = onejdk_pin_live_ref(_target);
    }
  }

  ~scoped_pin_guard() noexcept {
    if (_is_pinned && _target != nullptr) {
      onejdk_unpin_live_ref(_target);
      _is_pinned = false;
    }
  }

  [[nodiscard]] bool is_pinned() const noexcept {
    return _is_pinned;
  }

  scoped_pin_guard(const scoped_pin_guard&) = delete;
  scoped_pin_guard& operator=(const scoped_pin_guard&) = delete;
};

// ============================================================================
// Native Memory Segment Synchronizer
// Handles reverse native-to-Java updates (e.g. native reallocations)
// ============================================================================
class memory_segment_bridge {
public:
  static bool reallocate_and_sync(const void* old_buf, void* new_buf, size_t new_size) noexcept {
    return onejdk_update_native_ref(old_buf, new_buf, new_size);
  }
};

} // namespace onejdk

#endif // ONEJDK_NATIVE_SDK_NATIVE_SDK_HPP
