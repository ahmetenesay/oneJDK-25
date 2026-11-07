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

#ifndef SHARE_REDJET_DISPATCH_REDJETDISPATCHFACADE_HPP
#define SHARE_REDJET_DISPATCH_REDJETDISPATCHFACADE_HPP

/*
 * Architectural Facade for RedjetDispatchISA Vector ALU & ISA Catalog Routing.
 * Strictly C++14-safe: avoids pulling C++20-only internal headers into HotSpot C++14 TUs.
 */

#ifdef REDJET_STANDALONE_BUILD
#include <stddef.h>
#include <stdint.h>
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include <stddef.h>
#include <stdint.h>
#endif

#include "gc/redjet/engine/redjetBatchScope.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

struct hs_world_isa_entry_t;

RJ_API int hs_dispatch_fast_path_arithmetic(uint32_t op_id, uint8_t dtype,
                                               const void* a, const void* b, void* out,
                                               size_t count, uint8_t priority);

const hs_world_isa_entry_t* hs_find_world_isa(const char* name) noexcept;
size_t                      hs_world_isa_catalog_count() noexcept;
const hs_world_isa_entry_t* hs_world_isa_catalog_at(size_t idx) noexcept;
size_t                      hs_resolve_isa_fallback_chain(const char* start_isa,
                                                          const hs_world_isa_entry_t** out_chain,
                                                          size_t max_depth) noexcept;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API uint64_t hs_rdtsc_begin(void);
RJ_API uint64_t hs_rdtsc_end(void);
RJ_API uint64_t hs_rdtsc_freq_hz(void);
RJ_API uint64_t hs_morton_encode3d(uint32_t x, uint32_t y, uint32_t z);

#ifdef __cplusplus
}
#endif

#ifdef REDJET_STANDALONE_BUILD
class RedjetDispatchFacade {
#else
class RedjetDispatchFacade : public AllStatic {
#endif
public:
  enum VectorOp {
    VEC_OP_ADD             = 0x0001u,
    VEC_OP_SUB             = 0x0002u,
    VEC_OP_MUL             = 0x0003u,
    VEC_OP_DIV             = 0x0004u,
    VEC_OP_FMA             = 0x0005u,
    VEC_OP_DOT             = 0x0006u,
    VEC_OP_MIN             = 0x0007u,
    VEC_OP_MAX             = 0x0008u,
    VEC_OP_AND             = 0x0009u,
    VEC_OP_OR              = 0x000Au,
    VEC_OP_XOR             = 0x000Bu,
    VEC_OP_MULTI_LIMB_ADD  = 0x0010u,
    VEC_OP_MULTI_LIMB_MUL  = 0x0011u,
    VEC_OP_CARD_TABLE_SCAN = 0x0020u,
    VEC_OP_MEMZERO_FAST    = 0x0021u
  };

  static inline int dispatch_arithmetic(uint32_t op_id,
                                        uint8_t dtype,
                                        const void* a,
                                        const void* b,
                                        void* out,
                                        size_t count) {
    const uint8_t prio = static_cast<uint8_t>(RedjetBatchScopeManager::current_priority());
    return hs_dispatch_fast_path_arithmetic(op_id, dtype, a, b, out, count, prio);
  }

  static inline int dispatch_arithmetic_with_priority(uint32_t op_id,
                                                      uint8_t dtype,
                                                      const void* a,
                                                      const void* b,
                                                      void* out,
                                                      size_t count,
                                                      uint8_t priority) {
    return hs_dispatch_fast_path_arithmetic(op_id, dtype, a, b, out, count, priority);
  }

  static inline const hs_world_isa_entry_t* lookup_isa(const char* isa_name) {
    return hs_find_world_isa(isa_name);
  }

  static inline size_t isa_catalog_size() {
    return hs_world_isa_catalog_count();
  }

  static inline const hs_world_isa_entry_t* isa_catalog_entry(size_t idx) {
    return hs_world_isa_catalog_at(idx);
  }

  static inline size_t resolve_fallback_chain(const char* start_isa,
                                              const hs_world_isa_entry_t** out_chain,
                                              size_t max_depth) {
    return hs_resolve_isa_fallback_chain(start_isa, out_chain, max_depth);
  }

  static inline uint64_t rdtsc_begin() { return hs_rdtsc_begin(); }
  static inline uint64_t rdtsc_end()   { return hs_rdtsc_end(); }
  static inline uint64_t morton3d(uint32_t x, uint32_t y, uint32_t z) {
    return hs_morton_encode3d(x, y, z);
  }
};

#endif // SHARE_REDJET_DISPATCH_REDJETDISPATCHFACADE_HPP
