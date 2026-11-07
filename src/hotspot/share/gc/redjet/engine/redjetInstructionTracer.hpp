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

#ifndef SHARE_GC_REDJET_ENGINE_REDJETINSTRUCTIONTRACER_HPP
#define SHARE_GC_REDJET_ENGINE_REDJETINSTRUCTIONTRACER_HPP

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
// Redjet Hardware Instruction Tracer & Macro-Fusion Decoder (PLAN.md Section 3.1, 5.1)
// - Fast opcode length decoder & instruction classification for JIT/AOT code chunks
// - Detects x86-64 branch macro-fusion candidates (CMP/TEST + Jcc)
// - Identifies vector execution opportunities (SSE -> AVX2 -> AVX-512)
// - Evaluates memory access patterns (loads, stores, non-temporal streaming hints)
// ============================================================================

enum class RedjetInstrCategory : uint8_t {
  INSTR_UNKNOWN      = 0,
  INSTR_ALU_INTEGER  = 1,
  INSTR_MEMORY_LOAD  = 2,
  INSTR_MEMORY_STORE = 3,
  INSTR_BRANCH_COND  = 4,
  INSTR_BRANCH_UNCOND= 5,
  INSTR_VECTOR_AVX2  = 6,
  INSTR_VECTOR_AVX512= 7,
  INSTR_ATOMIC_LOCK  = 8
};

struct RedjetDecodedInstr {
  const uint8_t*      address{ nullptr };
  size_t              length{ 0 };
  uint8_t             primary_opcode{ 0 };
  RedjetInstrCategory category{ RedjetInstrCategory::INSTR_UNKNOWN };
  bool                is_macro_fusible{ false };
  bool                uses_rip_relative{ false };
  bool                has_lock_prefix{ false };
  int32_t             disp{ 0 };
};

struct RedjetCodeBlockAnalysis {
  size_t              instruction_count{ 0 };
  size_t              total_bytes{ 0 };
  size_t              load_count{ 0 };
  size_t              store_count{ 0 };
  size_t              branch_count{ 0 };
  size_t              vector_count{ 0 };
  size_t              macro_fusions_found{ 0 };
};

class RedjetInstructionTracer {
public:
  static constexpr size_t MAX_DECODED_WINDOW = 512;

private:
  static RedjetInstructionTracer* _instance;
  static std::atomic<bool>        _initialized;

  hs_ticket_lock_t                _tracer_lock;

  // Telemetry
  std::atomic<uint64_t>           _total_instructions_decoded{ 0 };
  std::atomic<uint64_t>           _total_blocks_analyzed{ 0 };
  std::atomic<uint64_t>           _total_fusions_detected{ 0 };

public:
  RedjetInstructionTracer();
  ~RedjetInstructionTracer();

  static void initialize();
  static void destroy();
  static RedjetInstructionTracer* get() noexcept { return _instance; }

  // 1. Single Instruction Length & Category Decoder
  RJ_API RedjetDecodedInstr decode_single(const uint8_t* code_ptr, size_t max_bytes) noexcept;

  // 2. Continuous Basic Block Analysis
  RJ_API RedjetCodeBlockAnalysis analyze_block(const uint8_t* code_start, size_t max_bytes);

  // 3. Macro-Fusion Evaluation
  RJ_API static bool can_macro_fuse(const RedjetDecodedInstr& prev, const RedjetDecodedInstr& curr) noexcept;

  // Telemetry queries
  uint64_t instructions_decoded() const noexcept { return _total_instructions_decoded.load(std::memory_order_relaxed); }
  uint64_t blocks_analyzed()       const noexcept { return _total_blocks_analyzed.load(std::memory_order_relaxed); }
  uint64_t fusions_detected()      const noexcept { return _total_fusions_detected.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_ENGINE_REDJETINSTRUCTIONTRACER_HPP
