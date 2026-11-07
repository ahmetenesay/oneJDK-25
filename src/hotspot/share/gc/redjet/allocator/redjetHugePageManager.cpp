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

#include "gc/redjet/allocator/redjetHugePageManager.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetHugePageManager* RedjetHugePageManager::_instance = nullptr;
std::atomic<bool> RedjetHugePageManager::_initialized{ false };

RedjetHugePageManager::RedjetHugePageManager()
  : _page_count(0),
    _large_pages_supported(false),
    _total_2m_allocated(0),
    _total_1g_allocated(0),
    _total_fallback_4k(0),
    _total_bytes_managed(0) {
  hs_ticket_lock_init(&_page_lock);
  std::memset(_pages, 0, sizeof(_pages));

  _large_pages_supported = acquire_large_page_privilege();
}

RedjetHugePageManager::~RedjetHugePageManager() {
  hs_ticket_lock_acquire(&_page_lock);
  for (size_t i = 0; i < _page_count; ++i) {
    if (_pages[i].address != nullptr) {
#if defined(_WIN32)
      VirtualFree(_pages[i].address, 0, MEM_RELEASE);
#else
      munmap(_pages[i].address, _pages[i].size_bytes);
#endif
      _pages[i].address = nullptr;
    }
  }
  _page_count = 0;
  hs_ticket_lock_release(&_page_lock);
}

void RedjetHugePageManager::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetHugePageManager();
  }
}

void RedjetHugePageManager::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

bool RedjetHugePageManager::acquire_large_page_privilege() {
#if defined(_WIN32)
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return false;
  }

  TOKEN_PRIVILEGES tp;
  LUID luid;
  if (!LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid)) {
    CloseHandle(token);
    return false;
  }

  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  BOOL res = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
  DWORD err = GetLastError();
  CloseHandle(token);

  if (!res || err == ERROR_NOT_ALL_ASSIGNED) {
    return false;
  }

  size_t min_large_page = GetLargePageMinimum();
  return (min_large_page > 0);
#else
  return true; // Linux supports MAP_HUGETLB if configured in kernel
#endif
}

