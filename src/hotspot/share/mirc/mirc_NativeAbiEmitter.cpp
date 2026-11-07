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

#include "mirc/mirc_NativeAbiEmitter.hpp"
#include "code/codeBuffer.hpp"
#include "runtime/icache.hpp"

MIRCNativeAbiConvention MIRCNativeAbiEmitter::host_abi_convention() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
  return ABI_WIN_X64;
#elif defined(__x86_64__)
  return ABI_SYSV_AMD64;
#elif defined(__aarch64__) || defined(_M_ARM64)
  return ABI_AAPCS64;
#elif defined(__riscv)
  return ABI_RISCV_LP64D;
#else
  return ABI_WIN_X64;
#endif
}

int MIRCNativeAbiEmitter::emit_downcall_stub(CodeBuffer* cbuf, const MIRCNativeAbiCallDesc& desc) {
  if (cbuf == nullptr || desc.native_func_target == nullptr) {
    return -1;
  }

  address start_pc = cbuf->insts_end();

  // Emitting native machine binary sequence for downcall wrapper
#if defined(_M_X64) || defined(__x86_64__)
  // Win x64 / SysV AMD64:
  // 1. push rbp; mov rbp, rsp
  // 2. sub rsp, shadow_space + aligned_stack
  // 3. call target
  // 4. mov rsp, rbp; pop rbp; ret
  uint32_t stack_alloc = 32 + desc.total_stack_bytes;
  if ((stack_alloc % 16) != 0) {
    stack_alloc += (16 - (stack_alloc % 16));
  }

  uint8_t prologue[] = {
    0x55,                         // push rbp
    0x48, 0x89, 0xE5,             // mov rbp, rsp
    0x48, 0x81, 0xEC, 0x00, 0x00, 0x00, 0x00 // sub rsp, stack_alloc
  };
  memcpy(&prologue[7], &stack_alloc, sizeof(uint32_t));
  cbuf->insts()->emit_bytes(prologue, sizeof(prologue));

  // mov rax, native_func_target; call rax
  uint64_t target_addr = (uint64_t)desc.native_func_target;
  uint8_t call_seq[] = {
    0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, imm64
    0xFF, 0xD0                                                   // call rax
  };
  memcpy(&call_seq[2], &target_addr, sizeof(uint64_t));
  cbuf->insts()->emit_bytes(call_seq, sizeof(call_seq));

  // epilogue
  uint8_t epilogue[] = {
    0x48, 0x89, 0xEC, // mov rsp, rbp
    0x5D,             // pop rbp
    0xC3              // ret
  };
  cbuf->insts()->emit_bytes(epilogue, sizeof(epilogue));

#elif defined(__aarch64__) || defined(_M_ARM64)
  // AArch64:
  // 1. stp x29, x30, [sp, #-16]!
  // 2. mov x29, sp
  // 3. ldr x16, [pc, #16] (loads target_addr directly from literal pool at PC + 16)
  // 4. blr x16
  // 5. ldp x29, x30, [sp], #16
  // 6. ret
  // 7. .quad target_addr
  uint32_t aarch64_code[] = {
    0xA9BF7BFD, // stp x29, x30, [sp, #-16]!
    0x910003FD, // mov x29, sp
    0x58000090, // ldr x16, [pc, #16]
    0xD63F0200, // blr x16
    0xA8C17BFD, // ldp x29, x30, [sp], #16
    0xD65F03C0  // ret
  };
  cbuf->insts()->emit_bytes((uint8_t*)aarch64_code, sizeof(aarch64_code));
  uint64_t target_addr = (uint64_t)desc.native_func_target;
  cbuf->insts()->emit_bytes((uint8_t*)&target_addr, sizeof(uint64_t));

#elif defined(__riscv)
  // RISC-V:
  // 1. addi sp, sp, -16
  // 2. sd ra, 8(sp); sd s0, 0(sp); addi s0, sp, 16
  // 3. auipc t0, 0
  // 4. ld t0, 20(t0) (loads target_addr from PC + 20)
  // 5. jalr ra, 0(t0)
  // 6. ld s0, 0(sp); ld ra, 8(sp); addi sp, sp, 16
  // 7. ret
  // 8. .quad target_addr
  uint32_t riscv_code[] = {
    0x11417113, // addi sp, sp, -16
    0x00113423, // sd ra, 8(sp)
    0x00813023, // sd s0, 0(sp)
    0x01010413, // addi s0, sp, 16
    0x00000297, // auipc t0, 0
    0x0142B283, // ld t0, 20(t0)
    0x000280E7, // jalr ra, 0(t0)
    0x00013403, // ld s0, 0(sp)
    0x00813083, // ld ra, 8(sp)
    0x01010113, // addi sp, sp, 16
    0x00008067  // ret
  };
  cbuf->insts()->emit_bytes((uint8_t*)riscv_code, sizeof(riscv_code));
  uint64_t target_addr = (uint64_t)desc.native_func_target;
  cbuf->insts()->emit_bytes((uint8_t*)&target_addr, sizeof(uint64_t));
#endif

  address end_pc = cbuf->insts_end();
  ICache::invalidate_range(start_pc, end_pc - start_pc);
  return (int)(end_pc - start_pc);
}

int MIRCNativeAbiEmitter::emit_upcall_stub(CodeBuffer* cbuf, const MIRCNativeAbiCallDesc& desc) {
  if (cbuf == nullptr || desc.slot16_upcall_target == nullptr) {
    return -1;
  }

  address start_pc = cbuf->insts_end();

#if defined(_M_X64) || defined(__x86_64__)
  // Upcall direct jump to Slot 16 trampoline:
  // mov r11, slot16_target; jmp r11
  uint64_t target_addr = (uint64_t)desc.slot16_upcall_target;
  uint8_t jump_seq[] = {
    0x49, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov r11, imm64
    0x41, 0xFF, 0xE3                                             // jmp r11
  };
  memcpy(&jump_seq[2], &target_addr, sizeof(uint64_t));
  cbuf->insts()->emit_bytes(jump_seq, sizeof(jump_seq));

#elif defined(__aarch64__) || defined(_M_ARM64)
  // AArch64:
  // 1. ldr x16, [pc, #8]
  // 2. br x16
  // 3. .quad target_addr
  uint32_t aarch64_code[] = {
    0x58000050, // ldr x16, [pc, #8]
    0xD61F0200  // br x16
  };
  cbuf->insts()->emit_bytes((uint8_t*)aarch64_code, sizeof(aarch64_code));
  uint64_t target_addr = (uint64_t)desc.slot16_upcall_target;
  cbuf->insts()->emit_bytes((uint8_t*)&target_addr, sizeof(uint64_t));

#elif defined(__riscv)
  // RISC-V:
  // 1. auipc t0, 0
  // 2. ld t0, 12(t0)
  // 3. jr t0
  // 4. .quad target_addr
  uint32_t riscv_code[] = {
    0x00000297, // auipc t0, 0
    0x00C2B283, // ld t0, 12(t0)
    0x00028067  // jr t0
  };
  cbuf->insts()->emit_bytes((uint8_t*)riscv_code, sizeof(riscv_code));
  uint64_t target_addr = (uint64_t)desc.slot16_upcall_target;
  cbuf->insts()->emit_bytes((uint8_t*)&target_addr, sizeof(uint64_t));
#endif

  address end_pc = cbuf->insts_end();
  ICache::invalidate_range(start_pc, end_pc - start_pc);
  return (int)(end_pc - start_pc);
}
