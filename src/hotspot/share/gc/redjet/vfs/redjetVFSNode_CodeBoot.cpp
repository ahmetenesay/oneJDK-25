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

#include "gc/redjet/vfs/redjetVFSNode_CodeBoot.hpp"
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

void* RedjetVFSNode_CodeBoot::allocate_os_pages(size_t bytes, bool executable) {
  size_t aligned_bytes = (bytes + SYSTEM_PAGE_SIZE - 1) & ~(SYSTEM_PAGE_SIZE - 1);
#if defined(_WIN32)
  DWORD prot = executable ? PAGE_EXECUTE_READ : PAGE_READWRITE;
  return VirtualAlloc(nullptr, aligned_bytes, MEM_COMMIT | MEM_RESERVE, prot);
#else
  int prot = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
  void* ptr = mmap(nullptr, aligned_bytes, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
}

bool RedjetVFSNode_CodeBoot::protect_os_pages(void* addr, size_t bytes, RedjetWXState target_state) {
  if (addr == nullptr || bytes == 0) return false;
  size_t aligned_bytes = (bytes + SYSTEM_PAGE_SIZE - 1) & ~(SYSTEM_PAGE_SIZE - 1);

#if defined(_WIN32)
  DWORD new_prot = PAGE_READWRITE;
  if (target_state == RedjetWXState::WX_EXECUTABLE) {
    new_prot = PAGE_EXECUTE_READ; // W^X: never PAGE_EXECUTE_READWRITE
  } else if (target_state == RedjetWXState::WX_SEALED_RO) {
    new_prot = PAGE_READONLY;
  }
  DWORD old_prot = 0;
  return VirtualProtect(addr, aligned_bytes, new_prot, &old_prot) != 0;
#else
  int prot = PROT_READ | PROT_WRITE;
  if (target_state == RedjetWXState::WX_EXECUTABLE) {
    prot = PROT_READ | PROT_EXEC; // W^X: never PROT_WRITE | PROT_EXEC
  } else if (target_state == RedjetWXState::WX_SEALED_RO) {
    prot = PROT_READ;
  }
  return mprotect(addr, aligned_bytes, prot) == 0;
#endif
}

void RedjetVFSNode_CodeBoot::free_os_pages(void* addr, size_t bytes) {
  if (addr == nullptr || bytes == 0) return;
  size_t aligned_bytes = (bytes + SYSTEM_PAGE_SIZE - 1) & ~(SYSTEM_PAGE_SIZE - 1);
#if defined(_WIN32)
  VirtualFree(addr, 0, MEM_RELEASE);
#else
  munmap(addr, aligned_bytes);
#endif
}

RedjetVFSNode_CodeBoot::RedjetVFSNode_CodeBoot(const char* name,
                                               const char* full_path,
                                               RedjetVFSNode* parent,
                                               size_t quota_bytes) :
  RedjetVFSNode(name, full_path, parent, quota_bytes),
  _pages_head(nullptr) {

  size_t sub_quota = quota_bytes / 4;
  _node_rawcode_cell   = new RedjetVFSNode("rawcode_cell",
                                           "/heap/off_heap/code_boot/rawcode_cell",
                                           this, sub_quota);
  _node_vac_processed  = new RedjetVFSNode("vac_processed_code_chunk",
                                           "/heap/off_heap/code_boot/vac_processed_code_chunk",
                                           this, sub_quota);
  _node_mirc_lvl1      = new RedjetVFSNode("lvl1",
                                           "/heap/off_heap/code_boot/mirc_execution_ready_page/lvl1",
                                           this, sub_quota);
  _node_mirc_lvl2      = new RedjetVFSNode("lvl2",
                                           "/heap/off_heap/code_boot/mirc_execution_ready_page/lvl2",
                                           this, sub_quota);

  _children.append(_node_rawcode_cell);
  _children.append(_node_vac_processed);
  _children.append(_node_mirc_lvl1);
  _children.append(_node_mirc_lvl2);
}

RedjetVFSNode_CodeBoot::~RedjetVFSNode_CodeBoot() {
  _page_lock.lock();
  RedjetCodePageDesc* cur = _pages_head;
  while (cur != nullptr) {
    RedjetCodePageDesc* next = cur->next;
    free_os_pages(cur->page_address, cur->page_size);
    delete cur;
    cur = next;
  }
  _pages_head = nullptr;
  _page_lock.unlock();
}

void* RedjetVFSNode_CodeBoot::alloc_rawcode_cell(size_t bytes, uint32_t method_id, uint64_t vac_key) {
  if (vac_key != RedjetVFSCrypto::KEY_VAC_READ) {
    _total_unauthorized_access_trapped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  size_t aligned_sz = (bytes + SYSTEM_PAGE_SIZE - 1) & ~(SYSTEM_PAGE_SIZE - 1);
  void* mem = allocate_os_pages(aligned_sz, false);
  if (mem == nullptr) return nullptr;

  auto* desc = new RedjetCodePageDesc();
  desc->page_address = mem;
  desc->page_size = aligned_sz;
  desc->used_bytes = bytes;
  desc->tier = RedjetCodePageTier::RAWCODE_CELL;
  desc->wx_state = RedjetWXState::WX_SEALED_RO;
  desc->auth_seal = RedjetVFSCrypto::derive_7cipher_key(RedjetVFSCrypto::DEFAULT_VAC_SECRETS);
  desc->method_id = method_id;
  desc->alloc_tsc = sacculus_rdtsc();

  // Seal page as read-only
  protect_os_pages(mem, aligned_sz, RedjetWXState::WX_SEALED_RO);

  _page_lock.lock();
  desc->next = _pages_head;
  _pages_head = desc;
  _page_lock.unlock();

  _total_pages_allocated.fetch_add(1, std::memory_order_relaxed);
  add_used(aligned_sz);
  if (_node_rawcode_cell != nullptr) {
    _node_rawcode_cell->add_used(aligned_sz);
  }

  return mem;
}

void* RedjetVFSNode_CodeBoot::alloc_vac_processed_chunk(size_t bytes, uint32_t method_id, uint64_t mirc_key) {
  if (mirc_key != RedjetVFSCrypto::KEY_MIRC_READ) {
    _total_unauthorized_access_trapped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  size_t aligned_sz = (bytes + SYSTEM_PAGE_SIZE - 1) & ~(SYSTEM_PAGE_SIZE - 1);
  void* mem = allocate_os_pages(aligned_sz, false);
  if (mem == nullptr) return nullptr;

  auto* desc = new RedjetCodePageDesc();
  desc->page_address = mem;
  desc->page_size = aligned_sz;
  desc->used_bytes = bytes;
  desc->tier = RedjetCodePageTier::VAC_PROCESSED_CHUNK;
  desc->wx_state = RedjetWXState::WX_WRITABLE;
  desc->auth_seal = RedjetVFSCrypto::derive_7cipher_key(RedjetVFSCrypto::DEFAULT_MIRC_SECRETS);
  desc->method_id = method_id;
  desc->alloc_tsc = sacculus_rdtsc();

  _page_lock.lock();
  desc->next = _pages_head;
  _pages_head = desc;
  _page_lock.unlock();

  _total_pages_allocated.fetch_add(1, std::memory_order_relaxed);
  add_used(aligned_sz);
  if (_node_vac_processed != nullptr) {
    _node_vac_processed->add_used(aligned_sz);
  }

  return mem;
}

void* RedjetVFSNode_CodeBoot::alloc_mirc_ready_page(size_t bytes, uint32_t method_id, int mirc_level, uint64_t jit_key) {
  if (jit_key != RedjetVFSCrypto::KEY_JIT_TRANSITION &&
      jit_key != RedjetVFSCrypto::derive_7cipher_key(RedjetVFSCrypto::DEFAULT_MIRC_SECRETS)) {
    _total_unauthorized_access_trapped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  size_t aligned_sz = (bytes + SYSTEM_PAGE_SIZE - 1) & ~(SYSTEM_PAGE_SIZE - 1);
  void* mem = allocate_os_pages(aligned_sz, false); // Begins writable for code emission
  if (mem == nullptr) return nullptr;

  auto* desc = new RedjetCodePageDesc();
  desc->page_address = mem;
  desc->page_size = aligned_sz;
  desc->used_bytes = bytes;
  desc->tier = (mirc_level >= 2) ? RedjetCodePageTier::MIRC_READY_PAGE_LVL2 : RedjetCodePageTier::MIRC_READY_PAGE_LVL1;
  desc->wx_state = RedjetWXState::WX_WRITABLE;
  desc->auth_seal = RedjetVFSCrypto::KEY_JIT_TRANSITION;
  desc->method_id = method_id;
  desc->alloc_tsc = sacculus_rdtsc();

  _page_lock.lock();
  desc->next = _pages_head;
  _pages_head = desc;
  _page_lock.unlock();

  _total_pages_allocated.fetch_add(1, std::memory_order_relaxed);
  add_used(aligned_sz);
  if (mirc_level >= 2) {
    if (_node_mirc_lvl2 != nullptr) _node_mirc_lvl2->add_used(aligned_sz);
  } else {
    if (_node_mirc_lvl1 != nullptr) _node_mirc_lvl1->add_used(aligned_sz);
  }

  return mem;
}

bool RedjetVFSNode_CodeBoot::transition_wx(void* page_addr, size_t bytes, RedjetWXState target_state, uint64_t auth_key) {
  if (page_addr == nullptr || auth_key != RedjetVFSCrypto::KEY_JIT_TRANSITION) {
    _total_unauthorized_access_trapped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  _page_lock.lock();
  RedjetCodePageDesc* cur = _pages_head;
  while (cur != nullptr) {
    if (cur->page_address == page_addr) {
      break;
    }
    cur = cur->next;
  }

  if (cur == nullptr) {
    _page_lock.unlock();
    return false;
  }

  // Enforce strict W^X transition:
  // If moving to EXECUTABLE, page must first be flushed from I-Cache and write access revoked
  bool ok = protect_os_pages(page_addr, cur->page_size, target_state);
  if (ok) {
    cur->wx_state = target_state;
    _total_wx_transitions.fetch_add(1, std::memory_order_relaxed);
  }
  _page_lock.unlock();
  return ok;
}

bool RedjetVFSNode_CodeBoot::verify_page_seal(const void* page_addr, uint64_t expected_seal) noexcept {
  if (page_addr == nullptr) return false;
  _page_lock.lock();
  const RedjetCodePageDesc* cur = _pages_head;
  while (cur != nullptr) {
    if (cur->page_address == page_addr) {
      bool match = (cur->auth_seal == expected_seal);
      _page_lock.unlock();
      if (match) {
        _total_crypto_seals_verified.fetch_add(1, std::memory_order_relaxed);
      }
      return match;
    }
    cur = cur->next;
  }
  _page_lock.unlock();
  return false;
}

bool RedjetVFSNode_CodeBoot::free_code_page(void* page_addr) {
  if (page_addr == nullptr) return false;

  _page_lock.lock();
  RedjetCodePageDesc* prev = nullptr;
  RedjetCodePageDesc* cur = _pages_head;
  while (cur != nullptr) {
    if (cur->page_address == page_addr) {
      if (prev == nullptr) {
        _pages_head = cur->next;
      } else {
        prev->next = cur->next;
      }
      break;
    }
    prev = cur;
    cur = cur->next;
  }
  _page_lock.unlock();

  if (cur == nullptr) return false;

  size_t sz = cur->page_size;
  RedjetCodePageTier tier = cur->tier;
  free_os_pages(cur->page_address, sz);
  delete cur;

  sub_used(sz);
  if (tier == RedjetCodePageTier::RAWCODE_CELL && _node_rawcode_cell != nullptr) {
    _node_rawcode_cell->sub_used(sz);
  } else if (tier == RedjetCodePageTier::VAC_PROCESSED_CHUNK && _node_vac_processed != nullptr) {
    _node_vac_processed->sub_used(sz);
  } else if (tier == RedjetCodePageTier::MIRC_READY_PAGE_LVL1 && _node_mirc_lvl1 != nullptr) {
    _node_mirc_lvl1->sub_used(sz);
  } else if (tier == RedjetCodePageTier::MIRC_READY_PAGE_LVL2 && _node_mirc_lvl2 != nullptr) {
    _node_mirc_lvl2->sub_used(sz);
  }

  return true;
}
