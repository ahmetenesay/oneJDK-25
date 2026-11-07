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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETBYTECODEPARSER_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETBYTECODEPARSER_HPP

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
// Redjet Off-Heap Bytecode Pre-Parser & CFG Builder (PLAN.md Section 5.1, 9.1)
// - Ingests sealed class bytecode directly from rawcode_cell
// - Decodes JVM bytecode instructions into Basic Blocks & Control Flow Graph
// - Identifies natural loop back-edges, induction variables, and invoke call sites
// - Supplies 3-address Intermediate Representation (IR) to Puzzler Mod 2
// ============================================================================

enum class RedjetJvmOpcode : uint8_t {
  OP_NOP          = 0x00,
  OP_ACONST_NULL  = 0x01,
  OP_ICONST_0     = 0x03,
  OP_ILOAD        = 0x15,
  OP_LLOAD        = 0x16,
  OP_ALOAD        = 0x19,
  OP_ISTORE       = 0x36,
  OP_ASTORE       = 0x3A,
  OP_IADD         = 0x60,
  OP_LADD         = 0x61,
  OP_IMUL         = 0x68,
  OP_IF_ICMPEQ    = 0x9F,
  OP_IF_ICMPNE    = 0xA0,
  OP_IF_ICMPLT    = 0xA1,
  OP_IF_ICMPGE    = 0xA2,
  OP_GOTO         = 0xA7,
  OP_IRETURN      = 0xAC,
  OP_ARETURN      = 0xB0,
  OP_RETURN       = 0xB1,
  OP_GETFIELD     = 0xB4,
  OP_PUTFIELD     = 0xB5,
  OP_INVOKEVIRTUAL= 0xB6,
  OP_INVOKESPECIAL= 0xB7,
  OP_INVOKESTATIC = 0xB8
};

struct RedjetBasicBlock {
  uint32_t start_bci{ 0 };
  uint32_t end_bci{ 0 };
  uint32_t true_successor_bci{ 0 };
  uint32_t false_successor_bci{ 0 };
  bool     has_loop_backedge{ false };
  uint16_t instruction_count{ 0 };
  bool     contains_invoke{ false };
};

struct RedjetMethodCfg {
  uint32_t         method_id{ 0 };
  size_t           bytecode_size{ 0 };
  size_t           num_blocks{ 0 };
  size_t           num_loops{ 0 };
  RedjetBasicBlock blocks[64];
};

class RedjetBytecodeParser {
private:
  hs_ticket_lock_t      _parser_lock;

  // Telemetry
  std::atomic<uint64_t> _total_methods_parsed{ 0 };
  std::atomic<uint64_t> _total_opcodes_decoded{ 0 };
  std::atomic<uint64_t> _total_blocks_built{ 0 };
  std::atomic<uint64_t> _total_loops_detected{ 0 };

public:
  RedjetBytecodeParser();
  ~RedjetBytecodeParser();

  // 1. Parse Bytecode Buffer and Build CFG
  RJ_API RedjetMethodCfg parse_and_build_cfg(const uint8_t* bytecode, size_t length, uint32_t method_id);

  // 2. Query Opcode Instruction Length
  RJ_API static uint8_t get_instruction_length(uint8_t opcode, const uint8_t* code_ptr) noexcept;

  // Telemetry
  uint64_t methods_parsed()  const noexcept { return _total_methods_parsed.load(std::memory_order_relaxed); }
  uint64_t opcodes_decoded() const noexcept { return _total_opcodes_decoded.load(std::memory_order_relaxed); }
  uint64_t blocks_built()    const noexcept { return _total_blocks_built.load(std::memory_order_relaxed); }
  uint64_t loops_detected()  const noexcept { return _total_loops_detected.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_PUZZLER_REDJETBYTECODEPARSER_HPP
