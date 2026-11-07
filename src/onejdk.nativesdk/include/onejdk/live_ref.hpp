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

#ifndef ONEJDK_NATIVE_SDK_LIVE_REF_HPP
#define ONEJDK_NATIVE_SDK_LIVE_REF_HPP

#include "onejdk/onejdk_native.h"
#include <atomic>
#include <utility>
#include <type_traits>
#include <cassert>

namespace onejdk {

// ============================================================================
// onejdk::live_ref<T> (PLAN.md Section 10.6)
//
// Modern C++20 Smart Pointer for Zero-Copy, Bidirectionally Tracked Java References.
// When Redjet GC relocates an object during compaction or promotion, the native pointer
// is updated atomically in-place via the thread's registered tracking slot.
//
// Usage:
//   void native_compute(JNIEnv* env, jobject obj) {
//       onejdk::live_ref<MyDataStruct> ref(env, obj);
//       ref->compute_heavy(); // Always accesses the current physical address!
//   }
// ============================================================================

template <typename T>
class live_ref {
public:
  using element_type = T;

private:
  T*       volatile _ptr{ nullptr };
  uint64_t          _tracker_id{ 0 };
  bool              _is_pinned{ false };

  void register_tracking() noexcept {
    if (_ptr != nullptr) {
      _tracker_id = onejdk_register_native_ref(reinterpret_cast<void* volatile*>(&_ptr), _ptr);
    }
  }

  void unregister_tracking() noexcept {
    if (_is_pinned && _ptr != nullptr) {
      onejdk_unpin_live_ref(_ptr);
      _is_pinned = false;
    }
    if (_tracker_id != 0) {
      onejdk_release_native_ref(_tracker_id);
      _tracker_id = 0;
    }
    _ptr = nullptr;
  }

public:
  constexpr live_ref() noexcept = default;

  constexpr explicit live_ref(std::nullptr_t) noexcept : _ptr(nullptr), _tracker_id(0), _is_pinned(false) {}

  explicit live_ref(T* raw_pointer) noexcept : _ptr(raw_pointer), _tracker_id(0), _is_pinned(false) {
    register_tracking();
  }

  // Generic constructor taking raw pointer or object handle
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  explicit live_ref(U* raw_pointer) noexcept : _ptr(static_cast<T*>(raw_pointer)), _tracker_id(0), _is_pinned(false) {
    register_tracking();
  }

  ~live_ref() noexcept {
    unregister_tracking();
  }

  // Copy semantics: registers a new distinct slot in the tracker
  live_ref(const live_ref& other) noexcept : _ptr(other._ptr), _tracker_id(0), _is_pinned(false) {
    register_tracking();
    if (other._is_pinned) {
      pin();
    }
  }

  live_ref& operator=(const live_ref& other) noexcept {
    if (this != &other) {
      unregister_tracking();
      _ptr = other._ptr;
      register_tracking();
      if (other._is_pinned) {
        pin();
      }
    }
    return *this;
  }

  // Move semantics: transfers ownership of tracker registration
  live_ref(live_ref&& other) noexcept
    : _ptr(other._ptr), _tracker_id(other._tracker_id), _is_pinned(other._is_pinned) {
    other._ptr = nullptr;
    other._tracker_id = 0;
    other._is_pinned = false;
    // Update the slot address registered with the tracker to point to our new instance
    if (_ptr != nullptr) {
      onejdk_release_native_ref(_tracker_id);
      _tracker_id = onejdk_register_native_ref(reinterpret_cast<void* volatile*>(&_ptr), _ptr);
    }
  }

  live_ref& operator=(live_ref&& other) noexcept {
    if (this != &other) {
      unregister_tracking();
      _ptr = other._ptr;
      _tracker_id = other._tracker_id;
      _is_pinned = other._is_pinned;
      other._ptr = nullptr;
      other._tracker_id = 0;
      other._is_pinned = false;
      if (_ptr != nullptr) {
        onejdk_release_native_ref(_tracker_id);
        _tracker_id = onejdk_register_native_ref(reinterpret_cast<void* volatile*>(&_ptr), _ptr);
      }
    }
    return *this;
  }

  // Direct dereference operator with zero indirection overhead
  [[nodiscard]] T* operator->() const noexcept {
    return _ptr;
  }

  [[nodiscard]] T& operator*() const noexcept {
    return *_ptr;
  }

  [[nodiscard]] T* get() const noexcept {
    return _ptr;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return _ptr != nullptr;
  }

  [[nodiscard]] uint64_t tracker_id() const noexcept {
    return _tracker_id;
  }

  [[nodiscard]] bool is_pinned() const noexcept {
    return _is_pinned;
  }

  // Logical pinning: binds object to /heap/on_heap/park_arena (prevents evacuation)
  bool pin() noexcept {
    if (_ptr != nullptr && !_is_pinned) {
      if (onejdk_pin_live_ref(_ptr)) {
        _is_pinned = true;
        return true;
      }
    }
    return false;
  }

  // Unpinning: restores normal GC mobility
  bool unpin() noexcept {
    if (_ptr != nullptr && _is_pinned) {
      if (onejdk_unpin_live_ref(_ptr)) {
        _is_pinned = false;
        return true;
      }
    }
    return false;
  }

  // Reset to new target pointer
  void reset(T* new_ptr = nullptr) noexcept {
    unregister_tracking();
    _ptr = new_ptr;
    register_tracking();
  }

  // Swap support
  void swap(live_ref& other) noexcept {
    std::swap(_ptr, other._ptr);
    std::swap(_tracker_id, other._tracker_id);
    std::swap(_is_pinned, other._is_pinned);
    // Re-register slots to match memory locations
    if (_ptr != nullptr) {
      onejdk_release_native_ref(_tracker_id);
      _tracker_id = onejdk_register_native_ref(reinterpret_cast<void* volatile*>(&_ptr), _ptr);
    }
    if (other._ptr != nullptr) {
      onejdk_release_native_ref(other._tracker_id);
      other._tracker_id = onejdk_register_native_ref(reinterpret_cast<void* volatile*>(&other._ptr), other._ptr);
    }
  }
};

template <typename T>
void swap(live_ref<T>& a, live_ref<T>& b) noexcept {
  a.swap(b);
}

template <typename T, typename U>
bool operator==(const live_ref<T>& a, const live_ref<U>& b) noexcept {
  return a.get() == b.get();
}

template <typename T, typename U>
bool operator!=(const live_ref<T>& a, const live_ref<U>& b) noexcept {
  return a.get() != b.get();
}

template <typename T>
bool operator==(const live_ref<T>& a, std::nullptr_t) noexcept {
  return a.get() == nullptr;
}

template <typename T>
bool operator==(std::nullptr_t, const live_ref<T>& a) noexcept {
  return a.get() == nullptr;
}

template <typename T>
bool operator!=(const live_ref<T>& a, std::nullptr_t) noexcept {
  return a.get() != nullptr;
}

template <typename T>
bool operator!=(std::nullptr_t, const live_ref<T>& a) noexcept {
  return a.get() != nullptr;
}

} // namespace onejdk

#endif // ONEJDK_NATIVE_SDK_LIVE_REF_HPP
