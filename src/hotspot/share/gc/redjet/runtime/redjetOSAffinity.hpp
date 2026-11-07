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

#ifndef SHARE_GC_REDJET_REDJETOSAFFINITY_HPP
#define SHARE_GC_REDJET_REDJETOSAFFINITY_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#if defined(X86) || defined(AARCH64) || defined(RISCV)
#include "is/isMacrowebHolder.hpp"
#define REDJET_HAS_MACROWEB_HOLDER 1
#endif

#ifndef REDJET_PAUSE_SMT
#if defined(_WIN32) && (defined(_M_AMD64) || defined(_M_IX86))
#include <intrin.h>
#define REDJET_PAUSE_SMT() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define REDJET_PAUSE_SMT() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define REDJET_PAUSE_SMT() asm volatile("yield" ::: "memory")
#elif defined(__riscv)
#define REDJET_PAUSE_SMT() asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory")
#else
#define REDJET_PAUSE_SMT() ((void)0)
#endif
#endif

#ifndef REDJET_CYCLE_COUNTER
#if defined(_WIN32) && (defined(_M_AMD64) || defined(_M_IX86))
#include <intrin.h>
#define REDJET_CYCLE_COUNTER() __rdtsc()
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define REDJET_CYCLE_COUNTER() __rdtsc()
#else
#define REDJET_CYCLE_COUNTER() 0ULL
#endif
#endif

enum class RedjetCoreClass : uint8_t {
  ANY_CORE          = 0,
  VCACHE_3D_CCD0    = 1, // AMD 3D V-Cache high-L3 CCD (e.g. 96MB L3 CCD0 on X3D)
  HIGH_FREQ_CCD1    = 2, // AMD high-frequency non-V-Cache CCD1
  INTEL_BLC_PCORE   = 3, // Intel P-Core / bLLC high-IPC core
  EFFICIENCY_ECORE  = 4  // Intel E-Core / ARM Little core (background Falline/Landfill GC threads!)
};

struct RedjetTopologySummary {
  uint32_t total_logical_cpus;
  uint32_t vcache_ccd0_mask_count;
  uint32_t high_freq_ccd1_mask_count;
  uint32_t p_core_count;
  uint32_t e_core_count;
  bool     has_amd_3d_vcache_asymmetry;
  bool     has_hybrid_p_e_cores;
  size_t   max_l3_bytes_per_ccx;
};

class RedjetOSAffinity : public AllStatic {
public:
  static void initialize_topology();
  static const RedjetTopologySummary& topology();
  static bool bind_current_thread(RedjetCoreClass target_class, uint32_t lane_hint = 0);
  static bool bind_mutator_for_batch(uint32_t priority, bool sram_focus);
  static bool bind_background_gc_worker(uint32_t worker_id);
  static uint32_t current_logical_cpu();

  static inline uint64_t read_fast_cycles() {
    return REDJET_CYCLE_COUNTER();
  }

  static inline void smt_spin_pause(uint32_t iterations = 1) {
    for (uint32_t i = 0; i < iterations; ++i) {
      REDJET_PAUSE_SMT();
    }
  }
};

#endif // SHARE_GC_REDJET_REDJETOSAFFINITY_HPP
