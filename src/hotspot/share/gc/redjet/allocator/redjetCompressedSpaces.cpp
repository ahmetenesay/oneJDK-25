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

#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "gc/redjet/runtime/redjetRingLog.hpp"
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "code/codeCache.hpp"
#include "logging/log.hpp"
#include "oops/compressedKlass.hpp"
#include "oops/compressedOops.hpp"
#include "runtime/os.hpp"
#endif

RedjetCompressedSpaces g_redjet_compressed_spaces;

RedjetCompressedSpaces& RedjetCompressedSpaces::instance() noexcept {
  return g_redjet_compressed_spaces;
}

static void* reserve_isolated_arena(size_t bytes, bool executable_capable) noexcept {
  if (bytes == 0) return nullptr;
#if defined(_WIN32)
  DWORD prot = executable_capable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
  void* mem = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, prot);
  return mem;
#else
  int prot = PROT_READ | PROT_WRITE | (executable_capable ? PROT_EXEC : 0);
  void* mem = mmap(nullptr, bytes, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (mem == MAP_FAILED) ? nullptr : mem;
#endif
}

static void release_isolated_arena(void* base, size_t bytes) noexcept {
  if (base == nullptr || bytes == 0) return;
#if defined(_WIN32)
  (void)bytes;
  VirtualFree(base, 0, MEM_RELEASE);
#else
  munmap(base, bytes);
#endif
}

