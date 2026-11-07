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

#include "gc/redjet/vfs/redjetCodeSealTrap.hpp"
#include <cstdio>
#include <cstdlib>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetCodeSealTrap* RedjetCodeSealTrap::_instance = nullptr;
std::atomic<bool>   RedjetCodeSealTrap::_initialized{ false };

RedjetCodeSealTrap::RedjetCodeSealTrap() {
  hs_ticket_lock_init(&_trap_lock);
  for (size_t i = 0; i < MAX_SEALED_ENTRIES; ++i) {
    _entries[i].code_ptr = nullptr;
    _entries[i].code_size = 0;
    _entries[i].method_id = 0;
    _entries[i].expected_seal = 0;
    _entries[i].last_verified_tsc = 0;
    _entries[i].is_active = false;
    _entries[i].is_compromised = false;
  }
}

RedjetCodeSealTrap::~RedjetCodeSealTrap() {
  // Release resources
}

void RedjetCodeSealTrap::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetCodeSealTrap();
  }
}

void RedjetCodeSealTrap::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

static inline uint64_t rotl64(uint64_t x, int b) noexcept {
  return (x << b) | (x >> (64 - b));
}

uint64_t RedjetCodeSealTrap::compute_code_seal(const void* data, size_t size, uint64_t k0, uint64_t k1) noexcept {
  if (data == nullptr || size == 0) return 0;

  uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
  uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
  uint64_t v3 = 0x7465646279746573ULL ^ k1;

  const uint8_t* ptr = static_cast<const uint8_t*>(data);
  size_t words = size / sizeof(uint64_t);

  for (size_t i = 0; i < words; ++i) {
    uint64_t m = 0;
    std::memcpy(&m, ptr + (i * sizeof(uint64_t)), sizeof(uint64_t));

    v3 ^= m;
    // SipRound
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);

    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);

    v0 ^= m;
  }

  // Handle trailing bytes
  size_t remainder = size % sizeof(uint64_t);
  uint64_t last_word = static_cast<uint64_t>(size) << 56;
  if (remainder > 0) {
    const uint8_t* rem_ptr = ptr + (words * sizeof(uint64_t));
    for (size_t j = 0; j < remainder; ++j) {
      last_word |= static_cast<uint64_t>(rem_ptr[j]) << (j * 8);
    }
  }

  v3 ^= last_word;
  v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
  v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
  v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
  v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
  v0 ^= last_word;

  v2 ^= 0xff;
  // Finalization rounds
  for (int r = 0; r < 4; ++r) {
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
  }

  return (v0 ^ v1 ^ v2 ^ v3);
}

bool RedjetCodeSealTrap::register_code_seal(void* code_ptr, size_t code_size, uint32_t method_id) {
  if (code_ptr == nullptr || code_size == 0) return false;

  uint64_t seal = compute_code_seal(code_ptr, code_size, GOLDEN_SEED_K0, GOLDEN_SEED_K1);

  hs_ticket_lock_acquire(&_trap_lock);

  // Check if already registered
  for (size_t i = 0; i < _entry_count; ++i) {
    if (_entries[i].code_ptr == code_ptr) {
      _entries[i].code_size = code_size;
      _entries[i].method_id = method_id;
      _entries[i].expected_seal = seal;
      _entries[i].last_verified_tsc = sacculus_rdtsc();
      _entries[i].is_active = true;
      _entries[i].is_compromised = false;
      hs_ticket_lock_release(&_trap_lock);
      return true;
    }
  }

  if (_entry_count >= MAX_SEALED_ENTRIES) {
    hs_ticket_lock_release(&_trap_lock);
    return false;
  }

  size_t idx = _entry_count++;
  _entries[idx].code_ptr = code_ptr;
  _entries[idx].code_size = code_size;
  _entries[idx].method_id = method_id;
  _entries[idx].expected_seal = seal;
  _entries[idx].last_verified_tsc = sacculus_rdtsc();
  _entries[idx].is_active = true;
  _entries[idx].is_compromised = false;

  _total_pages_registered.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_trap_lock);
  return true;
}

bool RedjetCodeSealTrap::unregister_code_seal(void* code_ptr) {
  if (code_ptr == nullptr) return false;

  hs_ticket_lock_acquire(&_trap_lock);
  for (size_t i = 0; i < _entry_count; ++i) {
    if (_entries[i].code_ptr == code_ptr) {
      _entries[i].is_active = false;
      _entries[i].code_ptr = nullptr;
      hs_ticket_lock_release(&_trap_lock);
      return true;
    }
  }
  hs_ticket_lock_release(&_trap_lock);
  return false;
}

RedjetSealStatus RedjetCodeSealTrap::verify_page(void* code_ptr) {
  if (code_ptr == nullptr) return RedjetSealStatus::SEAL_INVALID_PAGE;

  hs_ticket_lock_acquire(&_trap_lock);

  RedjetSealEntry* entry = nullptr;
  for (size_t i = 0; i < _entry_count; ++i) {
    if (_entries[i].is_active && _entries[i].code_ptr == code_ptr) {
      entry = &_entries[i];
      break;
    }
  }

  if (entry == nullptr) {
    hs_ticket_lock_release(&_trap_lock);
    return RedjetSealStatus::SEAL_INVALID_PAGE;
  }

  _total_verifications.fetch_add(1, std::memory_order_relaxed);
  entry->last_verified_tsc = sacculus_rdtsc();

  uint64_t actual_seal = compute_code_seal(entry->code_ptr, entry->code_size, GOLDEN_SEED_K0, GOLDEN_SEED_K1);

  if (actual_seal != entry->expected_seal) {
    entry->is_compromised = true;
    _total_bitflips_trapped.fetch_add(1, std::memory_order_relaxed);
    hs_ticket_lock_release(&_trap_lock);
    return RedjetSealStatus::SEAL_BIT_FLIP_TRAPPED;
  }

  hs_ticket_lock_release(&_trap_lock);
  return RedjetSealStatus::SEAL_VALID;
}

size_t RedjetCodeSealTrap::verify_all_pages() {
  uint64_t start_tsc = sacculus_rdtsc();
  size_t violations = 0;

  hs_ticket_lock_acquire(&_trap_lock);

  for (size_t i = 0; i < _entry_count; ++i) {
    RedjetSealEntry& entry = _entries[i];
    if (!entry.is_active || entry.code_ptr == nullptr) continue;

    _total_verifications.fetch_add(1, std::memory_order_relaxed);
    entry.last_verified_tsc = start_tsc;

    uint64_t actual_seal = compute_code_seal(entry.code_ptr, entry.code_size, GOLDEN_SEED_K0, GOLDEN_SEED_K1);
    if (actual_seal != entry.expected_seal) {
      entry.is_compromised = true;
      violations++;
      _total_bitflips_trapped.fetch_add(1, std::memory_order_relaxed);
    }
  }

  _total_scan_cycles.fetch_add(sacculus_rdtsc() - start_tsc, std::memory_order_relaxed);
  hs_ticket_lock_release(&_trap_lock);
  return violations;
}

void RedjetCodeSealTrap::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetCodeSealTrap] W^X Executable Code Seal Status:");
  st->print_cr("  - Registered Pages:     %llu",
               static_cast<unsigned long long>(_total_pages_registered.load(std::memory_order_relaxed)));
  st->print_cr("  - Verifications Run:    %llu",
               static_cast<unsigned long long>(_total_verifications.load(std::memory_order_relaxed)));
  st->print_cr("  - Bit-Flips Trapped:    %llu",
               static_cast<unsigned long long>(_total_bitflips_trapped.load(std::memory_order_relaxed)));
  st->print_cr("  - Active Code Pages:");
  for (size_t i = 0; i < _entry_count; ++i) {
    if (_entries[i].is_active) {
      st->print_cr("      [%zu] Method %u (Base: %p, Size: %zu B) -> Status: %s [Expected Seal: 0x%llx]",
                   i,
                   _entries[i].method_id,
                   _entries[i].code_ptr,
                   _entries[i].code_size,
                   _entries[i].is_compromised ? "COMPROMISED (TRAPPED)" : "VERIFIED CLEAN",
                   static_cast<unsigned long long>(_entries[i].expected_seal));
    }
  }
}
