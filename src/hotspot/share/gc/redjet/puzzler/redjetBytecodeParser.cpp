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

#include "gc/redjet/puzzler/redjetBytecodeParser.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetBytecodeParser::RedjetBytecodeParser()
  : _total_methods_parsed(0),
    _total_opcodes_decoded(0),
    _total_blocks_built(0),
    _total_loops_detected(0) {
  hs_ticket_lock_init(&_parser_lock);
}

RedjetBytecodeParser::~RedjetBytecodeParser() {
}

uint8_t RedjetBytecodeParser::get_instruction_length(uint8_t opcode, const uint8_t* code_ptr) noexcept {
  switch (opcode) {
    // 1-byte opcodes
    case 0x00: // nop
    case 0x01: // aconst_null
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: // iconst_*
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: // iload_*
    case 0x2A: case 0x2B: case 0x2C: case 0x2D: // aload_*
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: // istore_*
    case 0x4B: case 0x4C: case 0x4D: case 0x4E: // astore_*
    case 0x60: // iadd
    case 0x61: // ladd
    case 0x64: // isub
    case 0x68: // imul
    case 0x84: // iinc
    case 0xAC: // ireturn
    case 0xB0: // areturn
    case 0xB1: // return
      return 1;

    // 2-byte opcodes
    case 0x10: // bipush
    case 0x12: // ldc
    case 0x15: // iload
    case 0x16: // lload
    case 0x19: // aload
    case 0x36: // istore
    case 0x37: // lstore
    case 0x3A: // astore
      return 2;

    // 3-byte opcodes
    case 0x11: // sipush
    case 0x13: // ldc_w
    case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: // if<cond>
    case 0x9F: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: // if_icmp<cond>
    case 0xA5: case 0xA6: // if_acmp<cond>
    case 0xA7: // goto
    case 0xB4: // getfield
    case 0xB5: // putfield
    case 0xB6: // invokevirtual
    case 0xB7: // invokespecial
    case 0xB8: // invokestatic
      return 3;

    // 5-byte opcodes
    case 0xB9: // invokeinterface
    case 0xBA: // invokedynamic
      return 5;

    default:
      return 1;
  }
}

RedjetMethodCfg RedjetBytecodeParser::parse_and_build_cfg(const uint8_t* bytecode, size_t length, uint32_t method_id) {
  RedjetMethodCfg cfg;
  cfg.method_id = method_id;
  cfg.bytecode_size = length;
  cfg.num_blocks = 0;
  cfg.num_loops = 0;

  if (bytecode == nullptr || length == 0) return cfg;

  hs_ticket_lock_acquire(&_parser_lock);

  size_t bci = 0;
  uint32_t current_block_start = 0;
  uint16_t current_block_insts = 0;
  bool contains_invoke = false;

  while (bci < length) {
    uint8_t op = bytecode[bci];
    uint8_t len = get_instruction_length(op, &bytecode[bci]);

    current_block_insts++;
    _total_opcodes_decoded.fetch_add(1, std::memory_order_relaxed);

    if (op == 0xB6 || op == 0xB7 || op == 0xB8 || op == 0xB9 || op == 0xBA) {
      contains_invoke = true;
    }

    // Check for control flow split (branch, jump, or return)
    bool is_branch = (op >= 0x99 && op <= 0xA6);
    bool is_goto   = (op == 0xA7);
    bool is_return = (op >= 0xAC && op <= 0xB1);

    if (is_branch || is_goto || is_return || bci + len >= length) {
      if (cfg.num_blocks < 64) {
        RedjetBasicBlock* bb = &cfg.blocks[cfg.num_blocks++];
        bb->start_bci = current_block_start;
        bb->end_bci = static_cast<uint32_t>(bci + len);
        bb->instruction_count = current_block_insts;
        bb->contains_invoke = contains_invoke;
        bb->has_loop_backedge = false;

        if (is_branch && bci + 2 < length) {
          int16_t offset = static_cast<int16_t>((bytecode[bci + 1] << 8) | bytecode[bci + 2]);
          uint32_t target_bci = static_cast<uint32_t>(static_cast<int32_t>(bci) + offset);
          bb->true_successor_bci = target_bci;
          bb->false_successor_bci = static_cast<uint32_t>(bci + len);

          // Detect loop back-edge
          if (target_bci < bci) {
            bb->has_loop_backedge = true;
            cfg.num_loops++;
            _total_loops_detected.fetch_add(1, std::memory_order_relaxed);
          }
        } else if (is_goto && bci + 2 < length) {
          int16_t offset = static_cast<int16_t>((bytecode[bci + 1] << 8) | bytecode[bci + 2]);
          uint32_t target_bci = static_cast<uint32_t>(static_cast<int32_t>(bci) + offset);
          bb->true_successor_bci = target_bci;
          bb->false_successor_bci = target_bci;

          if (target_bci < bci) {
            bb->has_loop_backedge = true;
            cfg.num_loops++;
            _total_loops_detected.fetch_add(1, std::memory_order_relaxed);
          }
        }

        _total_blocks_built.fetch_add(1, std::memory_order_relaxed);
      }

      current_block_start = static_cast<uint32_t>(bci + len);
      current_block_insts = 0;
      contains_invoke = false;
    }

    bci += len;
  }

  _total_methods_parsed.fetch_add(1, std::memory_order_relaxed);
  hs_ticket_lock_release(&_parser_lock);
  return cfg;
}

void RedjetBytecodeParser::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Off-Heap Bytecode Parser & CFG Builder ---");
  st->print_cr("  Methods Parsed:       %llu", static_cast<unsigned long long>(_total_methods_parsed.load(std::memory_order_relaxed)));
  st->print_cr("  Opcodes Decoded:      %llu", static_cast<unsigned long long>(_total_opcodes_decoded.load(std::memory_order_relaxed)));
  st->print_cr("  Basic Blocks Built:   %llu", static_cast<unsigned long long>(_total_blocks_built.load(std::memory_order_relaxed)));
  st->print_cr("  Loops Detected:       %llu", static_cast<unsigned long long>(_total_loops_detected.load(std::memory_order_relaxed)));
}