bool RedjetCompressedSpaces::initialize(uintptr_t on_heap_low,
                                        size_t on_heap_reserved_bytes,
                                        size_t requested_code_boot_bytes,
                                        size_t requested_header_pane_bytes) {
  if (_initialized.exchange(true, std::memory_order_acq_rel)) {
    return true;
  }

  // 1. Snapshot legacy HotSpot CodeCache & CompressedClassSpace bounds so we guarantee zero collision
#ifndef REDJET_STANDALONE_BUILD
  if (CodeCache::low_bound() != nullptr && CodeCache::high_bound() != nullptr) {
    _legacy_codecache_low  = reinterpret_cast<uintptr_t>(CodeCache::low_bound());
    _legacy_codecache_high = reinterpret_cast<uintptr_t>(CodeCache::high_bound());
  }
  if (CompressedKlassPointers::base() != nullptr && CompressedKlassPointers::range() > 0) {
    _legacy_ccs_low  = reinterpret_cast<uintptr_t>(CompressedKlassPointers::base());
    _legacy_ccs_high = _legacy_ccs_low + CompressedKlassPointers::range();
  }
#endif

  // 2. Configure Unified On-Heap NarrowOop Encoding
  _oop_span_bytes = on_heap_reserved_bytes;
  const uintptr_t on_heap_high = on_heap_low + on_heap_reserved_bytes;
  constexpr size_t FOUR_GB      = 4ULL * 1024 * 1024 * 1024;
  constexpr size_t THIRTYTWO_GB = 32ULL * 1024 * 1024 * 1024;
  constexpr size_t TWOFIFTYSIX_GB = 256ULL * 1024 * 1024 * 1024;

  if (on_heap_high <= FOUR_GB) {
    _oop_base  = 0;
    _oop_shift = 0;
    _oop_mode  = RedjetCompressedMode::ZERO_BASE_ZERO_SHIFT;
  } else if (on_heap_high <= THIRTYTWO_GB) {
    _oop_base  = 0;
    _oop_shift = 3;
    _oop_mode  = RedjetCompressedMode::ZERO_BASE_SHIFT_3;
  } else if (on_heap_reserved_bytes <= FOUR_GB) {
    _oop_base  = on_heap_low;
    _oop_shift = 0;
    _oop_mode  = RedjetCompressedMode::BASE_ZERO_SHIFT;
  } else if (on_heap_reserved_bytes <= THIRTYTWO_GB) {
    _oop_base  = on_heap_low;
    _oop_shift = 3;
    _oop_mode  = RedjetCompressedMode::BASE_SHIFT_3;
  } else if (on_heap_reserved_bytes <= TWOFIFTYSIX_GB) {
    // Redjet-exclusive 64-byte cacheline-aligned compressed oops up to 256 GB!
    _oop_base  = on_heap_low;
    _oop_shift = 6;
    _oop_mode  = RedjetCompressedMode::BASE_SHIFT_6_CL;
  } else {
    _oop_base  = 0;
    _oop_shift = 0;
    _oop_mode  = RedjetCompressedMode::UNCOMPRESSED_64BIT;
  }

  // 3. Provision isolated Redjet HeaderPane (/heap/on_heap/landfill/header_pane)
  size_t hp_bytes = (requested_header_pane_bytes >= 16ULL * 1024 * 1024)
                        ? requested_header_pane_bytes
                        : (64ULL * 1024 * 1024);
  // Clamp initial commit to a safe, fast footprint (64 MB default commit if large)
  size_t hp_alloc_bytes = (hp_bytes > 64ULL * 1024 * 1024) ? (64ULL * 1024 * 1024) : hp_bytes;
  void* hp_mem = reserve_isolated_arena(hp_alloc_bytes, false);
  if (hp_mem != nullptr) {
    _klass_base           = reinterpret_cast<uintptr_t>(hp_mem);
    _klass_reserved_bytes = hp_alloc_bytes;
    _owns_header_pane_arena = true;
    // Reserve offset 0 (64B) as null sentinel so narrowKlass == 0 always decodes to nullptr!
    _klass_used_bytes.store(64, std::memory_order_relaxed);
    if ((_klass_base + _klass_reserved_bytes) <= FOUR_GB) {
      _klass_base  = 0;
      _klass_shift = 0;
      _klass_mode  = RedjetCompressedMode::ZERO_BASE_ZERO_SHIFT;
    } else if (_klass_reserved_bytes <= FOUR_GB) {
      _klass_shift = 0;
      _klass_mode  = RedjetCompressedMode::BASE_ZERO_SHIFT;
    } else {
      _klass_shift = 3;
      _klass_mode  = RedjetCompressedMode::BASE_SHIFT_3;
    }
  }

  // 4. Provision isolated VFS /heap/off_heap/code_boot arena (strictly disjoint from HotSpot CodeCache & CCS)
  size_t cb_bytes = (requested_code_boot_bytes >= 16ULL * 1024 * 1024)
                        ? requested_code_boot_bytes
                        : (64ULL * 1024 * 1024);
  size_t cb_alloc_bytes = (cb_bytes > 64ULL * 1024 * 1024) ? (64ULL * 1024 * 1024) : cb_bytes;
  void* cb_mem = reserve_isolated_arena(cb_alloc_bytes, true);
  if (cb_mem != nullptr) {
    _owns_code_boot_arena = true;
    uintptr_t base = reinterpret_cast<uintptr_t>(cb_mem);
    size_t quarter = (cb_alloc_bytes / 4) & ~static_cast<size_t>(4095ULL);

    _code_boot.arena_base           = base;
    _code_boot.arena_reserved_bytes = cb_alloc_bytes;
    _code_boot.rawcode_cell_base    = base;
    _code_boot.rawcode_cell_bytes   = quarter;
    _code_boot.vac_chunk_base       = base + quarter;
    _code_boot.vac_chunk_bytes      = quarter;
    _code_boot.mirc_lvl1_base       = base + (quarter * 2);
    _code_boot.mirc_lvl1_bytes      = quarter;
    _code_boot.mirc_lvl2_base       = base + (quarter * 3);
    _code_boot.mirc_lvl2_bytes      = cb_alloc_bytes - (quarter * 3);
  }

  bool isolated = verify_isolation_invariants();
  _code_boot.isolated_from_legacy_codecache = ranges_disjoint(
      _code_boot.arena_base, _code_boot.arena_base + _code_boot.arena_reserved_bytes,
      _legacy_codecache_low, _legacy_codecache_high);
  _code_boot.isolated_from_legacy_ccs = ranges_disjoint(
      _code_boot.arena_base, _code_boot.arena_base + _code_boot.arena_reserved_bytes,
      _legacy_ccs_low, _legacy_ccs_high);

#ifndef REDJET_STANDALONE_BUILD
  log_info(gc, init)("RedjetGC CompressedSpaces & CodeBoot Isolation: oop_mode=%s (shift=%d), "
                     "header_pane=%s (base=" PTR_FORMAT ", %zu MB), code_boot=" PTR_FORMAT " (%zu MB, isolated=%s)",
                     mode_name(_oop_mode), _oop_shift,
                     mode_name(_klass_mode), _klass_base, _klass_reserved_bytes / (1024 * 1024),
                     _code_boot.arena_base, _code_boot.arena_reserved_bytes / (1024 * 1024),
                     isolated ? "VERIFIED" : "COLLISION_DETECTED");
#endif
  return isolated;
}

