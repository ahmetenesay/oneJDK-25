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

#include "gc/redjet/vfs/redjetVFSNode_MircCodePage.hpp"
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

RedjetVFSNode_MircCodePage::RedjetVFSNode_MircCodePage(const char* name,
                                                     const char* full_path,
                                                     RedjetVFSNode* parent,
                                                     size_t quota_bytes)
  : RedjetVFSNode(name, full_path, parent, quota_bytes),
    _page_count(0),
    _node_lvl1(nullptr),
    _node_lvl2(nullptr),
    _mirc_read_key(0),
    _total_pages_created(0),
    _total_wx_executable_promotions(0),
    _total_slot16_trampoline_swaps(0),
    _total_pages_deoptimized(0) {
  hs_ticket_lock_init(&_mirc_lock);
  std::memset(_pages, 0, sizeof(_pages));

  _node_lvl1 = add_child("lvl1", "/heap/off_heap/code_boot/mirc_execution_ready_page/lvl1", quota_bytes / 2);
  _node_lvl2 = add_child("lvl2", "/heap/off_heap/code_boot/mirc_execution_ready_page/lvl2", quota_bytes / 2);
}

RedjetVFSNode_MircCodePage::~RedjetVFSNode_MircCodePage() {
  destroy();
}

void RedjetVFSNode_MircCodePage::init(uint64_t mirc_read_key) {
  hs_ticket_lock_acquire(&_mirc_lock);
  _mirc_read_key = (mirc_read_key != 0) ? mirc_read_key : 0xB18C4D9F025AE734ULL;
  _page_count = 0;
  std::memset(_pages, 0, sizeof(_pages));
  set_used_bytes(0);
  hs_ticket_lock_release(&_mirc_lock);
}

void RedjetVFSNode_MircCodePage::destroy() {
  hs_ticket_lock_acquire(&_mirc_lock);

  for (size_t i = 0; i < _page_count; ++i) {
    if (_pages[i].code_address != nullptr) {
      if (_pages[i].slot16_trampoline_addr != nullptr) {
        // Rollback trampoline before freeing
        rollback_slot16_trampoline(&_pages[i]);
      }

#if defined(_WIN32)
      VirtualFree(_pages[i].code_address, 0, MEM_RELEASE);
#else
      munmap(_pages[i].code_address, _pages[i].page_size);
#endif
      _pages[i].code_address = nullptr;
    }
  }

  _page_count = 0;
  set_used_bytes(0);
  hs_ticket_lock_release(&_mirc_lock);
}

