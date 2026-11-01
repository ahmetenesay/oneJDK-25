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

#include "gc/redjet/rjnrml/redjetOSAffinity.hpp"
#include "logging/log.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

namespace {

struct CpuSlot {
  uint16_t group_or_node;
  uint16_t cpu_index;
  uint64_t mask_bit;
};

struct TopologyState {
  std::mutex            init_lock;
  std::atomic<bool>     initialized{false};
  RedjetTopologySummary summary{};
  std::vector<CpuSlot>  all_cpus;
  std::vector<CpuSlot>  vcache_ccd0_cpus;
  std::vector<CpuSlot>  high_freq_ccd1_cpus;
  std::vector<CpuSlot>  p_core_cpus;
  std::vector<CpuSlot>  e_core_cpus;
};

static TopologyState g_topo;

#if defined(_WIN32)
static void set_win_thread_eco_qos(bool eco_efficiency) {
  struct LocalThreadPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  };
  constexpr int LocalThreadPowerThrottling = 1;

  LocalThreadPowerThrottlingState state{};
  state.Version = 1;
  state.ControlMask = 0x1;
  state.StateMask = eco_efficiency ? 0x1 : 0x0;

  SetThreadPriority(GetCurrentThread(),
                    eco_efficiency ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL);

  using SetThreadInfoFn = BOOL(WINAPI*)(HANDLE, int, PVOID, DWORD);
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 != nullptr) {
    auto fn = reinterpret_cast<SetThreadInfoFn>(GetProcAddress(kernel32, "SetThreadInformation"));
    if (fn != nullptr) {
      (void)fn(GetCurrentThread(), LocalThreadPowerThrottling, &state, sizeof(state));
    }
  }
}
#endif

} // namespace

void RedjetOSAffinity::initialize_topology() {
  if (g_topo.initialized.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_topo.init_lock);
  if (g_topo.initialized.load(std::memory_order_relaxed)) {
    return;
  }

#if defined(_WIN32)
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
  if (length > 0) {
    std::vector<uint8_t> buffer(length);
    auto pinfo = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (GetLogicalProcessorInformationEx(RelationAll, pinfo, &length)) {
      size_t offset = 0;
      size_t max_l3 = 0;
      size_t min_l3 = static_cast<size_t>(-1);

      while (offset < length) {
        auto cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        if (cur->Relationship == RelationProcessorCore) {
          BYTE eff_class = cur->Processor.EfficiencyClass;
          for (WORD g = 0; g < cur->Processor.GroupCount; ++g) {
            KAFFINITY mask = cur->Processor.GroupMask[g].Mask;
            WORD grp = cur->Processor.GroupMask[g].Group;
            for (BYTE b = 0; b < sizeof(KAFFINITY) * 8; ++b) {
              if (mask & (KAFFINITY(1) << b)) {
                CpuSlot slot{grp, b, KAFFINITY(1) << b};
                g_topo.all_cpus.push_back(slot);
                if (eff_class > 0) {
                  g_topo.p_core_cpus.push_back(slot);
                } else {
                  g_topo.e_core_cpus.push_back(slot);
                }
              }
            }
          }
        } else if (cur->Relationship == RelationCache) {
          if (cur->Cache.Level == 3) {
            size_t csize = cur->Cache.CacheSize;
            if (csize > max_l3) max_l3 = csize;
            if (csize < min_l3) min_l3 = csize;
            WORD grp = cur->Cache.GroupMask.Group;
            KAFFINITY mask = cur->Cache.GroupMask.Mask;
            for (BYTE b = 0; b < sizeof(KAFFINITY) * 8; ++b) {
              if (mask & (KAFFINITY(1) << b)) {
                CpuSlot slot{grp, b, KAFFINITY(1) << b};
                if (csize >= 96 * 1024 * 1024) { // AMD 3D V-Cache 96MB CCD0
                  g_topo.vcache_ccd0_cpus.push_back(slot);
                } else {
                  g_topo.high_freq_ccd1_cpus.push_back(slot);
                }
              }
            }
          }
        }
        offset += cur->Size;
      }
      g_topo.summary.total_logical_cpus = static_cast<uint32_t>(g_topo.all_cpus.size());
      g_topo.summary.p_core_count = static_cast<uint32_t>(g_topo.p_core_cpus.size());
      g_topo.summary.e_core_count = static_cast<uint32_t>(g_topo.e_core_cpus.size());
      g_topo.summary.vcache_ccd0_mask_count = static_cast<uint32_t>(g_topo.vcache_ccd0_cpus.size());
      g_topo.summary.high_freq_ccd1_mask_count = static_cast<uint32_t>(g_topo.high_freq_ccd1_cpus.size());
      g_topo.summary.max_l3_bytes_per_ccx = max_l3;
      g_topo.summary.has_amd_3d_vcache_asymmetry = (g_topo.summary.vcache_ccd0_mask_count > 0 &&
                                                    g_topo.summary.high_freq_ccd1_mask_count > 0);
      g_topo.summary.has_hybrid_p_e_cores = (g_topo.summary.p_core_count > 0 &&
                                             g_topo.summary.e_core_count > 0 &&
                                             g_topo.summary.p_core_count != g_topo.summary.total_logical_cpus);
    }
  }
#elif defined(__linux__)
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  if (nprocs <= 0) nprocs = 1;
  g_topo.summary.total_logical_cpus = static_cast<uint32_t>(nprocs);
  for (long i = 0; i < nprocs; ++i) {
    CpuSlot slot{0, static_cast<uint16_t>(i), 1ULL << (i % 64)};
    g_topo.all_cpus.push_back(slot);
    g_topo.p_core_cpus.push_back(slot);
  }
  g_topo.summary.p_core_count = static_cast<uint32_t>(nprocs);
  g_topo.summary.e_core_count = 0;
#else
  g_topo.summary.total_logical_cpus = 1;
  g_topo.summary.p_core_count = 1;
  g_topo.summary.e_core_count = 0;
  CpuSlot slot{0, 0, 1};
  g_topo.all_cpus.push_back(slot);
#endif

  g_topo.initialized.store(true, std::memory_order_release);
}

