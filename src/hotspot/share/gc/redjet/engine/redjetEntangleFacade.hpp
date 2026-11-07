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

#ifndef SHARE_REDJET_ENTEXC_REDJETENTANGLEFACADE_HPP
#define SHARE_REDJET_ENTEXC_REDJETENTANGLEFACADE_HPP

/*
 * Architectural Facade for RedjetEntangle DAG Kernel Fusion & Object Entanglement.
 * Strictly C++14-safe for inclusion across standard HotSpot translation units.
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

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

struct HS_hotspot_tensor_t;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API void     HS_hotspot_entangle_begin(void);
RJ_API void     HS_hotspot_entangle_commit(void);
RJ_API void     HS_hotspot_entangle_cancel(void);
RJ_API void     HS_hotspot_entangle_reset(void);
RJ_API int      HS_hotspot_entangle_is_active(void);
RJ_API void     HS_hotspot_entangle_record_op(uint32_t fn_id,
                                                     const HS_hotspot_tensor_t* inputs, int n_in,
                                                     HS_hotspot_tensor_t* outputs, int n_out);
RJ_API void     HS_hotspot_entangle_record_gc_fusion(void* cell_ptr, uint32_t prev_fn,
                                                            size_t occupant_size, size_t aligned_size,
                                                            void* fwd_addr, uint64_t sweep_no);
RJ_API void     HS_hotspot_entangle_record_object_entangle(void* obj_a, void* obj_b, uint64_t tag);
RJ_API void     HS_hotspot_entangle_execute_parallel(uint32_t num_workers);
RJ_API void     HS_hotspot_entangle_execute_sequential(void);
RJ_API uint32_t HS_hotspot_entangle_get_fused_count(void);
RJ_API uint32_t HS_hotspot_entangle_get_dag_depth(void);
RJ_API uint32_t HS_hotspot_entangle_get_warmup_hits(void);
RJ_API uint32_t HS_hotspot_entangle_get_superkernel_count(void);
RJ_API void     HS_hotspot_entangle_set_warmup_threshold(uint32_t threshold);
RJ_API uint32_t HS_hotspot_entangle_get_warmup_threshold(void);

#ifdef __cplusplus
}
#endif

#ifdef REDJET_STANDALONE_BUILD
class RedjetEntangleFacade {
#else
class RedjetEntangleFacade : public AllStatic {
#endif
public:
  static inline void begin_transaction() {
    HS_hotspot_entangle_begin();
  }

  static inline void commit_transaction() {
    HS_hotspot_entangle_commit();
  }

  static inline void cancel_transaction() {
    HS_hotspot_entangle_cancel();
  }

  static inline void reset_engine() {
    HS_hotspot_entangle_reset();
  }

  static inline bool is_transaction_active() {
    return HS_hotspot_entangle_is_active() != 0;
  }

  static inline void record_kernel(uint32_t fn_id,
                                   const HS_hotspot_tensor_t* inputs, int n_in,
                                   HS_hotspot_tensor_t* outputs, int n_out) {
    HS_hotspot_entangle_record_op(fn_id, inputs, n_in, outputs, n_out);
  }

  static inline void record_gc_cell_fusion(void* cell_ptr,
                                           uint32_t prev_fn,
                                           size_t occupant_size,
                                           size_t aligned_size,
                                           void* fwd_addr,
                                           uint64_t sweep_no) {
    HS_hotspot_entangle_record_gc_fusion(cell_ptr, prev_fn, occupant_size, aligned_size, fwd_addr, sweep_no);
  }

  static inline void record_object_link(void* obj_a, void* obj_b, uint64_t tag) {
    HS_hotspot_entangle_record_object_entangle(obj_a, obj_b, tag);
  }

  static inline void execute_dag_parallel(uint32_t num_workers) {
    HS_hotspot_entangle_execute_parallel(num_workers);
  }

  static inline void execute_dag_sequential() {
    HS_hotspot_entangle_execute_sequential();
  }

  static inline uint32_t fused_op_count()    { return HS_hotspot_entangle_get_fused_count(); }
  static inline uint32_t dag_depth()         { return HS_hotspot_entangle_get_dag_depth(); }
  static inline uint32_t warmup_hits()       { return HS_hotspot_entangle_get_warmup_hits(); }
  static inline uint32_t superkernel_count() { return HS_hotspot_entangle_get_superkernel_count(); }

  static inline void set_warmup_threshold(uint32_t threshold) {
    HS_hotspot_entangle_set_warmup_threshold(threshold);
  }

  static inline uint32_t warmup_threshold() {
    return HS_hotspot_entangle_get_warmup_threshold();
  }
};

#ifdef REDJET_STANDALONE_BUILD
class RedjetEntangleTransactionGuard {
#else
class RedjetEntangleTransactionGuard : public StackObj {
#endif
private:
  bool _active;

  RedjetEntangleTransactionGuard(const RedjetEntangleTransactionGuard&);
  RedjetEntangleTransactionGuard& operator=(const RedjetEntangleTransactionGuard&);

public:
  RedjetEntangleTransactionGuard() : _active(true) {
    RedjetEntangleFacade::begin_transaction();
  }

  ~RedjetEntangleTransactionGuard() {
    if (_active) {
      RedjetEntangleFacade::commit_transaction();
      _active = false;
    }
  }

  void commit() {
    if (_active) {
      RedjetEntangleFacade::commit_transaction();
      _active = false;
    }
  }

  void cancel() {
    if (_active) {
      RedjetEntangleFacade::cancel_transaction();
      _active = false;
    }
  }
};

#endif // SHARE_REDJET_ENTEXC_REDJETENTANGLEFACADE_HPP