RedjetMircPageEntry* RedjetVFSNode_MircCodePage::allocate_code_page(uint32_t method_id,
                                                                   uint8_t opt_level,
                                                                   size_t code_size) {
  if (code_size == 0) {
    return nullptr;
  }

  // 4KB page alignment
  size_t page_aligned = (code_size + 4095) & ~static_cast<size_t>(4095);

  hs_ticket_lock_acquire(&_mirc_lock);

  if (_page_count >= MAX_MIRC_PAGES) {
    hs_ticket_lock_release(&_mirc_lock);
    return nullptr;
  }

  if (used_bytes() + page_aligned > quota_bytes() && quota_bytes() > 0) {
    hs_ticket_lock_release(&_mirc_lock);
    return nullptr;
  }

  void* mem = nullptr;
#if defined(_WIN32)
  mem = VirtualAlloc(nullptr, page_aligned, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  mem = mmap(nullptr, page_aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    mem = nullptr;
  }
#endif

  if (mem == nullptr) {
    hs_ticket_lock_release(&_mirc_lock);
    return nullptr;
  }

  RedjetMircPageEntry* entry = &_pages[_page_count++];
  entry->code_address = mem;
  entry->page_size = page_aligned;
  entry->code_bytes = code_size;
  entry->method_id = method_id;
  entry->opt_level = opt_level;
  entry->wx_state = RedjetWXState::WX_WRITABLE;
  entry->crypto_seal = 0;
  entry->entry_tsc = hs_read_tsc();
  entry->slot16_trampoline_addr = nullptr;
  std::memset(entry->saved_trampoline_bytes, 0, sizeof(entry->saved_trampoline_bytes));

  _total_pages_created.fetch_add(1, std::memory_order_relaxed);
  set_used_bytes(used_bytes() + page_aligned);

  if (opt_level == 1 && _node_lvl1 != nullptr) {
    _node_lvl1->set_used_bytes(_node_lvl1->used_bytes() + page_aligned);
  } else if (opt_level == 2 && _node_lvl2 != nullptr) {
    _node_lvl2->set_used_bytes(_node_lvl2->used_bytes() + page_aligned);
  }

  hs_ticket_lock_release(&_mirc_lock);
  return entry;
}

bool RedjetVFSNode_MircCodePage::commit_and_make_executable(RedjetMircPageEntry* entry, uint64_t auth_key) {
  if (entry == nullptr || entry->code_address == nullptr) {
    return false;
  }

  hs_ticket_lock_acquire(&_mirc_lock);

  uint64_t effective_key = (auth_key != 0) ? auth_key : _mirc_read_key;

  // Compute seal
  uint64_t digest = 0xD4C3B2A1E5F60718ULL;
  digest = RedjetVFSCrypto::poly_eval_step(digest, entry->method_id);
  digest = RedjetVFSCrypto::poly_eval_step(digest, entry->opt_level);
  digest = RedjetVFSCrypto::poly_eval_step(digest, entry->code_bytes);
  digest = RedjetVFSCrypto::poly_eval_step(digest, effective_key);

  const uint8_t* code_bytes = reinterpret_cast<const uint8_t*>(entry->code_address);
  for (size_t i = 0; i < entry->code_bytes; ++i) {
    digest = (digest * 6364136223846793005ULL) + code_bytes[i];
  }

  entry->crypto_seal = digest;

  // Transition W^X to PAGE_EXECUTE_READ
#if defined(_WIN32)
  DWORD old_protect;
  if (!VirtualProtect(entry->code_address, entry->page_size, PAGE_EXECUTE_READ, &old_protect)) {
    hs_ticket_lock_release(&_mirc_lock);
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), entry->code_address, entry->page_size);
#else
  if (mprotect(entry->code_address, entry->page_size, PROT_READ | PROT_EXEC) != 0) {
    hs_ticket_lock_release(&_mirc_lock);
    return false;
  }
#if defined(__GNUC__) || defined(__clang__)
  char* start = reinterpret_cast<char*>(entry->code_address);
  char* end = start + entry->page_size;
  __builtin___clear_cache(start, end);
#endif
#endif

  entry->wx_state = RedjetWXState::WX_EXECUTABLE;
  _total_wx_executable_promotions.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_mirc_lock);
  return true;
}

bool RedjetVFSNode_MircCodePage::patch_slot16_trampoline(RedjetMircPageEntry* entry, void* trampoline_addr) {
  if (entry == nullptr || entry->code_address == nullptr || trampoline_addr == nullptr) {
    return false;
  }

  if (entry->wx_state != RedjetWXState::WX_EXECUTABLE) {
    return false; // Must be sealed executable before hot-swap
  }

  hs_ticket_lock_acquire(&_mirc_lock);

  entry->slot16_trampoline_addr = trampoline_addr;
  // 1. Save original 16 bytes
  std::memcpy(entry->saved_trampoline_bytes, trampoline_addr, 16);

  // 2. Format 16-byte trampoline jump:
  // x86-64:
  // 48 B8 [64-bit target address]   -> movabs rax, <addr>  (10 bytes)
  // FF E0                           -> jmp rax             (2 bytes)
  // 90 90 90 90                     -> nop nop nop nop     (4 bytes) = 16 bytes total
  uint8_t patch[16];
  patch[0] = 0x48;
  patch[1] = 0xB8;
  uint64_t target = reinterpret_cast<uint64_t>(entry->code_address);
  std::memcpy(&patch[2], &target, 8);
  patch[10] = 0xFF;
  patch[11] = 0xE0;
  patch[12] = 0x90;
  patch[13] = 0x90;
  patch[14] = 0x90;
  patch[15] = 0x90;

  // 3. Atomically patch trampoline with memory protection toggle
#if defined(_WIN32)
  DWORD old_protect;
  if (VirtualProtect(trampoline_addr, 16, PAGE_EXECUTE_READWRITE, &old_protect)) {
    std::memcpy(trampoline_addr, patch, 16);
    VirtualProtect(trampoline_addr, 16, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), trampoline_addr, 16);
  } else {
    hs_ticket_lock_release(&_mirc_lock);
    return false;
  }
#else
  long page_sz = sysconf(_SC_PAGESIZE);
  uintptr_t addr = reinterpret_cast<uintptr_t>(trampoline_addr);
  uintptr_t base = addr & ~(page_sz - 1);
  if (mprotect(reinterpret_cast<void*>(base), page_sz, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
    std::memcpy(trampoline_addr, patch, 16);
    mprotect(reinterpret_cast<void*>(base), page_sz, PROT_READ | PROT_EXEC);
#if defined(__GNUC__) || defined(__clang__)
    char* start = reinterpret_cast<char*>(trampoline_addr);
    __builtin___clear_cache(start, start + 16);
#endif
  } else {
    hs_ticket_lock_release(&_mirc_lock);
    return false;
  }
#endif

  _total_slot16_trampoline_swaps.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_mirc_lock);
  return true;
}

bool RedjetVFSNode_MircCodePage::rollback_slot16_trampoline(RedjetMircPageEntry* entry) {
  if (entry == nullptr || entry->slot16_trampoline_addr == nullptr) {
    return false;
  }

  void* trampoline_addr = entry->slot16_trampoline_addr;

#if defined(_WIN32)
  DWORD old_protect;
  if (VirtualProtect(trampoline_addr, 16, PAGE_EXECUTE_READWRITE, &old_protect)) {
    std::memcpy(trampoline_addr, entry->saved_trampoline_bytes, 16);
    VirtualProtect(trampoline_addr, 16, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), trampoline_addr, 16);
  } else {
    return false;
  }
#else
  long page_sz = sysconf(_SC_PAGESIZE);
  uintptr_t addr = reinterpret_cast<uintptr_t>(trampoline_addr);
  uintptr_t base = addr & ~(page_sz - 1);
  if (mprotect(reinterpret_cast<void*>(base), page_sz, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
    std::memcpy(trampoline_addr, entry->saved_trampoline_bytes, 16);
    mprotect(reinterpret_cast<void*>(base), page_sz, PROT_READ | PROT_EXEC);
#if defined(__GNUC__) || defined(__clang__)
    char* start = reinterpret_cast<char*>(trampoline_addr);
    __builtin___clear_cache(start, start + 16);
#endif
  } else {
    return false;
  }
#endif

  entry->slot16_trampoline_addr = nullptr;
  _total_pages_deoptimized.fetch_add(1, std::memory_order_relaxed);
  return true;
}

RedjetMircPageEntry* RedjetVFSNode_MircCodePage::find_page(uint32_t method_id, uint8_t opt_level) noexcept {
  hs_ticket_lock_acquire(&_mirc_lock);

  for (size_t i = 0; i < _page_count; ++i) {
    if (_pages[i].method_id == method_id &&
        (_pages[i].opt_level == opt_level || opt_level == 0)) {
      RedjetMircPageEntry* found = &_pages[i];
      hs_ticket_lock_release(&_mirc_lock);
      return found;
    }
  }

  hs_ticket_lock_release(&_mirc_lock);
  return nullptr;
}

void RedjetVFSNode_MircCodePage::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet VFS Node: MIRC Execution Code Page (/heap/off_heap/code_boot/mirc_execution_ready_page) ---");
  st->print_cr("  Tracked Pages:       %zu / %zu", _page_count, MAX_MIRC_PAGES);
  st->print_cr("  Used Bytes:          %zu bytes (Quota: %zu bytes)", used_bytes(), quota_bytes());
  st->print_cr("  Pages Created:       %llu", static_cast<unsigned long long>(_total_pages_created.load(std::memory_order_relaxed)));
  st->print_cr("  WX Promotions:       %llu", static_cast<unsigned long long>(_total_wx_executable_promotions.load(std::memory_order_relaxed)));
  st->print_cr("  Slot 16 Swaps:       %llu", static_cast<unsigned long long>(_total_slot16_trampoline_swaps.load(std::memory_order_relaxed)));
  st->print_cr("  Deoptimizations:     %llu", static_cast<unsigned long long>(_total_pages_deoptimized.load(std::memory_order_relaxed)));
}
