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

#ifndef SHARE_REDLET_ENTEXC_REDLETENTANGLEFACADE_HPP
#define SHARE_REDLET_ENTEXC_REDLETENTANGLEFACADE_HPP

/*
 * Architectural Facade for RedletEntangle DAG Kernel Fusion & Object Entanglement.
 * Strictly C++14-safe for inclusion across standard HotSpot translation units.
 */

#ifdef REDLET_STANDALONE_BUILD
#include <stddef.h>
#include <stdint.h>
#else
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include <stddef.h>
#include <stdint.h>
#endif

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

struct SF_custominiVm_tensor_t;

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API void     SF_custominiVm_entangle_begin(void);
MSIMD_API void     SF_custominiVm_entangle_commit(void);
MSIMD_API void     SF_custominiVm_entangle_cancel(void);
MSIMD_API void     SF_custominiVm_entangle_reset(void);
MSIMD_API int      SF_custominiVm_entangle_is_active(void);
MSIMD_API void     SF_custominiVm_entangle_record_op(uint32_t fn_id,
                                                     const SF_custominiVm_tensor_t* inputs, int n_in,
                                                     SF_custominiVm_tensor_t* outputs, int n_out);
MSIMD_API void     SF_custominiVm_entangle_record_gc_fusion(void* cell_ptr, uint32_t prev_fn,
                                                            size_t occupant_size, size_t aligned_size,
                                                            void* fwd_addr, uint64_t sweep_no);
MSIMD_API void     SF_custominiVm_entangle_record_object_entangle(void* obj_a, void* obj_b, uint64_t tag);
MSIMD_API void     SF_custominiVm_entangle_execute_parallel(uint32_t num_workers);
MSIMD_API void     SF_custominiVm_entangle_execute_sequential(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_fused_count(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_dag_depth(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_warmup_hits(void);
MSIMD_API uint32_t SF_custominiVm_entangle_get_superkernel_count(void);
MSIMD_API void     SF_custominiVm_entangle_set_warmup_threshold(uint32_t threshold);
MSIMD_API uint32_t SF_custominiVm_entangle_get_warmup_threshold(void);

#ifdef __cplusplus
}
#endif

#ifdef REDLET_STANDALONE_BUILD
class RedletEntangleFacade {
#else
class RedletEntangleFacade : public AllStatic {
#endif
public:
  static inline void begin_transaction() {
    SF_custominiVm_entangle_begin();
  }

  static inline void commit_transaction() {
    SF_custominiVm_entangle_commit();
  }

  static inline void cancel_transaction() {
    SF_custominiVm_entangle_cancel();
  }

  static inline void reset_engine() {
    SF_custominiVm_entangle_reset();
  }

  static inline bool is_transaction_active() {
    return SF_custominiVm_entangle_is_active() != 0;
  }

  static inline void record_kernel(uint32_t fn_id,
                                   const SF_custominiVm_tensor_t* inputs, int n_in,
                                   SF_custominiVm_tensor_t* outputs, int n_out) {
    SF_custominiVm_entangle_record_op(fn_id, inputs, n_in, outputs, n_out);
  }

  static inline void record_gc_cell_fusion(void* cell_ptr,
                                           uint32_t prev_fn,
                                           size_t occupant_size,
                                           size_t aligned_size,
                                           void* fwd_addr,
                                           uint64_t sweep_no) {
    SF_custominiVm_entangle_record_gc_fusion(cell_ptr, prev_fn, occupant_size, aligned_size, fwd_addr, sweep_no);
  }

  static inline void record_object_link(void* obj_a, void* obj_b, uint64_t tag) {
    SF_custominiVm_entangle_record_object_entangle(obj_a, obj_b, tag);
  }

  static inline void execute_dag_parallel(uint32_t num_workers) {
    SF_custominiVm_entangle_execute_parallel(num_workers);
  }

  static inline void execute_dag_sequential() {
    SF_custominiVm_entangle_execute_sequential();
  }

  static inline uint32_t fused_op_count()    { return SF_custominiVm_entangle_get_fused_count(); }
  static inline uint32_t dag_depth()         { return SF_custominiVm_entangle_get_dag_depth(); }
  static inline uint32_t warmup_hits()       { return SF_custominiVm_entangle_get_warmup_hits(); }
  static inline uint32_t superkernel_count() { return SF_custominiVm_entangle_get_superkernel_count(); }

  static inline void set_warmup_threshold(uint32_t threshold) {
    SF_custominiVm_entangle_set_warmup_threshold(threshold);
  }

  static inline uint32_t warmup_threshold() {
    return SF_custominiVm_entangle_get_warmup_threshold();
  }
};

#ifdef REDLET_STANDALONE_BUILD
class RedletEntangleTransactionGuard {
#else
class RedletEntangleTransactionGuard : public StackObj {
#endif
private:
  bool _active;

  RedletEntangleTransactionGuard(const RedletEntangleTransactionGuard&);
  RedletEntangleTransactionGuard& operator=(const RedletEntangleTransactionGuard&);

public:
  RedletEntangleTransactionGuard() : _active(true) {
    RedletEntangleFacade::begin_transaction();
  }

  ~RedletEntangleTransactionGuard() {
    if (_active) {
      RedletEntangleFacade::commit_transaction();
      _active = false;
    }
  }

  void commit() {
    if (_active) {
      RedletEntangleFacade::commit_transaction();
      _active = false;
    }
  }

  void cancel() {
    if (_active) {
      RedletEntangleFacade::cancel_transaction();
      _active = false;
    }
  }
};

#endif // SHARE_REDLET_ENTEXC_REDLETENTANGLEFACADE_HPP
