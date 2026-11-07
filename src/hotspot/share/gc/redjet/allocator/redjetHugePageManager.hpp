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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJETHUGEPAGEMANAGER_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJETHUGEPAGEMANAGER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
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
// Redjet Huge Page & Gigantic Page Manager (PLAN.md Section 4.3, 8.2)
// - Native 2MB HugeTLB & 1GB Gigantic Page Allocation
// - Windows SeLockMemoryPrivilege & MEM_LARGE_PAGES support
// - Linux MAP_HUGETLB & madvise(MADV_HUGEPAGE) THP integration
// - Zero TLB-miss high-throughput runway backing
// ============================================================================

enum class RedjetHugePageType : uint8_t {
  STANDARD_4K = 0,
  HUGE_2M     = 1,
  GIGANTIC_1G = 2
};

struct alignas(64) RedjetHugePageRecord {
  void*              address{ nullptr };
  size_t             size_bytes{ 0 };
  RedjetHugePageType page_type{ RedjetHugePageType::HUGE_2M };
  uint32_t           numa_node{ 0 };
  bool               is_locked{ false };
  uint64_t           allocation_tsc{ 0 };
};

class RedjetHugePageManager {
public:
  static constexpr size_t HUGE_PAGE_2M_SIZE = 2ULL * 1024 * 1024;
  static constexpr size_t GIGANTIC_1G_SIZE  = 1024ULL * 1024 * 1024;
  static constexpr size_t MAX_TRACKED_PAGES = 4096;

private:
  static RedjetHugePageManager* _instance;
  static std::atomic<bool>      _initialized;

  RedjetHugePageRecord          _pages[MAX_TRACKED_PAGES];
  size_t                        _page_count{ 0 };
  hs_ticket_lock_t              _page_lock;

  bool                          _large_pages_supported{ false };

  // Telemetry
  std::atomic<uint64_t>         _total_2m_allocated{ 0 };
  std::atomic<uint64_t>         _total_1g_allocated{ 0 };
  std::atomic<uint64_t>         _total_fallback_4k{ 0 };
  std::atomic<uint64_t>         _total_bytes_managed{ 0 };
  std::atomic<uint64_t>         _total_pages_freed{ 0 };

  static bool acquire_large_page_privilege();

public:
  RedjetHugePageManager();
  ~RedjetHugePageManager();

  static void initialize();
  static void destroy();
  static RedjetHugePageManager* get() noexcept { return _instance; }

  // 1. Huge Page Allocation
  RJ_API void* allocate_2m_huge_page(uint32_t numa_node = 0);
  RJ_API void* allocate_1g_gigantic_page(uint32_t numa_node = 0);

  // 2. Huge Page Release
  RJ_API bool free_huge_page(void* addr);

  // 3. Advise OS on Huge Page Backing (Linux MADV_HUGEPAGE)
  RJ_API static bool advise_huge_page(void* addr, size_t size);

  // 4. Advise OS on Page Demotion (Linux MADV_NOHUGEPAGE)
  RJ_API static bool advise_demote_page(void* addr, size_t size);

  // Queries
  bool large_pages_supported() const noexcept { return _large_pages_supported; }
  size_t tracked_page_count()  const noexcept { return _page_count; }

  // Telemetry
  uint64_t pages_2m_allocated() const noexcept { return _total_2m_allocated.load(std::memory_order_relaxed); }
  uint64_t pages_1g_allocated() const noexcept { return _total_1g_allocated.load(std::memory_order_relaxed); }
  uint64_t fallback_4k_count()  const noexcept { return _total_fallback_4k.load(std::memory_order_relaxed); }
  uint64_t bytes_managed()      const noexcept { return _total_bytes_managed.load(std::memory_order_relaxed); }
  uint64_t pages_freed()        const noexcept { return _total_pages_freed.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJETHUGEPAGEMANAGER_HPP
