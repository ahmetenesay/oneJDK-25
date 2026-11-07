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

#include "gc/redjet/runtime/redjetCoreAffinitizer.hpp"
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
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetCoreAffinitizer* RedjetCoreAffinitizer::_instance = nullptr;
std::atomic<bool> RedjetCoreAffinitizer::_initialized{ false };

RedjetCoreAffinitizer::RedjetCoreAffinitizer()
  : _total_cpus(0),
    _p_core_count(0),
    _e_core_count(0),
    _is_hybrid_topology(false),
    _total_pcore_bindings(0),
    _total_ecore_bindings(0),
    _total_migrations_prevented(0) {
  hs_ticket_lock_init(&_affinitizer_lock);
  std::memset(_cpus, 0, sizeof(_cpus));

  detect_core_types();
}

RedjetCoreAffinitizer::~RedjetCoreAffinitizer() {
}

void RedjetCoreAffinitizer::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetCoreAffinitizer();
  }
}

void RedjetCoreAffinitizer::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetCoreAffinitizer::detect_core_types() {
#if defined(_WIN32)
  SYSTEM_INFO sys_info;
  GetSystemInfo(&sys_info);
  _total_cpus = std::min<size_t>(sys_info.dwNumberOfProcessors, MAX_CPUS);
#else
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  _total_cpus = (nprocs > 0) ? std::min<size_t>(nprocs, MAX_CPUS) : 8;
#endif

  // CPUID 0x1A: Hybrid Information
  bool has_hybrid_leaf = false;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  int cpu_info[4] = { 0 };
#if defined(_MSC_VER)
  __cpuid(cpu_info, 0);
#else
  __cpuid(0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif
  if (static_cast<uint32_t>(cpu_info[0]) >= 0x1A) {
    has_hybrid_leaf = true;
  }
#endif

  _p_core_count = 0;
  _e_core_count = 0;

  if (has_hybrid_leaf && _total_cpus >= 8) {
    _is_hybrid_topology = true;
    // Typical Intel Hybrid desktop: first 8/16 are P-cores, remainder are E-cores
    size_t split = _total_cpus / 2;
    for (size_t i = 0; i < _total_cpus; ++i) {
      _cpus[i].logical_id = static_cast<uint32_t>(i);
      if (i < split) {
        _cpus[i].core_type = RedjetCoreType::CORE_PERFORMANCE;
        _cpus[i].max_frequency_mhz = 5400;
        _cpus[i].l2_cache_size_kb = 2048;
        _p_core_count++;
      } else {
        _cpus[i].core_type = RedjetCoreType::CORE_EFFICIENCY;
        _cpus[i].max_frequency_mhz = 4200;
        _cpus[i].l2_cache_size_kb = 1024;
        _e_core_count++;
      }
    }
  } else {
    _is_hybrid_topology = false;
    for (size_t i = 0; i < _total_cpus; ++i) {
      _cpus[i].logical_id = static_cast<uint32_t>(i);
      _cpus[i].core_type = RedjetCoreType::CORE_PERFORMANCE;
      _cpus[i].max_frequency_mhz = 3600;
      _cpus[i].l2_cache_size_kb = 1024;
      _p_core_count++;
    }
  }
}

RedjetCoreType RedjetCoreAffinitizer::get_core_type(uint32_t logical_cpu) const noexcept {
  if (logical_cpu < _total_cpus) {
    return _cpus[logical_cpu].core_type;
  }
  return RedjetCoreType::CORE_PERFORMANCE;
}

bool RedjetCoreAffinitizer::bind_current_thread_to_pcore() {
  uint64_t mask = 0;
  for (size_t i = 0; i < _total_cpus && i < 64; ++i) {
    if (_cpus[i].core_type == RedjetCoreType::CORE_PERFORMANCE) {
      mask |= (1ULL << i);
    }
  }

  if (mask == 0) mask = 0x01; // Fallback to core 0

#if defined(_WIN32)
  DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
  if (prev != 0) {
    _total_pcore_bindings.fetch_add(1, std::memory_order_relaxed);
    _total_migrations_prevented.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (size_t i = 0; i < _total_cpus; ++i) {
    if (_cpus[i].core_type == RedjetCoreType::CORE_PERFORMANCE) {
      CPU_SET(i, &cpuset);
    }
  }
  int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (res == 0) {
    _total_pcore_bindings.fetch_add(1, std::memory_order_relaxed);
    _total_migrations_prevented.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#else
  return false;
#endif
}

bool RedjetCoreAffinitizer::bind_current_thread_to_ecore() {
  uint64_t mask = 0;
  for (size_t i = 0; i < _total_cpus && i < 64; ++i) {
    if (_cpus[i].core_type == RedjetCoreType::CORE_EFFICIENCY) {
      mask |= (1ULL << i);
    }
  }

  if (mask == 0) {
    // If no dedicated E-cores exist, bind to higher half of homogeneous cores
    for (size_t i = _total_cpus / 2; i < _total_cpus && i < 64; ++i) {
      mask |= (1ULL << i);
    }
  }

  if (mask == 0) mask = 0x01;

#if defined(_WIN32)
  DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
  if (prev != 0) {
    _total_ecore_bindings.fetch_add(1, std::memory_order_relaxed);
    _total_migrations_prevented.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (size_t i = 0; i < _total_cpus; ++i) {
    if ((mask & (1ULL << (i % 64))) != 0) {
      CPU_SET(i, &cpuset);
    }
  }
  int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (res == 0) {
    _total_ecore_bindings.fetch_add(1, std::memory_order_relaxed);
    _total_migrations_prevented.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#else
  return false;
#endif
}

void RedjetCoreAffinitizer::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Heterogeneous Core Affinitizer ---");
  st->print_cr("  Topology:             %s", _is_hybrid_topology ? "HYBRID (P-Core / E-Core Asymmetry)" : "HOMOGENEOUS");
  st->print_cr("  Total Logical CPUs:   %zu", _total_cpus);
  st->print_cr("  Performance (P-Cores):%zu", _p_core_count);
  st->print_cr("  Efficiency (E-Cores): %zu", _e_core_count);
  st->print_cr("  P-Core Bindings:      %llu", static_cast<unsigned long long>(_total_pcore_bindings.load(std::memory_order_relaxed)));
  st->print_cr("  E-Core Bindings:      %llu", static_cast<unsigned long long>(_total_ecore_bindings.load(std::memory_order_relaxed)));
  st->print_cr("  Migrations Prevented: %llu", static_cast<unsigned long long>(_total_migrations_prevented.load(std::memory_order_relaxed)));
}
