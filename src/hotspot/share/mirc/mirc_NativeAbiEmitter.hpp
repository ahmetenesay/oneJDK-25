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

#ifndef SHARE_MIRC_MIRC_NATIVEABIEMITTER_HPP
#define SHARE_MIRC_MIRC_NATIVEABIEMITTER_HPP

#include <cstdint>
#include <cstddef>
#include "utilities/globalDefinitions.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

class CodeBuffer;
class MacroAssembler;

enum MIRCNativeAbiConvention {
  ABI_WIN_X64      = 0, // RCX, RDX, R8, R9, XMM0-3, 32-byte shadow space
  ABI_SYSV_AMD64   = 1, // RDI, RSI, RDX, RCX, R8, R9, XMM0-7
  ABI_AAPCS64      = 2, // X0-X7, V0-V7
  ABI_RISCV_LP64D  = 3  // A0-A7, FA0-FA7
};

struct MIRCNativeAbiCallDesc {
  MIRCNativeAbiConvention convention;
  uint32_t num_gpr_args;
  uint32_t num_fp_args;
  uint32_t total_stack_bytes;
  BasicType return_type;
  void* native_func_target;
  void* slot16_upcall_target;
};

// ============================================================================
// MIRC Native ABI Emitter: High-Throughput Downcall & Upcall Stub Emitter
// ============================================================================

class MIRCNativeAbiEmitter {
public:
  // Detects the host native ABI convention
  RJ_API static MIRCNativeAbiConvention host_abi_convention();

  // Emits downcall trampoline (Java caller -> native target)
  RJ_API static int emit_downcall_stub(CodeBuffer* cbuf, const MIRCNativeAbiCallDesc& desc);

  // Emits upcall trampoline (Native caller -> Java Slot 16 trampoline)
  RJ_API static int emit_upcall_stub(CodeBuffer* cbuf, const MIRCNativeAbiCallDesc& desc);
};

#endif // SHARE_MIRC_MIRC_NATIVEABIEMITTER_HPP
