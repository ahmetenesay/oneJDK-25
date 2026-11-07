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

#include "gc/redjet/runtime/redjetHtmEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <cpuid.h>
#include <immintrin.h>
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetHtmEngine* RedjetHtmEngine::_instance = nullptr;
std::atomic<bool> RedjetHtmEngine::_initialized{ false };

RedjetHtmEngine::RedjetHtmEngine()
  : _tsx_supported(false),
    _total_transactions_attempted(0),
    _total_transactions_committed(0),
    _total_conflict_aborts(0),
    _total_capacity_aborts(0),
    _total_fallback_lock_acquisitions(0) {
  hs_ticket_lock_init(&_fallback_lock);
  detect_tsx_hardware();
}

RedjetHtmEngine::~RedjetHtmEngine() {
}

void RedjetHtmEngine::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetHtmEngine();
  }
}

void RedjetHtmEngine::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetHtmEngine::detect_tsx_hardware() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  int cpu_info[4] = { 0 };

#if defined(_MSC_VER)
  __cpuidex(cpu_info, 7, 0);
#else
  __cpuid_count(7, 0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif

  // Bit 11 of EBX: RTM (Restricted Transactional Memory)
  bool rtm = (cpu_info[1] & (1 << 11)) != 0;
  _tsx_supported = rtm;
#else
  _tsx_supported = false;
#endif
}

uint32_t RedjetHtmEngine::begin_transaction() noexcept {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__))
  if (_tsx_supported) {
    return static_cast<uint32_t>(_xbegin());
  }
#endif
  return static_cast<uint32_t>(RedjetHtmStatus::HTM_ABORT_EXPLICIT);
}

void RedjetHtmEngine::end_transaction() noexcept {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__))
  if (_tsx_supported) {
    _xend();
  }
#endif
}

void RedjetHtmEngine::abort_transaction(uint8_t code) noexcept {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__))
  if (_tsx_supported) {
    _xabort(code);
  }
#endif
}

void RedjetHtmEngine::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Hardware Transactional Memory (HTM) Engine ---");
  st->print_cr("  Hardware TSX Supported:  %s", _tsx_supported ? "YES (Intel TSX Active)" : "NO (Ticket Lock Fallback)");
  st->print_cr("  Transactions Attempted:  %llu", static_cast<unsigned long long>(_total_transactions_attempted.load(std::memory_order_relaxed)));
  st->print_cr("  Transactions Committed:  %llu", static_cast<unsigned long long>(_total_transactions_committed.load(std::memory_order_relaxed)));
  st->print_cr("  Conflict Aborts:         %llu", static_cast<unsigned long long>(_total_conflict_aborts.load(std::memory_order_relaxed)));
  st->print_cr("  Capacity Aborts:         %llu", static_cast<unsigned long long>(_total_capacity_aborts.load(std::memory_order_relaxed)));
  st->print_cr("  Fallback Lock Acquired:  %llu", static_cast<unsigned long long>(_total_fallback_lock_acquisitions.load(std::memory_order_relaxed)));
}