const RedjetTopologySummary& RedjetOSAffinity::topology() {
  if (!g_topo.initialized.load(std::memory_order_acquire)) {
    initialize_topology();
  }
  return g_topo.summary;
}

bool RedjetOSAffinity::bind_current_thread(RedjetCoreClass target_class, uint32_t lane_hint) {
  if (!g_topo.initialized.load(std::memory_order_acquire)) {
    initialize_topology();
  }

  const std::vector<CpuSlot>* target_pool = &g_topo.all_cpus;
  switch (target_class) {
    case RedjetCoreClass::VCACHE_3D_CCD0:
      if (!g_topo.vcache_ccd0_cpus.empty()) target_pool = &g_topo.vcache_ccd0_cpus;
      else if (!g_topo.p_core_cpus.empty()) target_pool = &g_topo.p_core_cpus;
      break;
    case RedjetCoreClass::HIGH_FREQ_CCD1:
      if (!g_topo.high_freq_ccd1_cpus.empty()) target_pool = &g_topo.high_freq_ccd1_cpus;
      break;
    case RedjetCoreClass::INTEL_BLC_PCORE:
      if (!g_topo.p_core_cpus.empty()) target_pool = &g_topo.p_core_cpus;
      break;
    case RedjetCoreClass::EFFICIENCY_ECORE:
      if (!g_topo.e_core_cpus.empty()) target_pool = &g_topo.e_core_cpus;
      break;
    default:
      break;
  }

  if (target_pool->empty()) return false;
  const CpuSlot& chosen = (*target_pool)[lane_hint % target_pool->size()];

#if defined(_WIN32)
  GROUP_AFFINITY ga{};
  ga.Group = chosen.group_or_node;
  ga.Mask = chosen.mask_bit;
  bool ok = (SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0);
  if (target_class == RedjetCoreClass::EFFICIENCY_ECORE) {
    set_win_thread_eco_qos(true);
  } else {
    set_win_thread_eco_qos(false);
  }
  return ok;
#elif defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(chosen.cpu_index, &cpuset);
  return (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0);
#else
  (void)chosen;
  return true;
#endif
}

bool RedjetOSAffinity::bind_mutator_for_batch(uint32_t priority, bool sram_focus) {
  if (sram_focus || priority >= 8) {
    return bind_current_thread(RedjetCoreClass::VCACHE_3D_CCD0, priority);
  } else if (priority >= 5) {
    return bind_current_thread(RedjetCoreClass::INTEL_BLC_PCORE, priority);
  }
  return bind_current_thread(RedjetCoreClass::ANY_CORE, priority);
}

bool RedjetOSAffinity::bind_background_gc_worker(uint32_t worker_id) {
  if (g_topo.summary.has_hybrid_p_e_cores && !g_topo.e_core_cpus.empty()) {
    return bind_current_thread(RedjetCoreClass::EFFICIENCY_ECORE, worker_id);
  }
  return bind_current_thread(RedjetCoreClass::ANY_CORE, worker_id);
}

uint32_t RedjetOSAffinity::current_logical_cpu() {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentProcessorNumber());
#elif defined(__linux__)
  int cpu = sched_getcpu();
  return cpu >= 0 ? static_cast<uint32_t>(cpu) : 0u;
#else
  return 0u;
#endif
}
