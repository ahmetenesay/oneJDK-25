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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETCLASSUNLOADER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETCLASSUNLOADER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#else
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

class RedjetHeap;

// ============================================================================
// 1. CLASS UNLOADING DETAILED METRICS
// ============================================================================
struct RedjetClassUnloadStats {
  size_t   classes_unloaded{ 0 };
  size_t   cld_purged{ 0 };
  size_t   nmethods_unlinked{ 0 };
  size_t   metaspace_reclaimed_bytes{ 0 };
  uint64_t elapsed_us{ 0 };
  size_t   lambda_proxies_unlinked{ 0 };
  size_t   jvmci_unlinked{ 0 };
};

// ============================================================================
// 2. REDJET CLASS UNLOADER (PLAN.md Section 8.10.3)
// Dynamic anonymous class unloading, Lambda form and dynamic proxy unlinking,
// CodeCache / JVMCI unlinking, and Metaspace chunk release to OS / VFS code_boot.
// ============================================================================
class RedjetClassUnloader {
private:
  RedjetHeap*            _heap;
  std::atomic<bool>      _unloading_enabled{ true };

  // Telemetry & metrics
  std::atomic<uint64_t>  _total_unloading_cycles{ 0 };
  std::atomic<uint64_t>  _total_classes_unloaded{ 0 };
  std::atomic<uint64_t>  _total_cld_purged{ 0 };
  std::atomic<uint64_t>  _total_nmethods_unlinked{ 0 };
  std::atomic<uint64_t>  _total_metaspace_reclaimed_bytes{ 0 };
  std::atomic<uint64_t>  _total_unloading_time_us{ 0 };
  std::atomic<uint64_t>  _total_lambda_proxies_unlinked{ 0 };
  std::atomic<uint64_t>  _total_jvmci_unlinked{ 0 };

  RedjetClassUnloadStats _last_stats;

  // Phase implementations
  void   prepare_unloading();
  bool   unlink_class_loader_data();
  void   unlink_code_cache_and_jvmci(bool unloading_occurred);
  void   purge_dead_cld_and_caches();
  size_t release_metaspace_chunks();

public:
  explicit RedjetClassUnloader(RedjetHeap* heap);
  ~RedjetClassUnloader() = default;

  // Unloading lifecycle methods
  void prepare();
  bool unlink();
  void purge();
  void finish();

  // Full composite execution
  bool do_unloading();

  // Configuration & Status
  void set_unloading_enabled(bool enabled) { _unloading_enabled.store(enabled, std::memory_order_release); }
  bool is_unloading_enabled() const noexcept { return _unloading_enabled.load(std::memory_order_relaxed); }

  uint64_t total_unloading_cycles()          const noexcept { return _total_unloading_cycles.load(std::memory_order_relaxed); }
  uint64_t total_classes_unloaded()          const noexcept { return _total_classes_unloaded.load(std::memory_order_relaxed); }
  uint64_t total_cld_purged()                const noexcept { return _total_cld_purged.load(std::memory_order_relaxed); }
  uint64_t total_nmethods_unlinked()         const noexcept { return _total_nmethods_unlinked.load(std::memory_order_relaxed); }
  uint64_t total_metaspace_reclaimed_bytes() const noexcept { return _total_metaspace_reclaimed_bytes.load(std::memory_order_relaxed); }
  uint64_t total_unloading_time_us()         const noexcept { return _total_unloading_time_us.load(std::memory_order_relaxed); }
  uint64_t total_lambda_proxies_unlinked()   const noexcept { return _total_lambda_proxies_unlinked.load(std::memory_order_relaxed); }
  uint64_t total_jvmci_unlinked()            const noexcept { return _total_jvmci_unlinked.load(std::memory_order_relaxed); }

  const RedjetClassUnloadStats& last_stats() const noexcept { return _last_stats; }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETCLASSUNLOADER_HPP
