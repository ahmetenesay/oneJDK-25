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

#include "mirc/mirc_Patcher.hpp"
#include "gc/redjet/allocator/redjetMotherline.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "runtime/atomic.hpp"
#include "runtime/icache.hpp"
#include <cstring>

// ============================================================================
// Slot 16 Atomic Patcher & Method Slot Switch
// ============================================================================

void Overwrite_Slot16(void* slot16_addr, void* new_target_slot) {
  if (slot16_addr == nullptr || new_target_slot == nullptr) return;

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  int32_t rel_offset = (int32_t)((uintptr_t)new_target_slot - ((uintptr_t)slot16_addr + 5));
  uint64_t patch = 0;
  uint8_t* p = (uint8_t*)&patch;
  p[0] = 0xE9; // JMP opcode
  memcpy(&p[1], &rel_offset, 4);
  p[5] = 0x90; p[6] = 0x90; p[7] = 0x90;
  Atomic::store((volatile uint64_t*)slot16_addr, patch);
  ICache::invalidate_range(static_cast<address>(slot16_addr), 8);

#elif defined(__aarch64__) || defined(_M_ARM64)
  // AArch64: Dockpoint Slot 16 trampoline
  int64_t diff = (int64_t)((uintptr_t)new_target_slot - (uintptr_t)slot16_addr);
  if (diff >= -134217728LL && diff <= 134217724LL) {
    uint32_t b_insn = 0x14000000 | (((uint32_t)(diff >> 2)) & 0x03FFFFFF);
    uint32_t nop_insn = 0xD503201F; // nop
    uint64_t patch = ((uint64_t)nop_insn << 32) | (uint64_t)b_insn;
    Atomic::store((volatile uint64_t*)slot16_addr, patch);
  } else {
    // 16-byte trampoline: ldr x16, [pc, #8]; br x16; .quad target
    uint32_t insns[2] = { 0x58000050, 0xD61F0200 };
    memcpy(slot16_addr, insns, 8);
    Atomic::store((volatile uint64_t*)((uintptr_t)slot16_addr + 8), (uint64_t)new_target_slot);
  }
  ICache::invalidate_range(static_cast<address>(slot16_addr), 16);

#elif defined(__riscv)
  // RISC-V: Dockpoint Slot 16 trampoline
  int64_t diff = (int64_t)((uintptr_t)new_target_slot - (uintptr_t)slot16_addr);
  if (diff >= -1048576LL && diff <= 1048574LL) {
    // jal zero, imm
    uint32_t imm20 = (uint32_t)diff;
    uint32_t j_insn = 0x0000006F |
                      ((imm20 & 0x100000) << 11) |
                      ((imm20 & 0x0007FE) << 20) |
                      ((imm20 & 0x000800) << 9)  |
                      (imm20 & 0x0FF000);
    uint32_t nop_insn = 0x00000013; // nop
    uint64_t patch = ((uint64_t)nop_insn << 32) | (uint64_t)j_insn;
    Atomic::store((volatile uint64_t*)slot16_addr, patch);
  } else {
    // 16-byte trampoline: auipc t0, 0; ld t0, 12(t0); jr t0; .quad target
    uint32_t insns[3] = { 0x00000297, 0x00C2B283, 0x00028067 };
    memcpy(slot16_addr, insns, 12);
    Atomic::store((volatile uint64_t*)((uintptr_t)slot16_addr + 12), (uint64_t)new_target_slot);
  }
  ICache::invalidate_range(static_cast<address>(slot16_addr), 16);

#else
  Atomic::store((volatile void**)slot16_addr, new_target_slot);
  ICache::invalidate_range(static_cast<address>(slot16_addr), sizeof(void*));
#endif
}

bool MIRC_Atomic_Switch_Slot(MIRC_Method_Descriptor* desc, uint8_t target_slot_index) {
  if (desc == nullptr || target_slot_index >= 17) return false;
  void* target_code = desc->simd_slots[target_slot_index];
  if (target_code == nullptr) return false;

  if (desc->slot16_trampoline_addr != nullptr) {
    Overwrite_Slot16(desc->slot16_trampoline_addr, target_code);
  }
  desc->active_slot_index = target_slot_index;
  return true;
}

// ============================================================================
// MIRC_Patcher Implementation: Non-STW Hot-Swap Binder
// ============================================================================

bool MIRC_Patcher::apply_hotswap(const mirc_hotswap_delta_t& delta, HS_hotspot_live_header_t* obj) {
  // 1. Binary Patch Code Buffer if patch target is specified
  if (delta.patch_target_pc != nullptr && delta.new_code_entry != nullptr && delta.patch_size > 0) {
    memcpy(delta.patch_target_pc, delta.new_code_entry, delta.patch_size);
    ICache::invalidate_range(static_cast<address>(delta.patch_target_pc), delta.patch_size);
  }

  if (obj == nullptr || obj->hsMN != HS_LIVE_MAGIC) {
    return true; // Patch applied at code level
  }

  // 2. Apply Hot-Swap Patch & SIMD Ladder Update
  if (delta.slot16_overwrite) {
    // Dynamic candidate discovered by Puzzler written directly to Slot 16
    strncpy(obj->simd_table[16].name, delta.winner_isa_name, sizeof(obj->simd_table[16].name) - 1);
    obj->simd_table[16].name[sizeof(obj->simd_table[16].name) - 1] = '\0';
    obj->simd_table[16].ladder_level = 16;
    obj->simd_table[16].is_active = 1;
    obj->active_simd_idx = 16;
    for (uint16_t i = 0; i < 16; ++i) {
      obj->simd_table[i].is_active = 0;
    }
  } else {
    // Existing candidate rung selected
    obj->active_simd_idx = delta.new_simd_idx;
    for (uint16_t i = 0; i < 17; ++i) {
      obj->simd_table[i].is_active = (i == delta.new_simd_idx) ? 1 : 0;
    }
  }

  // 3. Reseal as Fledgling (has_optimized = 1)
  obj->cycles_gained.store(delta.cycles_gained, std::memory_order_release);
  obj->last_optimized_time.store(sacculus_now_ns(), std::memory_order_release);
  obj->opt_success = 1;
  obj->has_optimized.store(1, std::memory_order_release);

  // 4. Verify and write Tail Canary
  if (obj->memory_footprint.load(std::memory_order_relaxed) > 0 && !obj->isTailCanaryValid()) {
    obj->writeTailCanary();
  }

  // 5. Telemetry Emission
  hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, delta.method_id,
                 obj->active_simd_idx, obj->generation, obj->region,
                 0, 0, (uint32_t)delta.cycles_gained, delta.winner_isa_name);

  return true;
}