void RedjetCompressedSpaces::shutdown() noexcept {
  if (!_initialized.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (_owns_code_boot_arena && _code_boot.arena_base != 0) {
    release_isolated_arena(reinterpret_cast<void*>(_code_boot.arena_base), _code_boot.arena_reserved_bytes);
    std::memset(&_code_boot, 0, sizeof(_code_boot));
    _owns_code_boot_arena = false;
  }
  if (_owns_header_pane_arena && _klass_base != 0) {
    release_isolated_arena(reinterpret_cast<void*>(_klass_base), _klass_reserved_bytes);
    _klass_base = 0;
    _klass_reserved_bytes = 0;
    _owns_header_pane_arena = false;
  }
}

void* RedjetCompressedSpaces::allocate_in_header_pane(size_t bytes, size_t alignment) noexcept {
  if (bytes == 0 || _klass_reserved_bytes == 0 || _klass_base == 0) return nullptr;
  if (alignment < 8) alignment = 8;
  size_t mask = alignment - 1;
  size_t cur = _klass_used_bytes.load(std::memory_order_relaxed);
  while (true) {
    size_t aligned = (cur + mask) & ~mask;
    size_t next = aligned + bytes;
    if (next > _klass_reserved_bytes) {
      return nullptr;
    }
    if (_klass_used_bytes.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      void* ptr = reinterpret_cast<void*>(_klass_base + aligned);
      std::memset(ptr, 0, bytes);
      return ptr;
    }
  }
}

void* RedjetCompressedSpaces::allocate_rawcode_cell(size_t bytes, uint64_t vac_key) noexcept {
  if (vac_key != RedjetVFSTree::KEY_VAC_READ || bytes == 0 || _code_boot.rawcode_cell_bytes == 0) {
    return nullptr;
  }
  size_t aligned_sz = (bytes + 63ULL) & ~63ULL;
  size_t off = _rawcode_used.fetch_add(aligned_sz, std::memory_order_acq_rel);
  if (off + aligned_sz > _code_boot.rawcode_cell_bytes) {
    _rawcode_used.fetch_sub(aligned_sz, std::memory_order_relaxed);
    return nullptr;
  }
  return reinterpret_cast<void*>(_code_boot.rawcode_cell_base + off);
}

void* RedjetCompressedSpaces::allocate_vac_chunk(size_t bytes, uint64_t mirc_key) noexcept {
  if (mirc_key != RedjetVFSTree::KEY_MIRC_READ || bytes == 0 || _code_boot.vac_chunk_bytes == 0) {
    return nullptr;
  }
  size_t aligned_sz = (bytes + 63ULL) & ~63ULL;
  size_t off = _vac_chunk_used.fetch_add(aligned_sz, std::memory_order_acq_rel);
  if (off + aligned_sz > _code_boot.vac_chunk_bytes) {
    _vac_chunk_used.fetch_sub(aligned_sz, std::memory_order_relaxed);
    return nullptr;
  }
  return reinterpret_cast<void*>(_code_boot.vac_chunk_base + off);
}

void* RedjetCompressedSpaces::allocate_mirc_ready_page(size_t bytes, bool is_lvl2) noexcept {
  if (bytes == 0) return nullptr;
  size_t aligned_sz = (bytes + 63ULL) & ~63ULL;
  if (is_lvl2) {
    if (_code_boot.mirc_lvl2_bytes == 0) return nullptr;
    size_t off = _mirc_lvl2_used.fetch_add(aligned_sz, std::memory_order_acq_rel);
    if (off + aligned_sz > _code_boot.mirc_lvl2_bytes) {
      _mirc_lvl2_used.fetch_sub(aligned_sz, std::memory_order_relaxed);
      return nullptr;
    }
    return reinterpret_cast<void*>(_code_boot.mirc_lvl2_base + off);
  } else {
    if (_code_boot.mirc_lvl1_bytes == 0) return nullptr;
    size_t off = _mirc_lvl1_used.fetch_add(aligned_sz, std::memory_order_acq_rel);
    if (off + aligned_sz > _code_boot.mirc_lvl1_bytes) {
      _mirc_lvl1_used.fetch_sub(aligned_sz, std::memory_order_relaxed);
      return nullptr;
    }
    return reinterpret_cast<void*>(_code_boot.mirc_lvl1_base + off);
  }
}

bool RedjetCompressedSpaces::verify_isolation_invariants() const noexcept {
  uintptr_t cb_lo = _code_boot.arena_base;
  uintptr_t cb_hi = cb_lo + _code_boot.arena_reserved_bytes;
  uintptr_t hp_lo = _klass_base;
  uintptr_t hp_hi = hp_lo + _klass_reserved_bytes;

  bool ok_cc_cb = ranges_disjoint(cb_lo, cb_hi, _legacy_codecache_low, _legacy_codecache_high);
  bool ok_ccs_cb = ranges_disjoint(cb_lo, cb_hi, _legacy_ccs_low, _legacy_ccs_high);
  bool ok_ccs_hp = ranges_disjoint(hp_lo, hp_hi, _legacy_ccs_low, _legacy_ccs_high);
  bool ok_hp_cb  = ranges_disjoint(hp_lo, hp_hi, cb_lo, cb_hi);

  return ok_cc_cb && ok_ccs_cb && ok_ccs_hp && ok_hp_cb;
}

const char* RedjetCompressedSpaces::mode_name(RedjetCompressedMode m) noexcept {
  switch (m) {
    case RedjetCompressedMode::UNCOMPRESSED_64BIT:   return "UNCOMPRESSED_64BIT";
    case RedjetCompressedMode::ZERO_BASE_ZERO_SHIFT: return "ZERO_BASE_ZERO_SHIFT";
    case RedjetCompressedMode::ZERO_BASE_SHIFT_3:    return "ZERO_BASE_SHIFT_3";
    case RedjetCompressedMode::BASE_ZERO_SHIFT:      return "BASE_ZERO_SHIFT";
    case RedjetCompressedMode::BASE_SHIFT_3:         return "BASE_SHIFT_3";
    case RedjetCompressedMode::BASE_SHIFT_6_CL:      return "BASE_SHIFT_6_CACHELINE_64B";
    default:                                         return "UNKNOWN";
  }
}

#ifndef REDJET_STANDALONE_BUILD
void RedjetCompressedSpaces::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet CompressedSpaces & VFS CodeBoot Isolation:");
  st->print_cr("  NarrowOop Mode:          %s (base=" PTR_FORMAT ", shift=%d)",
               mode_name(_oop_mode), _oop_base, _oop_shift);
  st->print_cr("  HeaderPane NarrowKlass:  %s (base=" PTR_FORMAT ", shift=%d, used=%zu/%zu KB)",
               mode_name(_klass_mode), _klass_base, _klass_shift,
               klass_used_bytes() / 1024, _klass_reserved_bytes / 1024);
  st->print_cr("  VFS CodeBoot Arena:      base=" PTR_FORMAT ", reserved=%zu KB, used=%zu KB, isolated=%s",
               _code_boot.arena_base,
               _code_boot.arena_reserved_bytes / 1024,
               code_boot_used_bytes() / 1024,
               verify_isolation_invariants() ? "YES" : "NO");
}
#endif

extern "C" {

RJ_API int HS_hotspot_code_heap_query(size_t* out_reserved_bytes,
                                      size_t* out_used_bytes,
                                      uintptr_t* out_base_addr) {
  const RedjetCompressedSpaces& cs = RedjetCompressedSpaces::instance();
  if (!cs.is_initialized()) {
    if (out_reserved_bytes) *out_reserved_bytes = 0;
    if (out_used_bytes)     *out_used_bytes = 0;
    if (out_base_addr)      *out_base_addr = 0;
    return -1;
  }
  if (out_reserved_bytes) *out_reserved_bytes = cs.code_boot().arena_reserved_bytes;
  if (out_used_bytes)     *out_used_bytes     = cs.code_boot_used_bytes();
  if (out_base_addr)      *out_base_addr      = cs.code_boot().arena_base;
  return 0;
}

} // extern "C"
