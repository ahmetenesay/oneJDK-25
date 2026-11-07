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

#include "mirc/fcs_Archive.hpp"
#include "runtime/os.hpp"
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

uint64_t FCSArchive::compute_jar_hash(const char* jar_path) {
  if (jar_path == nullptr) return 0;
  uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
  size_t len = strlen(jar_path);
  for (size_t i = 0; i < len; ++i) {
    hash ^= (uint64_t)(unsigned char)jar_path[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t FCSArchive::compute_cpu_fingerprint() {
  uint64_t fp = 0;
#if defined(_M_X64) || defined(__x86_64__)
  fp |= (1ULL << 0); // x86_64 base
#elif defined(__aarch64__) || defined(_M_ARM64)
  fp |= (1ULL << 1); // AArch64 base
#elif defined(__riscv)
  fp |= (1ULL << 2); // RISC-V base
#endif
  fp ^= (uint64_t)FCSA_MAGIC;
  return fp;
}

bool FCSArchive::save_archive(const char* filepath,
                              const FCSA_Header& header,
                              const FCSA_Method_Entry* methods,
                              const void* vac_data, size_t vac_size,
                              const void* code_data, size_t code_size) {
  if (filepath == nullptr) return false;

  FILE* f = fopen(filepath, "wb");
  if (f == nullptr) return false;

  // 1. Write Header
  if (fwrite(&header, sizeof(FCSA_Header), 1, f) != 1) {
    fclose(f);
    return false;
  }

  // 2. Write Method Directory
  if (header.total_methods_count > 0 && methods != nullptr) {
    if (fwrite(methods, sizeof(FCSA_Method_Entry), header.total_methods_count, f) != header.total_methods_count) {
      fclose(f);
      return false;
    }
  }

  // 3. Write VAC Data (Layers 1-2)
  if (vac_size > 0 && vac_data != nullptr) {
    if (fwrite(vac_data, 1, vac_size, f) != vac_size) {
      fclose(f);
      return false;
    }
  }

  // 4. Write CodeBuffer Data (Layer 4)
  if (code_size > 0 && code_data != nullptr) {
    if (fwrite(code_data, 1, code_size, f) != code_size) {
      fclose(f);
      return false;
    }
  }

  fclose(f);
  return true;
}

bool FCSArchive::load_archive_mmap(const char* filepath,
                                   uint64_t expected_jar_hash,
                                   void** out_mapped_base,
                                   size_t* out_mapped_size,
                                   const FCSA_Header** out_header) {
  if (filepath == nullptr || out_mapped_base == nullptr || out_header == nullptr) {
    return false;
  }

#if defined(_WIN32)
  HANDLE hFile = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER fsize;
  if (!GetFileSizeEx(hFile, &fsize) || fsize.QuadPart < (LONGLONG)sizeof(FCSA_Header)) {
    CloseHandle(hFile);
    return false;
  }

  HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (hMapping == nullptr) {
    CloseHandle(hFile);
    return false;
  }

  void* mapped = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(hMapping);
  CloseHandle(hFile);

  if (mapped == nullptr) return false;

  const FCSA_Header* hdr = (const FCSA_Header*)mapped;
  if (hdr->magic != FCSA_MAGIC || hdr->version != FCSA_VERSION) {
    UnmapViewOfFile(mapped);
    return false;
  }

  if (expected_jar_hash != 0 && hdr->jar_fingerprint_hash != expected_jar_hash) {
    UnmapViewOfFile(mapped);
    return false;
  }

  *out_mapped_base = mapped;
  if (out_mapped_size != nullptr) {
    *out_mapped_size = (size_t)fsize.QuadPart;
  }
  *out_header = hdr;
  return true;

#else
  int fd = open(filepath, O_RDONLY);
  if (fd < 0) return false;

  struct stat st;
  if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(FCSA_Header)) {
    close(fd);
    return false;
  }

  void* mapped = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);

  if (mapped == MAP_FAILED) return false;

  const FCSA_Header* hdr = (const FCSA_Header*)mapped;
  if (hdr->magic != FCSA_MAGIC || hdr->version != FCSA_VERSION) {
    munmap(mapped, (size_t)st.st_size);
    return false;
  }

  if (expected_jar_hash != 0 && hdr->jar_fingerprint_hash != expected_jar_hash) {
    munmap(mapped, (size_t)st.st_size);
    return false;
  }

  *out_mapped_base = mapped;
  if (out_mapped_size != nullptr) {
    *out_mapped_size = (size_t)st.st_size;
  }
  *out_header = hdr;
  return true;
#endif
}
