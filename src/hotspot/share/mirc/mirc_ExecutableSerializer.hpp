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

#ifndef SHARE_MIRC_MIRC_EXECUTABLESERIALIZER_HPP
#define SHARE_MIRC_MIRC_EXECUTABLESERIALIZER_HPP

#include <cstddef>
#include <cstdint>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// MIRC_ExecutableSerializer
//
// Standalone Native Binary Image Emitter for `bin/jlc` Ahead-Of-Time / Direct
// Machine Code Compilation:
//   - Windows PE32+ (`.exe`) for x86_64 (`0x8664`) & AArch64 (`0xAA64`)
//   - Linux/POSIX ELF64 (`ET_EXEC`) for x86_64 (`EM_X86_64 = 62`),
//     AArch64 (`EM_AARCH64 = 183`), & RISC-V64 (`EM_RISCV = 243`)
// ============================================================================

enum class MIRC_BinaryFormat : uint8_t {
  PE32_PLUS_WINDOWS = 0,
  ELF64_POSIX       = 1
};

enum class MIRC_TargetArch : uint8_t {
  ARCH_X86_64  = 0,
  ARCH_AARCH64 = 1,
  ARCH_RISCV64 = 2
};

struct MIRC_NativeImageSpec {
  MIRC_BinaryFormat format{ MIRC_BinaryFormat::PE32_PLUS_WINDOWS };
  MIRC_TargetArch   arch{ MIRC_TargetArch::ARCH_X86_64 };
  const uint8_t*    text_bytes{ nullptr };
  size_t            text_size{ 0 };
  uint32_t          entry_offset_in_text{ 0 };
  const uint8_t*    rodata_bytes{ nullptr };
  size_t            rodata_size{ 0 };
  uint64_t          preferred_image_base{ 0x0000000140000000ULL };
};

class MIRC_ExecutableSerializer {
public:
  // Computes exact output buffer capacity required to serialize `spec`
  RJ_API static size_t compute_serialized_size(const MIRC_NativeImageSpec& spec) noexcept;

  // Serializes a complete PE32+ or ELF64 executable into `out_buf` (returns bytes written, 0 on error)
  RJ_API static size_t serialize_to_buffer(const MIRC_NativeImageSpec& spec,
                                           uint8_t* out_buf,
                                           size_t out_capacity) noexcept;

  // Writes a complete PE32+ or ELF64 executable directly to `output_file_path`
  RJ_API static bool write_executable_file(const MIRC_NativeImageSpec& spec,
                                           const char* output_file_path) noexcept;
};

#endif // SHARE_MIRC_MIRC_EXECUTABLESERIALIZER_HPP
