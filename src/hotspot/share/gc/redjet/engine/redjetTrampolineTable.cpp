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

#include "gc/redjet/engine/redjetTrampolineTable.hpp"
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetTrampolineTable* RedjetTrampolineTable::_instance = nullptr;
std::atomic<bool>      RedjetTrampolineTable::_initialized{ false };

RedjetTrampolineTable::RedjetTrampolineTable() {
  hs_ticket_lock_init(&_table_lock);
  for (size_t i = 0; i < MAX_TRAMPOLINES; ++i) {
    std::memset(_slots[i].code_bytes, 0x90, sizeof(_slots[i].code_bytes));
    _slots[i].target_address = nullptr;
    _slots[i].method_id = 0;
    _slots[i].active_tier = RedjetTrampolineTier::TIER_INTERPRETER_FALLBACK;
    _slots[i].is_occupied = false;
    _slots[i].installed_tsc = 0;
    _slots[i].swaps_count = 0;
  }
}

RedjetTrampolineTable::~RedjetTrampolineTable() {
  // Release resources
}

void RedjetTrampolineTable::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetTrampolineTable();
  }
}

void RedjetTrampolineTable::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

void RedjetTrampolineTable::emit_trampoline_code(uint8_t* code_buf, void* target_address) noexcept {
  if (code_buf == nullptr) return;

  // Slot 16 Architecture:
  // Byte 0..5: FF 25 02 00 00 00 (JMP qword ptr [rip + 2])
  // Byte 6..7: 66 90 (2-byte NOP padding to guarantee 8-byte alignment for target ptr)
  // Byte 8..15: 8-byte 64-bit absolute target pointer
  code_buf[0] = 0xFF;
  code_buf[1] = 0x25;
  code_buf[2] = 0x02;
  code_buf[3] = 0x00;
  code_buf[4] = 0x00;
  code_buf[5] = 0x00;
  code_buf[6] = 0x66;
  code_buf[7] = 0x90;

  std::atomic<void*>* target_atomic = reinterpret_cast<std::atomic<void*>*>(&code_buf[8]);
  target_atomic->store(target_address, std::memory_order_release);
}

void RedjetTrampolineTable::flush_instruction_cache(void* addr, size_t size) noexcept {
  if (addr == nullptr || size == 0) return;
#if defined(_WIN32)
  FlushInstructionCache(GetCurrentProcess(), addr, size);
#elif defined(__GNUC__) || defined(__clang__)
  __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr) + size);
#endif
}

void* RedjetTrampolineTable::install_trampoline(uint32_t method_id,
                                                void* initial_target,
                                                RedjetTrampolineTier tier) {
  hs_ticket_lock_acquire(&_table_lock);

  // Check if slot already exists for method
  for (size_t i = 0; i < _allocated_slots; ++i) {
    if (_slots[i].is_occupied && _slots[i].method_id == method_id) {
      // Re-target existing slot
      emit_trampoline_code(_slots[i].code_bytes, initial_target);
      _slots[i].target_address = initial_target;
      _slots[i].active_tier = tier;
      _slots[i].swaps_count++;
      flush_instruction_cache(_slots[i].code_bytes, sizeof(_slots[i].code_bytes));
      hs_ticket_lock_release(&_table_lock);
      _total_icache_flushes.fetch_add(1, std::memory_order_relaxed);
      return _slots[i].code_bytes;
    }
  }

  if (_allocated_slots >= MAX_TRAMPOLINES) {
    hs_ticket_lock_release(&_table_lock);
    return nullptr;
  }

  size_t idx = _allocated_slots++;
  RedjetTrampolineSlot& slot = _slots[idx];
  slot.method_id = method_id;
  slot.target_address = initial_target;
  slot.active_tier = tier;
  slot.is_occupied = true;
  slot.installed_tsc = sacculus_rdtsc();
  slot.swaps_count = 0;

  emit_trampoline_code(slot.code_bytes, initial_target);
  flush_instruction_cache(slot.code_bytes, sizeof(slot.code_bytes));

  _total_trampolines_installed.fetch_add(1, std::memory_order_relaxed);
  _total_icache_flushes.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_table_lock);
  return slot.code_bytes;
}

