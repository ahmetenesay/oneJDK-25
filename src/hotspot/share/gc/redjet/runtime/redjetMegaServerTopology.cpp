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

#include "gc/redjet/runtime/redjetMegaServerTopology.hpp"
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
#include <sched.h>
#include <pthread.h>
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

RedjetMegaServerTopology* RedjetMegaServerTopology::_instance = nullptr;
std::atomic<bool> RedjetMegaServerTopology::_initialized{ false };

RedjetMegaServerTopology::RedjetMegaServerTopology() {
  std::memset(_cpu_brand_string, 0, sizeof(_cpu_brand_string));
  std::memset(_cores, 0, sizeof(_cores));
  std::memset(_node_latency_matrix_ns, 0, sizeof(_node_latency_matrix_ns));
  std::memset(_ccd_latency_matrix_ns, 0, sizeof(_ccd_latency_matrix_ns));

  detect_cpu_info();
  build_topology_map();
  calibrate_latency_matrix();
}

RedjetMegaServerTopology::~RedjetMegaServerTopology() {
}

void RedjetMegaServerTopology::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetMegaServerTopology();
  }
}

void RedjetMegaServerTopology::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetMegaServerTopology::detect_cpu_info() {
  char vendor_id[13] = { 0 };

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  int cpu_info[4] = { 0 };

#if defined(_MSC_VER)
  __cpuid(cpu_info, 0);
#else
  __cpuid(0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif

  std::memcpy(vendor_id + 0, &cpu_info[1], 4); // EBX
  std::memcpy(vendor_id + 4, &cpu_info[3], 4); // EDX
  std::memcpy(vendor_id + 8, &cpu_info[2], 4); // ECX
  vendor_id[12] = '\0';

  if (std::strcmp(vendor_id, "AuthenticAMD") == 0) {
    _vendor = RedjetCpuVendor::AMD_ZEN;
    _interconnect = RedjetInterconnectType::AMD_INFINITY_FABRIC;
  } else if (std::strcmp(vendor_id, "GenuineIntel") == 0) {
    _vendor = RedjetCpuVendor::INTEL_XEON;
    _interconnect = RedjetInterconnectType::INTEL_UPI_MESH;
  } else {
    _vendor = RedjetCpuVendor::UNKNOWN;
    _interconnect = RedjetInterconnectType::UNKNOWN_INTERCONNECT;
  }

  // Fetch Brand String via Extended CPUID (0x80000002 to 0x80000004)
#if defined(_MSC_VER)
  __cpuid(cpu_info, 0x80000000);
#else
  __cpuid(0x80000000, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif
  uint32_t max_ext = static_cast<uint32_t>(cpu_info[0]);

  if (max_ext >= 0x80000004) {
    for (uint32_t op = 0x80000002; op <= 0x80000004; ++op) {
#if defined(_MSC_VER)
      __cpuid(cpu_info, op);
#else
      __cpuid(op, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif
      std::memcpy(_cpu_brand_string + (op - 0x80000002) * 16, cpu_info, 16);
    }
    _cpu_brand_string[48] = '\0';
  } else {
    std::snprintf(_cpu_brand_string, sizeof(_cpu_brand_string), "Generic x86-64 (%s)", vendor_id);
  }
#else
  _vendor = RedjetCpuVendor::ARM_NEOVERSE;
  _interconnect = RedjetInterconnectType::ARM_CMN_MESH;
  std::snprintf(_cpu_brand_string, sizeof(_cpu_brand_string), "ARM64 Server Architecture");
#endif
}

void RedjetMegaServerTopology::build_topology_map() {
#if defined(_WIN32)
  SYSTEM_INFO sys_info;
  GetSystemInfo(&sys_info);
  _total_logical_cpus = std::min(static_cast<size_t>(sys_info.dwNumberOfProcessors), MAX_LOGICAL_CPUS);
#else
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  _total_logical_cpus = (nprocs > 0) ? std::min(static_cast<size_t>(nprocs), MAX_LOGICAL_CPUS) : 8;
#endif

  if (_vendor == RedjetCpuVendor::AMD_ZEN) {
    // AMD Zen CCD heuristic: 8 cores per CCD
    _cores_per_ccd = 8;
    _total_ccds = std::max<size_t>(1, (_total_logical_cpus + _cores_per_ccd - 1) / _cores_per_ccd);
    _total_ccds = std::min(_total_ccds, MAX_CCDS);
  } else if (_vendor == RedjetCpuVendor::INTEL_XEON) {
    // Intel Xeon Sub-NUMA Clustering (SNC) heuristic: 16-32 cores per cluster
    _cores_per_ccd = 16;
    _total_ccds = std::max<size_t>(1, (_total_logical_cpus + _cores_per_ccd - 1) / _cores_per_ccd);
    _total_ccds = std::min(_total_ccds, MAX_CCDS);
  } else {
    _cores_per_ccd = 8;
    _total_ccds = std::max<size_t>(1, (_total_logical_cpus + 7) / 8);
    _total_ccds = std::min(_total_ccds, MAX_CCDS);
  }

  // NUMA nodes heuristic: 1 node per 4 CCDs or minimum 1
  _total_numa_nodes = std::max<size_t>(1, (_total_ccds + 3) / 4);
  _total_numa_nodes = std::min(_total_numa_nodes, MAX_NUMA_NODES);
  _total_sockets = std::max<size_t>(1, (_total_numa_nodes + 1) / 2);
  _total_sockets = std::min(_total_sockets, MAX_SOCKETS);
  _total_physical_cores = _total_logical_cpus;

  // Initialize per-core descriptor table
  for (size_t i = 0; i < _total_logical_cpus; ++i) {
    RedjetCoreInfo* info = &_cores[i];
    info->logical_cpu_id = static_cast<uint32_t>(i);
    info->physical_core_id = static_cast<uint32_t>(i);
    info->ccd_id = static_cast<uint32_t>(i / _cores_per_ccd);
    info->numa_node = static_cast<uint32_t>(info->ccd_id / 4);
    info->socket_id = static_cast<uint32_t>(info->numa_node / 2);
    info->l3_cache_id = info->ccd_id; // On Zen, 1 L3 per CCD
    info->is_hyperthread = (i >= _total_logical_cpus / 2);
    info->is_p_core = true;
  }
}

void RedjetMegaServerTopology::calibrate_latency_matrix() {
  // 1. NUMA Node Distance Matrix (nanoseconds)
  for (size_t i = 0; i < MAX_NUMA_NODES; ++i) {
    for (size_t j = 0; j < MAX_NUMA_NODES; ++j) {
      if (i == j) {
        _node_latency_matrix_ns[i][j] = 12; // Local NUMA node latency
      } else {
        uint32_t hops = static_cast<uint32_t>(std::abs(static_cast<int>(i) - static_cast<int>(j)));
        _node_latency_matrix_ns[i][j] = 45 + (hops * 25); // Cross-interconnect latency
      }
    }
  }

  // 2. CCD Distance Matrix (nanoseconds)
  for (size_t i = 0; i < MAX_CCDS; ++i) {
    for (size_t j = 0; j < MAX_CCDS; ++j) {
      if (i == j) {
        _ccd_latency_matrix_ns[i][j] = 10; // Intra-CCD L3 cache latency (~10ns)
      } else {
        size_t numa_i = i / 4;
        size_t numa_j = j / 4;
        if (numa_i == numa_j) {
          _ccd_latency_matrix_ns[i][j] = 35; // Cross-CCD within same NUMA/IOD (~35ns)
        } else {
          _ccd_latency_matrix_ns[i][j] = 85; // Cross-socket / remote NUMA CCD (~85ns)
        }
      }
    }
  }
}

const RedjetCoreInfo* RedjetMegaServerTopology::get_core_info(uint32_t logical_cpu) const noexcept {
  if (logical_cpu >= _total_logical_cpus) {
    return nullptr;
  }
  return &_cores[logical_cpu];
}

uint32_t RedjetMegaServerTopology::get_current_logical_cpu() const noexcept {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentProcessorNumber());
#elif defined(__linux__)
  int cpu = sched_getcpu();
  return (cpu >= 0) ? static_cast<uint32_t>(cpu) : 0;
#else
  return 0;
#endif
}

uint32_t RedjetMegaServerTopology::get_current_ccd_id() const noexcept {
  uint32_t cpu = get_current_logical_cpu();
  if (cpu < _total_logical_cpus) {
    return _cores[cpu].ccd_id;
  }
  return 0;
}

uint32_t RedjetMegaServerTopology::get_current_numa_node() const noexcept {
  uint32_t cpu = get_current_logical_cpu();
  if (cpu < _total_logical_cpus) {
    return _cores[cpu].numa_node;
  }
  return 0;
}

uint32_t RedjetMegaServerTopology::get_current_socket_id() const noexcept {
  uint32_t cpu = get_current_logical_cpu();
  if (cpu < _total_logical_cpus) {
    return _cores[cpu].socket_id;
  }
  return 0;
}

bool RedjetMegaServerTopology::are_cores_on_same_ccd(uint32_t core_a, uint32_t core_b) const noexcept {
  if (core_a >= _total_logical_cpus || core_b >= _total_logical_cpus) return false;
  return _cores[core_a].ccd_id == _cores[core_b].ccd_id;
}

bool RedjetMegaServerTopology::are_cores_on_same_l3(uint32_t core_a, uint32_t core_b) const noexcept {
  if (core_a >= _total_logical_cpus || core_b >= _total_logical_cpus) return false;
  return _cores[core_a].l3_cache_id == _cores[core_b].l3_cache_id;
}

bool RedjetMegaServerTopology::are_cores_on_same_numa(uint32_t core_a, uint32_t core_b) const noexcept {
  if (core_a >= _total_logical_cpus || core_b >= _total_logical_cpus) return false;
  return _cores[core_a].numa_node == _cores[core_b].numa_node;
}

bool RedjetMegaServerTopology::are_cores_on_same_socket(uint32_t core_a, uint32_t core_b) const noexcept {
  if (core_a >= _total_logical_cpus || core_b >= _total_logical_cpus) return false;
  return _cores[core_a].socket_id == _cores[core_b].socket_id;
}

uint32_t RedjetMegaServerTopology::get_interconnect_latency_ns(uint32_t core_a, uint32_t core_b) noexcept {
  if (core_a >= _total_logical_cpus || core_b >= _total_logical_cpus) return 100;
  uint32_t ccd_a = _cores[core_a].ccd_id;
  uint32_t ccd_b = _cores[core_b].ccd_id;

  uint32_t lat = get_ccd_distance_ns(ccd_a, ccd_b);
  if (lat > 20) {
    _total_cross_ccd_penalties_detected.fetch_add(1, std::memory_order_relaxed);
  }
  if (!are_cores_on_same_numa(core_a, core_b)) {
    _total_remote_numa_traps.fetch_add(1, std::memory_order_relaxed);
  }
  return lat;
}

uint32_t RedjetMegaServerTopology::get_numa_distance_ns(uint32_t node_a, uint32_t node_b) const noexcept {
  if (node_a >= MAX_NUMA_NODES || node_b >= MAX_NUMA_NODES) return 120;
  return _node_latency_matrix_ns[node_a][node_b];
}

uint32_t RedjetMegaServerTopology::get_ccd_distance_ns(uint32_t ccd_a, uint32_t ccd_b) const noexcept {
  if (ccd_a >= MAX_CCDS || ccd_b >= MAX_CCDS) return 100;
  return _ccd_latency_matrix_ns[ccd_a][ccd_b];
}

bool RedjetMegaServerTopology::bind_current_thread_to_core(uint32_t logical_cpu) {
  if (logical_cpu >= _total_logical_cpus) return false;

#if defined(_WIN32)
  DWORD_PTR mask = static_cast<DWORD_PTR>(1ULL << (logical_cpu % 64));
  DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
  if (prev != 0) {
    _total_thread_affinity_bindings.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(logical_cpu, &cpuset);
  int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (res == 0) {
    _total_thread_affinity_bindings.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#else
  return false;
#endif
}

bool RedjetMegaServerTopology::bind_current_thread_to_ccd(uint32_t ccd_id) {
  if (ccd_id >= _total_ccds) return false;

#if defined(_WIN32)
  DWORD_PTR mask = 0;
  for (size_t i = 0; i < _total_logical_cpus; ++i) {
    if (_cores[i].ccd_id == ccd_id && i < 64) {
      mask |= (1ULL << i);
    }
  }
  if (mask != 0) {
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (prev != 0) {
      _total_thread_affinity_bindings.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (size_t i = 0; i < _total_logical_cpus; ++i) {
    if (_cores[i].ccd_id == ccd_id) {
      CPU_SET(i, &cpuset);
    }
  }
  int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (res == 0) {
    _total_thread_affinity_bindings.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#else
  return false;
#endif
}

bool RedjetMegaServerTopology::bind_current_thread_to_numa(uint32_t numa_node) {
  if (numa_node >= _total_numa_nodes) return false;

#if defined(_WIN32)
  DWORD_PTR mask = 0;
  for (size_t i = 0; i < _total_logical_cpus; ++i) {
    if (_cores[i].numa_node == numa_node && i < 64) {
      mask |= (1ULL << i);
    }
  }
  if (mask != 0) {
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (prev != 0) {
      _total_thread_affinity_bindings.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (size_t i = 0; i < _total_logical_cpus; ++i) {
    if (_cores[i].numa_node == numa_node) {
      CPU_SET(i, &cpuset);
    }
  }
  int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (res == 0) {
    _total_thread_affinity_bindings.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#else
  return false;
#endif
}

uint32_t RedjetMegaServerTopology::get_recommended_ccd_runway(uint32_t thread_id) const noexcept {
  if (_total_ccds == 0) return 0;
  return (thread_id % static_cast<uint32_t>(_total_ccds));
}

void RedjetMegaServerTopology::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet MegaServer Hardware Topology ---");
  st->print_cr("  Brand String:         %s", _cpu_brand_string);
  st->print_cr("  Vendor:               %s",
               (_vendor == RedjetCpuVendor::AMD_ZEN) ? "AMD Zen Architecture" :
               (_vendor == RedjetCpuVendor::INTEL_XEON) ? "Intel Xeon Scalable Architecture" :
               (_vendor == RedjetCpuVendor::ARM_NEOVERSE) ? "ARM Neoverse Server Architecture" : "Unknown / Emulated");
  st->print_cr("  Interconnect:         %s",
               (_interconnect == RedjetInterconnectType::AMD_INFINITY_FABRIC) ? "AMD Infinity Fabric (GMI3)" :
               (_interconnect == RedjetInterconnectType::INTEL_UPI_MESH) ? "Intel Ultra Path Interconnect & 2D Mesh" :
               (_interconnect == RedjetInterconnectType::ARM_CMN_MESH) ? "ARM Coherent Mesh Network (CMN)" : "Standard Bus");
  st->print_cr("  Logical CPUs:         %zu", _total_logical_cpus);
  st->print_cr("  Physical Cores:       %zu", _total_physical_cores);
  st->print_cr("  Total CCDs/Clusters:  %zu (Cores/CCD: %zu)", _total_ccds, _cores_per_ccd);
  st->print_cr("  NUMA Nodes:           %zu", _total_numa_nodes);
  st->print_cr("  Sockets:              %zu", _total_sockets);
  st->print_cr("  Cross-CCD Penalties:  %llu", static_cast<unsigned long long>(_total_cross_ccd_penalties_detected.load(std::memory_order_relaxed)));
  st->print_cr("  Affinity Bindings:    %llu", static_cast<unsigned long long>(_total_thread_affinity_bindings.load(std::memory_order_relaxed)));
  st->print_cr("  Remote NUMA Traps:    %llu", static_cast<unsigned long long>(_total_remote_numa_traps.load(std::memory_order_relaxed)));
}