void* RedjetHugePageManager::allocate_2m_huge_page(uint32_t numa_node) {
  hs_ticket_lock_acquire(&_page_lock);

  if (_page_count >= MAX_TRACKED_PAGES) {
    hs_ticket_lock_release(&_page_lock);
    return nullptr;
  }

  void* addr = nullptr;
  RedjetHugePageType page_type = RedjetHugePageType::HUGE_2M;

#if defined(_WIN32)
  if (_large_pages_supported) {
    addr = VirtualAlloc(nullptr, HUGE_PAGE_2M_SIZE, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
  }
  if (addr == nullptr) {
    // Fallback to standard 2MB aligned virtual allocation
    addr = VirtualAlloc(nullptr, HUGE_PAGE_2M_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    page_type = RedjetHugePageType::STANDARD_4K;
    _total_fallback_4k.fetch_add(1, std::memory_order_relaxed);
  } else {
    _total_2m_allocated.fetch_add(1, std::memory_order_relaxed);
  }
#else
  addr = mmap(nullptr, HUGE_PAGE_2M_SIZE, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (addr == MAP_FAILED) {
    addr = mmap(nullptr, HUGE_PAGE_2M_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr != MAP_FAILED) {
      madvise(addr, HUGE_PAGE_2M_SIZE, MADV_HUGEPAGE);
    } else {
      addr = nullptr;
    }
    page_type = RedjetHugePageType::STANDARD_4K;
    _total_fallback_4k.fetch_add(1, std::memory_order_relaxed);
  } else {
    _total_2m_allocated.fetch_add(1, std::memory_order_relaxed);
  }
#endif

  if (addr != nullptr) {
    RedjetHugePageRecord* rec = &_pages[_page_count++];
    rec->address = addr;
    rec->size_bytes = HUGE_PAGE_2M_SIZE;
    rec->page_type = page_type;
    rec->numa_node = numa_node;
    rec->is_locked = (page_type == RedjetHugePageType::HUGE_2M);

    _total_bytes_managed.fetch_add(HUGE_PAGE_2M_SIZE, std::memory_order_relaxed);
  }

  hs_ticket_lock_release(&_page_lock);
  return addr;
}

void* RedjetHugePageManager::allocate_1g_gigantic_page(uint32_t numa_node) {
  hs_ticket_lock_acquire(&_page_lock);

  if (_page_count >= MAX_TRACKED_PAGES) {
    hs_ticket_lock_release(&_page_lock);
    return nullptr;
  }

  void* addr = nullptr;
  RedjetHugePageType page_type = RedjetHugePageType::GIGANTIC_1G;

#if defined(_WIN32)
  addr = VirtualAlloc(nullptr, GIGANTIC_1G_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  page_type = RedjetHugePageType::STANDARD_4K;
  _total_fallback_4k.fetch_add(1, std::memory_order_relaxed);
#else
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << 26)
#endif
  addr = mmap(nullptr, GIGANTIC_1G_SIZE, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
  if (addr == MAP_FAILED) {
    addr = mmap(nullptr, GIGANTIC_1G_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr != MAP_FAILED) {
      madvise(addr, GIGANTIC_1G_SIZE, MADV_HUGEPAGE);
    } else {
      addr = nullptr;
    }
    page_type = RedjetHugePageType::STANDARD_4K;
    _total_fallback_4k.fetch_add(1, std::memory_order_relaxed);
  } else {
    _total_1g_allocated.fetch_add(1, std::memory_order_relaxed);
  }
#endif

  if (addr != nullptr) {
    RedjetHugePageRecord* rec = &_pages[_page_count++];
    rec->address = addr;
    rec->size_bytes = GIGANTIC_1G_SIZE;
    rec->page_type = page_type;
    rec->numa_node = numa_node;
    rec->is_locked = (page_type == RedjetHugePageType::GIGANTIC_1G);

    _total_bytes_managed.fetch_add(GIGANTIC_1G_SIZE, std::memory_order_relaxed);
  }

  hs_ticket_lock_release(&_page_lock);
  return addr;
}

bool RedjetHugePageManager::free_huge_page(void* addr) {
  if (addr == nullptr) return false;

  hs_ticket_lock_acquire(&_page_lock);

  size_t target_idx = MAX_TRACKED_PAGES;
  for (size_t i = 0; i < _page_count; ++i) {
    if (_pages[i].address == addr) {
      target_idx = i;
      break;
    }
  }

  if (target_idx == MAX_TRACKED_PAGES) {
    hs_ticket_lock_release(&_page_lock);
    return false;
  }

  size_t bytes = _pages[target_idx].size_bytes;

#if defined(_WIN32)
  VirtualFree(addr, 0, MEM_RELEASE);
#else
  munmap(addr, bytes);
#endif

  for (size_t i = target_idx; i + 1 < _page_count; ++i) {
    _pages[i] = _pages[i + 1];
  }
  _pages[--_page_count] = RedjetHugePageRecord();

  if (_total_bytes_managed.load(std::memory_order_relaxed) >= bytes) {
    _total_bytes_managed.fetch_sub(bytes, std::memory_order_relaxed);
  }

  hs_ticket_lock_release(&_page_lock);
  return true;
}

bool RedjetHugePageManager::advise_huge_page(void* addr, size_t size) {
  if (addr == nullptr || size == 0) return false;
#if !defined(_WIN32)
  return (madvise(addr, size, MADV_HUGEPAGE) == 0);
#else
  return true;
#endif
}

void RedjetHugePageManager::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Huge Page & Gigantic Page Manager ---");
  st->print_cr("  Large Pages Supported: %s", _large_pages_supported ? "YES (Hardware Backed)" : "NO (Emulated via 4K Pages)");
  st->print_cr("  Tracked Huge Pages:    %zu / %zu", _page_count, MAX_TRACKED_PAGES);
  st->print_cr("  Total 2MB Pages:       %llu", static_cast<unsigned long long>(_total_2m_allocated.load(std::memory_order_relaxed)));
  st->print_cr("  Total 1GB Pages:       %llu", static_cast<unsigned long long>(_total_1g_allocated.load(std::memory_order_relaxed)));
  st->print_cr("  Fallback 4KB Pages:    %llu", static_cast<unsigned long long>(_total_fallback_4k.load(std::memory_order_relaxed)));
  st->print_cr("  Total Bytes Managed:   %llu MB", static_cast<unsigned long long>(_total_bytes_managed.load(std::memory_order_relaxed) / (1024 * 1024)));
}