bool RedjetTrampolineTable::hot_swap_target(uint32_t method_id,
                                            void* new_target,
                                            RedjetTrampolineTier new_tier) {
  hs_ticket_lock_acquire(&_table_lock);

  RedjetTrampolineSlot* target_slot = nullptr;
  for (size_t i = 0; i < _allocated_slots; ++i) {
    if (_slots[i].is_occupied && _slots[i].method_id == method_id) {
      target_slot = &_slots[i];
      break;
    }
  }

  if (target_slot == nullptr) {
    hs_ticket_lock_release(&_table_lock);
    return false;
  }

  // Atomic pointer store at byte 8 of code_bytes
  std::atomic<void*>* target_atomic = reinterpret_cast<std::atomic<void*>*>(&target_slot->code_bytes[8]);
  target_atomic->store(new_target, std::memory_order_release);

  target_slot->target_address = new_target;
  target_slot->active_tier = new_tier;
  target_slot->swaps_count++;

  flush_instruction_cache(target_slot->code_bytes, sizeof(target_slot->code_bytes));

  _total_hot_swaps_performed.fetch_add(1, std::memory_order_relaxed);
  _total_icache_flushes.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_table_lock);
  return true;
}

void* RedjetTrampolineTable::get_active_target(uint32_t method_id) const noexcept {
  for (size_t i = 0; i < _allocated_slots; ++i) {
    if (_slots[i].is_occupied && _slots[i].method_id == method_id) {
      const std::atomic<void*>* target_atomic = reinterpret_cast<const std::atomic<void*>*>(&_slots[i].code_bytes[8]);
      return target_atomic->load(std::memory_order_acquire);
    }
  }
  return nullptr;
}

bool RedjetTrampolineTable::release_trampoline(uint32_t method_id) {
  hs_ticket_lock_acquire(&_table_lock);
  for (size_t i = 0; i < _allocated_slots; ++i) {
    if (_slots[i].is_occupied && _slots[i].method_id == method_id) {
      _slots[i].is_occupied = false;
      _slots[i].target_address = nullptr;
      std::memset(_slots[i].code_bytes, 0x90, sizeof(_slots[i].code_bytes));
      flush_instruction_cache(_slots[i].code_bytes, sizeof(_slots[i].code_bytes));
      hs_ticket_lock_release(&_table_lock);
      return true;
    }
  }
  hs_ticket_lock_release(&_table_lock);
  return false;
}

void RedjetTrampolineTable::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetTrampolineTable] Slot 16 Dynamic Trampoline Status:");
  st->print_cr("  - Installed Slots:       %llu / %zu",
               static_cast<unsigned long long>(_total_trampolines_installed.load(std::memory_order_relaxed)),
               MAX_TRAMPOLINES);
  st->print_cr("  - Hot-Swaps Performed:   %llu",
               static_cast<unsigned long long>(_total_hot_swaps_performed.load(std::memory_order_relaxed)));
  st->print_cr("  - ICache Flushes Done:   %llu",
               static_cast<unsigned long long>(_total_icache_flushes.load(std::memory_order_relaxed)));
  st->print_cr("  - Active Trampoline Routes:");
  for (size_t i = 0; i < _allocated_slots; ++i) {
    if (_slots[i].is_occupied) {
      const char* tier_str = "UNKNOWN";
      switch (_slots[i].active_tier) {
        case RedjetTrampolineTier::TIER_INTERPRETER_FALLBACK: tier_str = "INTERPRETER"; break;
        case RedjetTrampolineTier::TIER_LEVEL1_FAST_JIT:      tier_str = "LEVEL1_JIT"; break;
        case RedjetTrampolineTier::TIER_LEVEL2_SUPERLOOP_MIRC:tier_str = "LEVEL2_SUPERLOOP"; break;
        case RedjetTrampolineTier::TIER_PUZZLER_HOTSWAP_WIN:  tier_str = "PUZZLER_WIN"; break;
      }
      st->print_cr("      [%zu] Method %u: Trampoline %p -> Target %p [Tier: %s, Swaps: %llu]",
                   i,
                   _slots[i].method_id,
                   _slots[i].code_bytes,
                   _slots[i].target_address,
                   tier_str,
                   static_cast<unsigned long long>(_slots[i].swaps_count));
    }
  }
}
