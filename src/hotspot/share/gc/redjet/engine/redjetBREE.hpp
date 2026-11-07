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

#ifndef SHARE_GC_REDJET_ENGINE_REDJETBREE_HPP
#define SHARE_GC_REDJET_ENGINE_REDJETBREE_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetUpclocker.hpp"

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
// BREE (Batch Register and Entangle Execution) Superloop Weaving Engine (PLAN.md Section 8.8.3)
// - 16-Depth LIFO Stack-Tree (tl_hs_batch_stack[16]) for nested @RedjetNestedExec scopes
// - Anti-Premature Reset: Inner batch completion pops stack without resetting P-Core / Turbo
// - Scope Escape & Promotion: Inner objects promoted to parent scope or runway/fledgling
// - Single-cycle batch memory evaporation (top = 0) on scope exit
// - Safepoint suppression coordination (-XX:+FCSEnableBatchScope)
// - Entangle weaving for 4 ALU + 2 Load port saturation
// ============================================================================

static constexpr size_t REDJET_BREE_STACK_CAPACITY = 16; // 16-Depth LIFO Stack
static constexpr size_t REDJET_BREE_RING_CAPACITY   = 64 * 1024; // 64 KB Batch Ring

struct alignas(64) RedjetBREEStackFrame {
  uint32_t            scope_id{ 0 };
  uint32_t            group_hex_id{ 0 };
  uint8_t*            batch_ring_base{ nullptr };
  size_t              watermark_bytes{ 0 };
  uint64_t            entry_tsc{ 0 };
  RedjetGroupPriority inherited_priority{ RedjetGroupPriority::PRIORITY_NORMAL };
  bool                safepoint_suppression_active{ false };
  bool                turbo_pinned{ false };
  bool                morph_enabled{ false };
  uint32_t            prefetch_distance{ 64 };
  size_t              promoted_objects_count{ 0 };
  size_t              evaporated_bytes{ 0 };

  void reset() noexcept {
    scope_id = 0;
    group_hex_id = 0;
    batch_ring_base = nullptr;
    watermark_bytes = 0;
    entry_tsc = 0;
    inherited_priority = RedjetGroupPriority::PRIORITY_NORMAL;
    safepoint_suppression_active = false;
    turbo_pinned = false;
    morph_enabled = false;
    prefetch_distance = 64;
    promoted_objects_count = 0;
    evaporated_bytes = 0;
  }
};

class RedjetBREE : public CHeapObj<mtGC> {
private:
  uint8_t*              _global_ring_storage{ nullptr };
  size_t                _storage_bytes{ 0 };
  hs_ticket_lock_t      _bree_lock;

  // Thread-local emulation registry for multi-threading
  static thread_local RedjetBREEStackFrame tl_hs_batch_stack[REDJET_BREE_STACK_CAPACITY];
  static thread_local size_t               tl_hs_batch_depth;

  // Global telemetry
  std::atomic<uint64_t> _total_batch_scopes_entered{ 0 };
  std::atomic<uint64_t> _total_nested_frames_pushed{ 0 };
  std::atomic<uint64_t> _total_evaporations{ 0 };
  std::atomic<uint64_t> _total_evaporated_bytes{ 0 };
  std::atomic<uint64_t> _total_promotions{ 0 };
  std::atomic<uint64_t> _total_safepoint_suppressions{ 0 };

public:
  RedjetBREE();
  ~RedjetBREE();

  static RedjetBREE& instance();

  void init(void* ring_storage, size_t ring_bytes);
  void destroy();

  // =========================================================================
  // 1. Nested Scope Lifecycle (Push / Pop 16-Depth LIFO)
  // =========================================================================
  RJ_API bool enter_batch_scope(uint32_t scope_id,
                                const char* name,
                                bool morph = false,
                                uint32_t prefetch = 64,
                                RedjetGroupPriority priority = RedjetGroupPriority::PRIORITY_HIGH);

  RJ_API bool exit_batch_scope(uint32_t scope_id);

  // Checks if thread is currently executing inside an active batch scope
  RJ_API static bool is_in_batch_scope() noexcept {
    return tl_hs_batch_depth > 0;
  }

  RJ_API static size_t current_nesting_depth() noexcept {
    return tl_hs_batch_depth;
  }

  // =========================================================================
  // 2. Batch Memory Bump Allocation & Fast Evaporation
  // =========================================================================
  RJ_API void* allocate_in_batch_ring(size_t bytes);

  // Single-cycle evaporation of all memory allocated in the current scope level
  RJ_API size_t evaporate_current_scope();

  // Promotes object out of current batch scope into parent scope or runway
  RJ_API bool promote_object_to_parent(void* obj_ptr, size_t size_bytes);

  // =========================================================================
  // 3. Safepoint Suppression Coordination (-XX:+FCSEnableBatchScope)
  // =========================================================================
  RJ_API static bool is_safepoint_suppressed() noexcept {
    if (tl_hs_batch_depth == 0) return false;
    return tl_hs_batch_stack[tl_hs_batch_depth - 1].safepoint_suppression_active;
  }

  // Telemetry
  uint64_t total_batch_scopes() const noexcept { return _total_batch_scopes_entered.load(std::memory_order_relaxed); }
  uint64_t total_nested_frames() const noexcept { return _total_nested_frames_pushed.load(std::memory_order_relaxed); }
  uint64_t total_evaporations() const noexcept { return _total_evaporations.load(std::memory_order_relaxed); }
  uint64_t total_evaporated_bytes() const noexcept { return _total_evaporated_bytes.load(std::memory_order_relaxed); }
  uint64_t total_promotions() const noexcept { return _total_promotions.load(std::memory_order_relaxed); }
  uint64_t total_safepoint_suppressions() const noexcept { return _total_safepoint_suppressions.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

extern RedjetBREE g_redjet_bree;

// ============================================================================
// RAII RedjetBREEGuard: Manages nested batch scope lifecycle
// ============================================================================
class RedjetBREEGuard {
private:
  uint32_t _scope_id;
  bool     _active;

public:
  RedjetBREEGuard(uint32_t scope_id, const char* name, bool morph = false, uint32_t prefetch = 64)
    : _scope_id(scope_id), _active(false) {
    _active = RedjetBREE::instance().enter_batch_scope(scope_id, name, morph, prefetch);
  }

  ~RedjetBREEGuard() {
    if (_active) {
      RedjetBREE::instance().exit_batch_scope(_scope_id);
    }
  }

  bool is_active() const noexcept { return _active; }
};

#endif // SHARE_GC_REDJET_ENGINE_REDJETBREE_HPP
