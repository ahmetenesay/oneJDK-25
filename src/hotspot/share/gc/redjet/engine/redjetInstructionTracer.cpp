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

#include "gc/redjet/engine/redjetInstructionTracer.hpp"
#include <cstdio>
#include <cstdlib>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetInstructionTracer* RedjetInstructionTracer::_instance = nullptr;
std::atomic<bool>        RedjetInstructionTracer::_initialized{ false };

RedjetInstructionTracer::RedjetInstructionTracer()
  : _total_instructions_decoded(0),
    _total_blocks_analyzed(0),
    _total_fusions_detected(0) {
  hs_ticket_lock_init(&_tracer_lock);
}

RedjetInstructionTracer::~RedjetInstructionTracer() {
  // Release resources
}

void RedjetInstructionTracer::initialize() {
  if (!_initialized.exchange(true, std::memory_order_acq_rel)) {
    _instance = new RedjetInstructionTracer();
  }
}

void RedjetInstructionTracer::destroy() {
  if (_initialized.exchange(false, std::memory_order_acq_rel)) {
    if (_instance != nullptr) {
      delete _instance;
      _instance = nullptr;
    }
  }
}

RedjetDecodedInstr RedjetInstructionTracer::decode_single(const uint8_t* code_ptr, size_t max_bytes) noexcept {
  RedjetDecodedInstr instr;
  instr.address = code_ptr;
  instr.length = 1;
  instr.primary_opcode = 0;
  instr.category = RedjetInstrCategory::INSTR_UNKNOWN;
  instr.is_macro_fusible = false;
  instr.uses_rip_relative = false;
  instr.has_lock_prefix = false;
  instr.disp = 0;

  if (code_ptr == nullptr || max_bytes == 0) return instr;

  size_t idx = 0;

  // 1. Consume Legacy Prefixes
  while (idx < max_bytes) {
    uint8_t byte = code_ptr[idx];
    if (byte == 0xF0) { // LOCK
      instr.has_lock_prefix = true;
      instr.category = RedjetInstrCategory::INSTR_ATOMIC_LOCK;
      idx++;
    } else if (byte == 0xF2 || byte == 0xF3 || byte == 0x66 || byte == 0x67 ||
               byte == 0x2E || byte == 0x36 || byte == 0x3E || byte == 0x26 ||
               byte == 0x64 || byte == 0x65) {
      idx++;
    } else {
      break;
    }
  }

  if (idx >= max_bytes) {
    instr.length = idx > 0 ? idx : 1;
    return instr;
  }

  // 2. EVEX Prefix (AVX-512)
  if (code_ptr[idx] == 0x62 && idx + 4 < max_bytes) {
    instr.category = RedjetInstrCategory::INSTR_VECTOR_AVX512;
    instr.primary_opcode = code_ptr[idx + 4];
    instr.length = idx + 6 <= max_bytes ? idx + 6 : max_bytes;
    _total_instructions_decoded.fetch_add(1, std::memory_order_relaxed);
    return instr;
  }

  // 3. VEX Prefix (AVX / AVX2)
  if (code_ptr[idx] == 0xC5 && idx + 2 < max_bytes) {
    instr.category = RedjetInstrCategory::INSTR_VECTOR_AVX2;
    instr.primary_opcode = code_ptr[idx + 2];
    instr.length = idx + 4 <= max_bytes ? idx + 4 : max_bytes;
    _total_instructions_decoded.fetch_add(1, std::memory_order_relaxed);
    return instr;
  }
  if (code_ptr[idx] == 0xC4 && idx + 3 < max_bytes) {
    instr.category = RedjetInstrCategory::INSTR_VECTOR_AVX2;
    instr.primary_opcode = code_ptr[idx + 3];
    instr.length = idx + 5 <= max_bytes ? idx + 5 : max_bytes;
    _total_instructions_decoded.fetch_add(1, std::memory_order_relaxed);
    return instr;
  }

  // 4. REX Prefix (x86-64)
  if ((code_ptr[idx] & 0xF0) == 0x40) {
    idx++;
    if (idx >= max_bytes) {
      instr.length = idx;
      return instr;
    }
  }

  // 5. Opcode Decoding
  uint8_t op = code_ptr[idx++];
  instr.primary_opcode = op;

  if (op == 0x0F) { // 2-byte opcode escape
    if (idx >= max_bytes) {
      instr.length = idx;
      return instr;
    }
    uint8_t op2 = code_ptr[idx++];
    if (op2 >= 0x80 && op2 <= 0x8F) { // Jcc rel32
      instr.category = RedjetInstrCategory::INSTR_BRANCH_COND;
      instr.length = idx + 4 <= max_bytes ? idx + 4 : max_bytes;
    } else if (op2 == 0x10 || op2 == 0x28) { // MOVUPS / MOVAPS (Load)
      instr.category = RedjetInstrCategory::INSTR_MEMORY_LOAD;
      instr.length = idx + 3 <= max_bytes ? idx + 3 : max_bytes;
    } else if (op2 == 0x11 || op2 == 0x29) { // MOVUPS / MOVAPS (Store)
      instr.category = RedjetInstrCategory::INSTR_MEMORY_STORE;
      instr.length = idx + 3 <= max_bytes ? idx + 3 : max_bytes;
    } else {
      instr.category = RedjetInstrCategory::INSTR_ALU_INTEGER;
      instr.length = idx + 2 <= max_bytes ? idx + 2 : max_bytes;
    }
  } else if (op >= 0x70 && op <= 0x7F) { // Jcc rel8
    instr.category = RedjetInstrCategory::INSTR_BRANCH_COND;
    instr.length = idx + 1 <= max_bytes ? idx + 1 : max_bytes;
  } else if (op == 0xEB) { // JMP rel8
    instr.category = RedjetInstrCategory::INSTR_BRANCH_UNCOND;
    instr.length = idx + 1 <= max_bytes ? idx + 1 : max_bytes;
  } else if (op == 0xE9) { // JMP rel32
    instr.category = RedjetInstrCategory::INSTR_BRANCH_UNCOND;
    instr.length = idx + 4 <= max_bytes ? idx + 4 : max_bytes;
  } else if (op == 0x85 || op == 0x84) { // TEST
    instr.category = RedjetInstrCategory::INSTR_ALU_INTEGER;
    instr.is_macro_fusible = true;
    instr.length = idx + 1 <= max_bytes ? idx + 1 : max_bytes;
  } else if (op == 0x39 || op == 0x3B || op == 0x3D) { // CMP
    instr.category = RedjetInstrCategory::INSTR_ALU_INTEGER;
    instr.is_macro_fusible = true;
    instr.length = idx + (op == 0x3D ? 4 : 1) <= max_bytes ? idx + (op == 0x3D ? 4 : 1) : max_bytes;
  } else if (op == 0x8B) { // MOV load
    instr.category = RedjetInstrCategory::INSTR_MEMORY_LOAD;
    instr.length = idx + 2 <= max_bytes ? idx + 2 : max_bytes;
  } else if (op == 0x89) { // MOV store
    instr.category = RedjetInstrCategory::INSTR_MEMORY_STORE;
    instr.length = idx + 2 <= max_bytes ? idx + 2 : max_bytes;
  } else if (op == 0x90) { // NOP
    instr.category = RedjetInstrCategory::INSTR_ALU_INTEGER;
    instr.length = idx;
  } else {
    instr.category = RedjetInstrCategory::INSTR_ALU_INTEGER;
    instr.length = idx + 1 <= max_bytes ? idx + 1 : max_bytes;
  }

  _total_instructions_decoded.fetch_add(1, std::memory_order_relaxed);
  return instr;
}

bool RedjetInstructionTracer::can_macro_fuse(const RedjetDecodedInstr& prev, const RedjetDecodedInstr& curr) noexcept {
  return prev.is_macro_fusible && (curr.category == RedjetInstrCategory::INSTR_BRANCH_COND);
}

RedjetCodeBlockAnalysis RedjetInstructionTracer::analyze_block(const uint8_t* code_start, size_t max_bytes) {
  RedjetCodeBlockAnalysis analysis;
  if (code_start == nullptr || max_bytes == 0) return analysis;

  _total_blocks_analyzed.fetch_add(1, std::memory_order_relaxed);

  size_t offset = 0;
  RedjetDecodedInstr prev_instr;
  bool has_prev = false;

  while (offset < max_bytes && analysis.instruction_count < MAX_DECODED_WINDOW) {
    RedjetDecodedInstr curr = decode_single(code_start + offset, max_bytes - offset);
    if (curr.length == 0) break;

    analysis.instruction_count++;
    analysis.total_bytes += curr.length;

    switch (curr.category) {
      case RedjetInstrCategory::INSTR_MEMORY_LOAD:
        analysis.load_count++;
        break;
      case RedjetInstrCategory::INSTR_MEMORY_STORE:
        analysis.store_count++;
        break;
      case RedjetInstrCategory::INSTR_BRANCH_COND:
      case RedjetInstrCategory::INSTR_BRANCH_UNCOND:
        analysis.branch_count++;
        break;
      case RedjetInstrCategory::INSTR_VECTOR_AVX2:
      case RedjetInstrCategory::INSTR_VECTOR_AVX512:
        analysis.vector_count++;
        break;
      default:
        break;
    }

    if (has_prev && can_macro_fuse(prev_instr, curr)) {
      analysis.macro_fusions_found++;
      _total_fusions_detected.fetch_add(1, std::memory_order_relaxed);
    }

    // Stop analysis if hit unconditional branch
    if (curr.category == RedjetInstrCategory::INSTR_BRANCH_UNCOND) {
      break;
    }

    prev_instr = curr;
    has_prev = true;
    offset += curr.length;
  }

  return analysis;
}

void RedjetInstructionTracer::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("[RedjetInstructionTracer] Opcode Decoder & Macro-Fusion Status:");
  st->print_cr("  - Instructions Decoded:  %llu",
               static_cast<unsigned long long>(_total_instructions_decoded.load(std::memory_order_relaxed)));
  st->print_cr("  - Code Blocks Analyzed:  %llu",
               static_cast<unsigned long long>(_total_blocks_analyzed.load(std::memory_order_relaxed)));
  st->print_cr("  - Macro-Fusions Found:   %llu fused branch pairs",
               static_cast<unsigned long long>(_total_fusions_detected.load(std::memory_order_relaxed)));
}
