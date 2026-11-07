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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETMEGASERVERTOPOLOGY_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETMEGASERVERTOPOLOGY_HPP

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
// MegaServer Hardware Topology & Interconnect Engine (PLAN.md Section 8.4, 8.8)
// - AMD EPYC / Threadripper (up to 16 CCDs, GMI3 Infinity Fabric interconnect)
// - Intel Xeon Scalable (up to 128 cores, Sub-NUMA Clustering SNC-3/SNC-4, UPI 2.0)
// - Ampere Altra Max / ARM Neoverse (128 cores, 2D mesh interconnect)
// - Hardware hop distance matrix & nanosecond latency penalty evaluator
// - Zero-overhead thread affinity binding and CCD-aware runway partitioning
// ============================================================================

enum class RedjetCpuVendor : uint8_t {
  UNKNOWN = 0,
  AMD_ZEN = 1,
  INTEL_XEON = 2,
  ARM_NEOVERSE = 3
};

enum class RedjetInterconnectType : uint8_t {
  UNKNOWN_INTERCONNECT = 0,
  AMD_INFINITY_FABRIC  = 1, // AMD GMI3 links through IOD
  INTEL_UPI_MESH       = 2, // Intel Ultra Path Interconnect & CHA 2D Mesh
  ARM_CMN_MESH         = 3  // ARM Coherent Mesh Network (CMN-600 / CMN-700)
};

struct RedjetCoreInfo {
  uint32_t logical_cpu_id{ 0 };
  uint32_t physical_core_id{ 0 };
  uint32_t ccd_id{ 0 };          // CCD (AMD) or Cluster/SNC partition (Intel)
  uint32_t numa_node{ 0 };
  uint32_t socket_id{ 0 };
  uint32_t l3_cache_id{ 0 };
  bool     is_hyperthread{ false };
  bool     is_p_core{ true };
};

class RedjetMegaServerTopology {
public:
  static constexpr size_t MAX_LOGICAL_CPUS = 512;
  static constexpr size_t MAX_CCDS         = 32;
  static constexpr size_t MAX_NUMA_NODES   = 16;
  static constexpr size_t MAX_SOCKETS      = 4;

private:
  static RedjetMegaServerTopology* _instance;
  static std::atomic<bool>         _initialized;

  RedjetCpuVendor         _vendor{ RedjetCpuVendor::UNKNOWN };
  RedjetInterconnectType  _interconnect{ RedjetInterconnectType::UNKNOWN_INTERCONNECT };
  char                    _cpu_brand_string[64]{ 0 };

  size_t                  _total_logical_cpus{ 0 };
  size_t                  _total_physical_cores{ 0 };
  size_t                  _total_ccds{ 0 };
  size_t                  _total_numa_nodes{ 0 };
  size_t                  _total_sockets{ 0 };
  size_t                  _cores_per_ccd{ 0 };

  RedjetCoreInfo          _cores[MAX_LOGICAL_CPUS];

  // Interconnect latency distance matrix in nanoseconds: [node_i][node_j]
  uint32_t                _node_latency_matrix_ns[MAX_NUMA_NODES][MAX_NUMA_NODES];

  // CCD cross-traffic penalty matrix in nanoseconds: [ccd_i][ccd_j]
  uint32_t                _ccd_latency_matrix_ns[MAX_CCDS][MAX_CCDS];

  // Telemetry
  std::atomic<uint64_t>   _total_cross_ccd_penalties_detected{ 0 };
  std::atomic<uint64_t>   _total_thread_affinity_bindings{ 0 };
  std::atomic<uint64_t>   _total_remote_numa_traps{ 0 };

  void detect_cpu_info();
  void build_topology_map();
  void calibrate_latency_matrix();

public:
  RedjetMegaServerTopology();
  ~RedjetMegaServerTopology();

  static void initialize();
  static void destroy();
  static RedjetMegaServerTopology* get() noexcept { return _instance; }

  // Hardware topology queries
  RedjetCpuVendor        vendor() const noexcept { return _vendor; }
  RedjetInterconnectType interconnect() const noexcept { return _interconnect; }
  const char*            brand_string() const noexcept { return _cpu_brand_string; }

  size_t total_logical_cpus() const noexcept { return _total_logical_cpus; }
  size_t total_physical_cores() const noexcept { return _total_physical_cores; }
  size_t total_ccds() const noexcept { return _total_ccds; }
  size_t total_numa_nodes() const noexcept { return _total_numa_nodes; }
  size_t total_sockets() const noexcept { return _total_sockets; }
  size_t cores_per_ccd() const noexcept { return _cores_per_ccd; }

  // Core information queries
  const RedjetCoreInfo* get_core_info(uint32_t logical_cpu) const noexcept;
  uint32_t get_current_logical_cpu() const noexcept;
  uint32_t get_current_ccd_id() const noexcept;
  uint32_t get_current_numa_node() const noexcept;
  uint32_t get_current_socket_id() const noexcept;

  // Interconnect & Cache co-locality tests
  bool are_cores_on_same_ccd(uint32_t core_a, uint32_t core_b) const noexcept;
  bool are_cores_on_same_l3(uint32_t core_a, uint32_t core_b) const noexcept;
  bool are_cores_on_same_numa(uint32_t core_a, uint32_t core_b) const noexcept;
  bool are_cores_on_same_socket(uint32_t core_a, uint32_t core_b) const noexcept;

  // Latency & Penalty calculations
  uint32_t get_interconnect_latency_ns(uint32_t core_a, uint32_t core_b) noexcept;
  uint32_t get_numa_distance_ns(uint32_t node_a, uint32_t node_b) const noexcept;
  uint32_t get_ccd_distance_ns(uint32_t ccd_a, uint32_t ccd_b) const noexcept;

  // Thread affinity binding
  RJ_API bool bind_current_thread_to_core(uint32_t logical_cpu);
  RJ_API bool bind_current_thread_to_ccd(uint32_t ccd_id);
  RJ_API bool bind_current_thread_to_numa(uint32_t numa_node);

  // Recommended partition for memory allocation
  uint32_t get_recommended_ccd_runway(uint32_t thread_id) const noexcept;

  // Telemetry
  uint64_t cross_ccd_penalties() const noexcept { return _total_cross_ccd_penalties_detected.load(std::memory_order_relaxed); }
  uint64_t affinity_bindings()   const noexcept { return _total_thread_affinity_bindings.load(std::memory_order_relaxed); }
  uint64_t remote_numa_traps()   const noexcept { return _total_remote_numa_traps.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETMEGASERVERTOPOLOGY_HPP
