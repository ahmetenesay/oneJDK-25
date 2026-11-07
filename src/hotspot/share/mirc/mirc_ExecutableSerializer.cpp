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

#include "mirc/mirc_ExecutableSerializer.hpp"
#include "memory/allocation.hpp"

#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
static inline size_t align_up_sz(size_t val, size_t align) noexcept {
  return (val + align - 1) & ~(align - 1);
}

static inline void write_u16_le(uint8_t* p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

static inline void write_u32_le(uint8_t* p, uint32_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

static inline void write_u64_le(uint8_t* p, uint64_t v) noexcept {
  write_u32_le(p, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
  write_u32_le(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

static constexpr size_t PE_FILE_ALIGN = 512;
static constexpr size_t PE_SECT_ALIGN = 4096;
static constexpr size_t ELF_PAGE_ALIGN = 4096;
static const char ELF_SHSTRTAB[] = "\0.text\0.rodata\0.shstrtab\0";

static size_t serialize_pe32_plus(const MIRC_NativeImageSpec& spec,
                                  uint8_t* out_buf,
                                  size_t out_capacity) noexcept {
  const bool has_rdata       = (spec.rodata_bytes != nullptr && spec.rodata_size > 0);
  const uint16_t num_sects   = has_rdata ? 2u : 1u;
  const size_t headers_raw   = PE_FILE_ALIGN; // 512B covers DOS(64) + NT(264) + 2*Sect(80) = 408B
  const size_t text_raw_sz   = align_up_sz(spec.text_size > 0 ? spec.text_size : 1, PE_FILE_ALIGN);
  const size_t rdata_raw_sz  = has_rdata ? align_up_sz(spec.rodata_size, PE_FILE_ALIGN) : 0;
  const size_t total_file_sz = headers_raw + text_raw_sz + rdata_raw_sz;

  if (out_buf == nullptr) return total_file_sz;
  if (out_capacity < total_file_sz) return 0;

  std::memset(out_buf, 0, total_file_sz);

  // 1. IMAGE_DOS_HEADER (64 bytes)
  write_u16_le(out_buf + 0x00, 0x5A4Du); // "MZ"
  write_u32_le(out_buf + 0x3C, 0x0040u); // e_lfanew = 64

  // 2. IMAGE_NT_HEADERS64 at offset 0x40 (4 + 20 + 240 = 264 bytes)
  uint8_t* nt = out_buf + 0x40;
  write_u32_le(nt + 0x00, 0x00004550u); // "PE\0\0"

  const uint16_t machine = (spec.arch == MIRC_TargetArch::ARCH_AARCH64) ? 0xAA64u : 0x8664u;
  write_u16_le(nt + 0x04, machine);
  write_u16_le(nt + 0x06, num_sects);
  write_u32_le(nt + 0x08, 0x672DF00Du); // Deterministic timestamp
  write_u16_le(nt + 0x14, 240u);        // SizeOfOptionalHeader (PE32+)
  write_u16_le(nt + 0x16, 0x0022u);     // IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE

  // 3. IMAGE_OPTIONAL_HEADER64 at nt + 0x18
  uint8_t* opt = nt + 0x18;
  const uint32_t text_rva  = static_cast<uint32_t>(PE_SECT_ALIGN);
  const uint32_t text_vsz  = static_cast<uint32_t>(align_up_sz(spec.text_size > 0 ? spec.text_size : 1, PE_SECT_ALIGN));
  const uint32_t rdata_rva = text_rva + text_vsz;
  const uint32_t rdata_vsz = has_rdata ? static_cast<uint32_t>(align_up_sz(spec.rodata_size, PE_SECT_ALIGN)) : 0u;
  const uint32_t image_sz  = rdata_rva + rdata_vsz;

  write_u16_le(opt + 0x00, 0x020Bu); // PE32+ Magic
  opt[0x02] = 14;                    // MajorLinkerVersion
  write_u32_le(opt + 0x04, static_cast<uint32_t>(text_raw_sz));
  write_u32_le(opt + 0x08, static_cast<uint32_t>(rdata_raw_sz));
  write_u32_le(opt + 0x10, text_rva + spec.entry_offset_in_text); // AddressOfEntryPoint
  write_u32_le(opt + 0x14, text_rva);                             // BaseOfCode
  write_u64_le(opt + 0x18, spec.preferred_image_base != 0 ? spec.preferred_image_base : 0x140000000ULL);
  write_u32_le(opt + 0x20, static_cast<uint32_t>(PE_SECT_ALIGN));
  write_u32_le(opt + 0x24, static_cast<uint32_t>(PE_FILE_ALIGN));
  write_u16_le(opt + 0x28, 6u);      // MajorOperatingSystemVersion (Win10/11+)
  write_u16_le(opt + 0x30, 6u);      // MajorSubsystemVersion
  write_u32_le(opt + 0x38, image_sz);
  write_u32_le(opt + 0x3C, static_cast<uint32_t>(headers_raw));
  write_u16_le(opt + 0x44, 3u);      // IMAGE_SUBSYSTEM_WINDOWS_CUI
  write_u16_le(opt + 0x46, 0x8160u); // HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT | TERMINAL_SERVER_AWARE
  write_u64_le(opt + 0x48, 0x00100000ULL); // SizeOfStackReserve (1 MB)
  write_u64_le(opt + 0x50, 0x00010000ULL); // SizeOfStackCommit  (64 KB)
  write_u64_le(opt + 0x58, 0x00100000ULL); // SizeOfHeapReserve  (1 MB)
  write_u64_le(opt + 0x60, 0x00010000ULL); // SizeOfHeapCommit   (64 KB)
  write_u32_le(opt + 0x6C, 16u);           // NumberOfRvaAndSizes

  // 4. Section Table at opt + 240 (40 bytes each)
  uint8_t* sh_text = opt + 240;
  std::memcpy(sh_text + 0x00, ".text\0\0\0", 8);
  write_u32_le(sh_text + 0x08, static_cast<uint32_t>(spec.text_size > 0 ? spec.text_size : 1));
  write_u32_le(sh_text + 0x0C, text_rva);
  write_u32_le(sh_text + 0x10, static_cast<uint32_t>(text_raw_sz));
  write_u32_le(sh_text + 0x14, static_cast<uint32_t>(headers_raw));
  write_u32_le(sh_text + 0x24, 0x60000020u); // CNT_CODE | MEM_EXECUTE | MEM_READ

  if (has_rdata) {
    uint8_t* sh_rd = sh_text + 40;
    std::memcpy(sh_rd + 0x00, ".rdata\0\0", 8);
    write_u32_le(sh_rd + 0x08, static_cast<uint32_t>(spec.rodata_size));
    write_u32_le(sh_rd + 0x0C, rdata_rva);
    write_u32_le(sh_rd + 0x10, static_cast<uint32_t>(rdata_raw_sz));
    write_u32_le(sh_rd + 0x14, static_cast<uint32_t>(headers_raw + text_raw_sz));
    write_u32_le(sh_rd + 0x24, 0x40000040u); // CNT_INITIALIZED_DATA | MEM_READ
  }

  // 5. Copy Section Payloads
  if (spec.text_bytes != nullptr && spec.text_size > 0) {
    std::memcpy(out_buf + headers_raw, spec.text_bytes, spec.text_size);
  } else {
    out_buf[headers_raw] = 0xC3u; // x86 RET safe fallback
  }
  if (has_rdata) {
    std::memcpy(out_buf + headers_raw + text_raw_sz, spec.rodata_bytes, spec.rodata_size);
  }

  return total_file_sz;
}

static size_t serialize_elf64(const MIRC_NativeImageSpec& spec,
                              uint8_t* out_buf,
                              size_t out_capacity) noexcept {
  const bool has_rodata     = (spec.rodata_bytes != nullptr && spec.rodata_size > 0);
  const uint16_t phnum      = has_rodata ? 2u : 1u;
  const uint16_t shnum      = has_rodata ? 4u : 3u; // NULL, .text, [.rodata], .shstrtab
  const size_t text_off     = ELF_PAGE_ALIGN;
  const size_t text_sz      = spec.text_size > 0 ? spec.text_size : 4;
  const size_t rodata_off   = align_up_sz(text_off + text_sz, ELF_PAGE_ALIGN);
  const size_t rodata_sz    = has_rodata ? spec.rodata_size : 0;
  const size_t shstr_off    = has_rodata ? (rodata_off + rodata_sz) : (text_off + text_sz);
  const size_t shdr_off     = align_up_sz(shstr_off + sizeof(ELF_SHSTRTAB), 8);
  const size_t total_sz     = shdr_off + static_cast<size_t>(shnum) * 64;

  if (out_buf == nullptr) return total_sz;
  if (out_capacity < total_sz) return 0;

  std::memset(out_buf, 0, total_sz);

  const uint64_t base_vaddr   = (spec.preferred_image_base != 0) ? spec.preferred_image_base : 0x400000ULL;
  const uint64_t text_vaddr   = base_vaddr + text_off;
  const uint64_t rodata_vaddr = base_vaddr + rodata_off;

  uint16_t e_machine = 62u; // EM_X86_64
  if (spec.arch == MIRC_TargetArch::ARCH_AARCH64)      e_machine = 183u; // EM_AARCH64
  else if (spec.arch == MIRC_TargetArch::ARCH_RISCV64) e_machine = 243u; // EM_RISCV

  // 1. Elf64_Ehdr (64 bytes)
  out_buf[0] = 0x7Fu; out_buf[1] = 'E'; out_buf[2] = 'L'; out_buf[3] = 'F';
  out_buf[4] = 2u;    // ELFCLASS64
  out_buf[5] = 1u;    // ELFDATA2LSB
  out_buf[6] = 1u;    // EV_CURRENT
  write_u16_le(out_buf + 16, 2u); // ET_EXEC
  write_u16_le(out_buf + 18, e_machine);
  write_u32_le(out_buf + 20, 1u);
  write_u64_le(out_buf + 24, text_vaddr + spec.entry_offset_in_text); // e_entry
  write_u64_le(out_buf + 32, 64ULL);                                  // e_phoff
  write_u64_le(out_buf + 40, shdr_off);                               // e_shoff
  write_u16_le(out_buf + 52, 64u);                                    // e_ehsize
  write_u16_le(out_buf + 54, 56u);                                    // e_phentsize
  write_u16_le(out_buf + 56, phnum);
  write_u16_le(out_buf + 58, 64u);                                    // e_shentsize
  write_u16_le(out_buf + 60, shnum);
  write_u16_le(out_buf + 62, shnum - 1u);                             // e_shstrndx

  // 2. Elf64_Phdr[0]: PT_LOAD (.text RX) at offset 64 (56 bytes each)
  uint8_t* ph0 = out_buf + 64;
  write_u32_le(ph0 + 0x00, 1u);             // PT_LOAD
  write_u32_le(ph0 + 0x04, 0x05u);          // PF_R | PF_X
  write_u64_le(ph0 + 0x08, 0ULL);           // p_offset (include ELF headers + .text)
  write_u64_le(ph0 + 0x10, base_vaddr);     // p_vaddr
  write_u64_le(ph0 + 0x18, base_vaddr);     // p_paddr
  write_u64_le(ph0 + 0x20, text_off + text_sz);
  write_u64_le(ph0 + 0x28, text_off + text_sz);
  write_u64_le(ph0 + 0x30, ELF_PAGE_ALIGN);

  if (has_rodata) {
    uint8_t* ph1 = ph0 + 56;
    write_u32_le(ph1 + 0x00, 1u);           // PT_LOAD
    write_u32_le(ph1 + 0x04, 0x04u);        // PF_R
    write_u64_le(ph1 + 0x08, rodata_off);
    write_u64_le(ph1 + 0x10, rodata_vaddr);
    write_u64_le(ph1 + 0x18, rodata_vaddr);
    write_u64_le(ph1 + 0x20, rodata_sz);
    write_u64_le(ph1 + 0x28, rodata_sz);
    write_u64_le(ph1 + 0x30, ELF_PAGE_ALIGN);
  }

  // 3. Payloads (.text, optional .rodata, .shstrtab)
  if (spec.text_bytes != nullptr && spec.text_size > 0) {
    std::memcpy(out_buf + text_off, spec.text_bytes, spec.text_size);
  }
  if (has_rodata) {
    std::memcpy(out_buf + rodata_off, spec.rodata_bytes, spec.rodata_size);
  }
  std::memcpy(out_buf + shstr_off, ELF_SHSTRTAB, sizeof(ELF_SHSTRTAB));

  // 4. Section Headers (64 bytes each): [0]=NULL, [1]=.text, [2]=.rodata/.shstrtab
  uint8_t* sh_text = out_buf + shdr_off + 64;
  write_u32_le(sh_text + 0x00, 1u);         // ".text" offset in ELF_SHSTRTAB
  write_u32_le(sh_text + 0x04, 1u);         // SHT_PROGBITS
  write_u64_le(sh_text + 0x08, 0x06ULL);    // SHF_ALLOC | SHF_EXECINSTR
  write_u64_le(sh_text + 0x10, text_vaddr);
  write_u64_le(sh_text + 0x18, text_off);
  write_u64_le(sh_text + 0x20, text_sz);
  write_u64_le(sh_text + 0x30, 16ULL);

  size_t shstr_idx = 2;
  if (has_rodata) {
    uint8_t* sh_ro = out_buf + shdr_off + 128;
    write_u32_le(sh_ro + 0x00, 7u);         // ".rodata"
    write_u32_le(sh_ro + 0x04, 1u);         // SHT_PROGBITS
    write_u64_le(sh_ro + 0x08, 0x02ULL);    // SHF_ALLOC
    write_u64_le(sh_ro + 0x10, rodata_vaddr);
    write_u64_le(sh_ro + 0x18, rodata_off);
    write_u64_le(sh_ro + 0x20, rodata_sz);
    write_u64_le(sh_ro + 0x30, 16ULL);
    shstr_idx = 3;
  }

  uint8_t* sh_str = out_buf + shdr_off + shstr_idx * 64;
  write_u32_le(sh_str + 0x00, 15u);         // ".shstrtab"
  write_u32_le(sh_str + 0x04, 3u);          // SHT_STRTAB
  write_u64_le(sh_str + 0x18, shstr_off);
  write_u64_le(sh_str + 0x20, sizeof(ELF_SHSTRTAB));
  write_u64_le(sh_str + 0x30, 1ULL);

  return total_sz;
}
} // namespace

size_t MIRC_ExecutableSerializer::compute_serialized_size(const MIRC_NativeImageSpec& spec) noexcept {
  if (spec.format == MIRC_BinaryFormat::ELF64_POSIX) {
    return serialize_elf64(spec, nullptr, 0);
  }
  return serialize_pe32_plus(spec, nullptr, 0);
}

size_t MIRC_ExecutableSerializer::serialize_to_buffer(const MIRC_NativeImageSpec& spec,
                                                      uint8_t* out_buf,
                                                      size_t out_capacity) noexcept {
  if (out_buf == nullptr || out_capacity == 0) return 0;
  if (spec.format == MIRC_BinaryFormat::ELF64_POSIX) {
    return serialize_elf64(spec, out_buf, out_capacity);
  }
  return serialize_pe32_plus(spec, out_buf, out_capacity);
}

bool MIRC_ExecutableSerializer::write_executable_file(const MIRC_NativeImageSpec& spec,
                                                      const char* output_file_path) noexcept {
  if (output_file_path == nullptr || output_file_path[0] == '\0') return false;

  const size_t total_sz = compute_serialized_size(spec);
  if (total_sz == 0) return false;

  uint8_t* buf = NEW_C_HEAP_ARRAY(uint8_t, total_sz, mtCompiler);
  const size_t written = serialize_to_buffer(spec, buf, total_sz);
  if (written != total_sz) {
    FREE_C_HEAP_ARRAY(uint8_t, buf);
    return false;
  }

  FILE* fp = std::fopen(output_file_path, "wb");
  if (fp == nullptr) {
    FREE_C_HEAP_ARRAY(uint8_t, buf);
    return false;
  }

  const size_t io_ok = std::fwrite(buf, 1, total_sz, fp);
  std::fclose(fp);
  FREE_C_HEAP_ARRAY(uint8_t, buf);

#ifndef _WIN32
  if (io_ok == total_sz && spec.format == MIRC_BinaryFormat::ELF64_POSIX) {
    (void)chmod(output_file_path, 0755);
  }
#endif

  return (io_ok == total_sz);
}
