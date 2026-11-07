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

#include "gc/redjet/vfs/redjetVFSNode_VacCodeChunk.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_error(...) if (false) printf
#endif

namespace {

static uint64_t compute_vac_chunk_mac(const void* data, size_t length, uint64_t secret_key) noexcept {
  if (data == nullptr || length == 0) return 0;
  const uint8_t* p = static_cast<const uint8_t*>(data);

  uint64_t h = secret_key ^ 0x5641435F43484E4BULL; // "VAC_CHNK"
  size_t chunks = length / 8;
  for (size_t i = 0; i < chunks; ++i) {
    uint64_t w;
    std::memcpy(&w, p + (i * 8), sizeof(w));
    h ^= w;
    h = (h << 31) | (h >> 33);
    h *= 0xFF51AFD7ED558CCDULL;
  }

  size_t rem = length % 8;
  if (rem > 0) {
    uint64_t last = 0;
    std::memcpy(&last, p + (chunks * 8), rem);
    h ^= last;
    h *= 0xC4CEB9FE1A85EC53ULL;
  }
  return h;
}

} // namespace

RedjetVFSNode_VacCodeChunk::RedjetVFSNode_VacCodeChunk(
    const char* name, const char* full_path, RedjetVFSNode* parent, size_t quota_bytes)
  : RedjetVFSNode(name, full_path, parent, quota_bytes),
    _chunk_count(0),
    _total_chunks_created(0),
    _total_chunks_sealed(0),
    _total_mirc_reads(0),
    _total_tamper_traps(0) {
  hs_ticket_lock_init(&_chunk_lock);
  std::memset(_chunks, 0, sizeof(_chunks));
}

RedjetVFSNode_VacCodeChunk::~RedjetVFSNode_VacCodeChunk() {
  hs_ticket_guard_t guard(_chunk_lock);
  for (size_t i = 0; i < _chunk_count; ++i) {
    if (_chunks[i] != nullptr) {
      size_t total_alloc = _chunks[i]->allocated_bytes;
#if defined(_WIN32)
      VirtualFree(_chunks[i], 0, MEM_RELEASE);
#else
      munmap(_chunks[i], total_alloc);
#endif
      _chunks[i] = nullptr;
    }
  }
  _chunk_count = 0;
}

RedjetVacCodeChunkHeader* RedjetVFSNode_VacCodeChunk::allocate_chunk(
    uint32_t method_id,
    size_t payload_bytes,
    uint16_t gpr_pressure,
    uint16_t vec_pressure,
    uint8_t branch_entropy) {

  if (payload_bytes == 0) return nullptr;

  size_t total_needed = sizeof(RedjetVacCodeChunkHeader) + payload_bytes;
  // Align up to 64-byte cache line
  total_needed = (total_needed + 63) & ~size_t(63);

  hs_ticket_guard_t guard(_chunk_lock);
  if (_chunk_count >= MAX_CHUNKS_TRACKED) return nullptr;

  if (_quota_bytes > 0 && (_used_bytes + total_needed) > _quota_bytes) {
    return nullptr;
  }

  // Check if method chunk already exists
  for (size_t i = 0; i < _chunk_count; ++i) {
    if (_chunks[i] != nullptr && _chunks[i]->method_id == method_id) {
      // Reallocate if existing payload too small
      if (_chunks[i]->allocated_bytes >= total_needed) {
        _chunks[i]->payload_bytes = payload_bytes;
        _chunks[i]->virt_gpr_pressure = gpr_pressure;
        _chunks[i]->virt_vec_pressure = vec_pressure;
        _chunks[i]->branch_entropy = branch_entropy;
        _chunks[i]->is_sealed = 0;
        _chunks[i]->is_mirc_acquired = 0;
        _chunks[i]->timestamp_tsc = sacculus_rdtsc();
        return _chunks[i];
      }
    }
  }

  uint8_t* raw_mem = nullptr;
#if defined(_WIN32)
  raw_mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, total_needed, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
  raw_mem = static_cast<uint8_t*>(mmap(nullptr, total_needed, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (raw_mem == MAP_FAILED) raw_mem = nullptr;
#endif

  if (raw_mem == nullptr) return nullptr;

  RedjetVacCodeChunkHeader* hdr = reinterpret_cast<RedjetVacCodeChunkHeader*>(raw_mem);
  std::memset(hdr, 0, sizeof(RedjetVacCodeChunkHeader));

  hdr->method_id = method_id;
  hdr->payload_bytes = payload_bytes;
  hdr->allocated_bytes = total_needed;
  hdr->timestamp_tsc = sacculus_rdtsc();
  hdr->virt_gpr_pressure = gpr_pressure;
  hdr->virt_vec_pressure = vec_pressure;
  hdr->branch_entropy = branch_entropy;
  hdr->has_no_alias = 1;
  hdr->is_sealed = 0;
  hdr->is_mirc_acquired = 0;

  _chunks[_chunk_count++] = hdr;
  _used_bytes += total_needed;
  _total_chunks_created.fetch_add(1, std::memory_order_relaxed);

  return hdr;
}

bool RedjetVFSNode_VacCodeChunk::seal_chunk(RedjetVacCodeChunkHeader* chunk, uint64_t access_key) {
  if (chunk == nullptr) return false;
  hs_ticket_guard_t guard(_chunk_lock);

  uint64_t mac = compute_vac_chunk_mac(chunk->payload_data(), chunk->payload_bytes, access_key);
  chunk->crypto_seal = mac;
  chunk->is_sealed = 1;
  chunk->is_mirc_acquired = 0;

  _total_chunks_sealed.fetch_add(1, std::memory_order_relaxed);
  return true;
}

const void* RedjetVFSNode_VacCodeChunk::acquire_for_mirc(
    uint32_t method_id,
    uint64_t access_key,
    size_t* out_payload_bytes,
    RedjetVacCodeChunkHeader** out_hdr) {

  if (out_payload_bytes != nullptr) *out_payload_bytes = 0;
  if (out_hdr != nullptr) *out_hdr = nullptr;

  hs_ticket_guard_t guard(_chunk_lock);

  RedjetVacCodeChunkHeader* target = nullptr;
  for (size_t i = 0; i < _chunk_count; ++i) {
    if (_chunks[i] != nullptr && _chunks[i]->method_id == method_id) {
      target = _chunks[i];
      break;
    }
  }

  if (target == nullptr) return nullptr;

  // Validate access key against KEY_MIRC_READ
  if (access_key != RedjetVFSTree::KEY_MIRC_READ) {
    _total_tamper_traps.fetch_add(1, std::memory_order_relaxed);
    log_error(gc)("Redjet VFS: Unauthorized attempt to read VAC chunk %u without KEY_MIRC_READ", method_id);
    return nullptr;
  }

  // Validate integrity seal if chunk is sealed
  if (target->is_sealed != 0) {
    uint64_t expected = compute_vac_chunk_mac(target->payload_data(), target->payload_bytes, access_key);
    if (expected != target->crypto_seal) {
      _total_tamper_traps.fetch_add(1, std::memory_order_relaxed);
      log_error(gc)("Redjet VFS: Crypto seal mismatch in VAC chunk %u! Tampering detected!", method_id);
      return nullptr;
    }
  }

  target->is_mirc_acquired = 1;
  _total_mirc_reads.fetch_add(1, std::memory_order_relaxed);

  if (out_payload_bytes != nullptr) *out_payload_bytes = target->payload_bytes;
  if (out_hdr != nullptr) *out_hdr = target;

  return target->payload_data();
}

bool RedjetVFSNode_VacCodeChunk::release_from_mirc(uint32_t method_id, uint64_t access_key) {
  if (access_key != RedjetVFSTree::KEY_MIRC_READ) return false;

  hs_ticket_guard_t guard(_chunk_lock);
  for (size_t i = 0; i < _chunk_count; ++i) {
    if (_chunks[i] != nullptr && _chunks[i]->method_id == method_id) {
      _chunks[i]->is_mirc_acquired = 0;
      return true;
    }
  }
  return false;
}

RedjetVacCodeChunkHeader* RedjetVFSNode_VacCodeChunk::find_chunk(uint32_t method_id) {
  hs_ticket_guard_t guard(_chunk_lock);
  for (size_t i = 0; i < _chunk_count; ++i) {
    if (_chunks[i] != nullptr && _chunks[i]->method_id == method_id) {
      return _chunks[i];
    }
  }
  return nullptr;
}

bool RedjetVFSNode_VacCodeChunk::remove_chunk(uint32_t method_id) {
  hs_ticket_guard_t guard(_chunk_lock);
  for (size_t i = 0; i < _chunk_count; ++i) {
    if (_chunks[i] != nullptr && _chunks[i]->method_id == method_id) {
      size_t alloc_sz = _chunks[i]->allocated_bytes;
#if defined(_WIN32)
      VirtualFree(_chunks[i], 0, MEM_RELEASE);
#else
      munmap(_chunks[i], alloc_sz);
#endif
      if (_used_bytes >= alloc_sz) _used_bytes -= alloc_sz;
      _chunks[i] = _chunks[_chunk_count - 1];
      _chunks[_chunk_count - 1] = nullptr;
      _chunk_count--;
      return true;
    }
  }
  return false;
}

bool RedjetVFSNode_VacCodeChunk::verify_chunk_integrity(RedjetVacCodeChunkHeader* chunk, uint64_t access_key) {
  if (chunk == nullptr || chunk->is_sealed == 0) return false;
  uint64_t expected = compute_vac_chunk_mac(chunk->payload_data(), chunk->payload_bytes, access_key);
  return expected == chunk->crypto_seal;
}

void RedjetVFSNode_VacCodeChunk::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("RedjetVFSNode_VacCodeChunk at %s:", _path);
  st->print_cr("  - Chunks Tracked:      %zu / %zu", _chunk_count, MAX_CHUNKS_TRACKED);
  st->print_cr("  - Memory Used:         %zu bytes (Quota: %zu bytes)", _used_bytes, _quota_bytes);
  st->print_cr("  - Chunks Created:      %llu", static_cast<unsigned long long>(_total_chunks_created.load(std::memory_order_relaxed)));
  st->print_cr("  - Chunks Sealed:       %llu", static_cast<unsigned long long>(_total_chunks_sealed.load(std::memory_order_relaxed)));
  st->print_cr("  - MIRC Reads:          %llu", static_cast<unsigned long long>(_total_mirc_reads.load(std::memory_order_relaxed)));
  st->print_cr("  - Tamper Traps:        %llu", static_cast<unsigned long long>(_total_tamper_traps.load(std::memory_order_relaxed)));
}
