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

#include "gc/redlet/redletFalline.hpp"

#ifdef REDLET_STANDALONE_BUILD
#include <cstdio>
#define log_trace(...) if (false) printf
#define log_info(...) if (false) printf
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
namespace os {
  static inline bool commit_memory(char* addr, size_t bytes, bool executable) {
    (void)executable;
#if defined(_WIN32)
    return VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return true;
#endif
  }
}
struct outputStream {
  void print_cr(...) {}
};
#define UINT64_FORMAT "%llu"
#else
#include "logging/log.hpp"
#include "utilities/ostream.hpp"
#endif

RedletFalline::RedletFalline() :
  _emptybox_count(0),
  _is_compacting(false),
  _snatch_hit_count(0),
  _snatch_miss_count(0),
  _split_count(0),
  _coalesced_merge_count(0) {

  for (size_t i = 0; i < NUM_EMPTYBOX_BINS; ++i) {
    _bin_heads[i] = nullptr;
    _bin_counts[i] = 0;
    _bin_locks[i].store(false, std::memory_order_relaxed);
  }
}

RedletFalline::~RedletFalline() {
  for (size_t i = 0; i < NUM_EMPTYBOX_BINS; ++i) {
    _bin_heads[i] = nullptr;
    _bin_counts[i] = 0;
  }
}

void RedletFalline::register_emptybox(size_t aligned_size, RedletEmptyBox* eb, uint64_t sweep_no) {
  if (eb == nullptr || aligned_size < sizeof(RedletEmptyBox)) {
    return;
  }

  int bin = find_emptybox_bin(aligned_size);
  eb->init(0, aligned_size, aligned_size, nullptr, sweep_no);

  lock_bin(bin);
  eb->next = _bin_heads[bin];
  eb->prev = nullptr;
  if (_bin_heads[bin] != nullptr) {
    _bin_heads[bin]->prev = eb;
  }
  _bin_heads[bin] = eb;
  _bin_counts[bin]++;
  _emptybox_count.fetch_add(1, std::memory_order_relaxed);
  unlock_bin(bin);

  log_trace(gc, alloc)("Registered EmptyBox in bin %d: size=%zu", bin, aligned_size);
}

RedletEmptyBox* RedletFalline::snatch_emptybox(size_t needed_bytes, uint64_t current_sweep) {
  if (_is_compacting.load(std::memory_order_acquire)) {
    return nullptr;
  }

  // Ensure minimum alignment
  size_t needed_aligned = (needed_bytes + 63ULL) & ~63ULL;
  int start_bin = find_emptybox_bin(needed_aligned);

  for (int bin = start_bin; bin < (int)NUM_EMPTYBOX_BINS; ++bin) {
    RedletEmptyBox* candidate = nullptr;
    size_t rem = 0;

    lock_bin(bin);
    RedletEmptyBox* cur = _bin_heads[bin];
    while (cur != nullptr) {
      if (cur->aligned_hole_size >= needed_aligned) {
        // Unlink from bin
        if (cur->prev != nullptr) {
          cur->prev->next = cur->next;
        } else {
          _bin_heads[bin] = cur->next;
        }
        if (cur->next != nullptr) {
          cur->next->prev = cur->prev;
        }
        cur->next = nullptr;
        cur->prev = nullptr;
        _bin_counts[bin]--;
        _emptybox_count.fetch_sub(1, std::memory_order_relaxed);

        candidate = cur;
        rem = candidate->aligned_hole_size - needed_aligned;
        break;
      }
      cur = cur->next;
    }
    unlock_bin(bin);

    if (candidate != nullptr) {
      if (candidate->is_decommitted) {
        os::commit_memory((char*)candidate, candidate->aligned_hole_size, false);
        candidate->is_decommitted = 0;
        candidate->decommitted_kb = 0;
      }

      // Split remainder if large enough (>= 128 bytes)
      if (rem >= 128) {
        candidate->aligned_hole_size = needed_aligned;
        uint8_t* tail_ptr = reinterpret_cast<uint8_t*>(candidate) + needed_aligned;
        RedletEmptyBox* tail = reinterpret_cast<RedletEmptyBox*>(tail_ptr);
        tail->init(0, rem, rem, nullptr, current_sweep);

        _split_count.fetch_add(1, std::memory_order_relaxed);

        int tail_bin = find_emptybox_bin(rem);
        lock_bin(tail_bin);
        tail->next = _bin_heads[tail_bin];
        tail->prev = nullptr;
        if (_bin_heads[tail_bin] != nullptr) {
          _bin_heads[tail_bin]->prev = tail;
        }
        _bin_heads[tail_bin] = tail;
        _bin_counts[tail_bin]++;
        _emptybox_count.fetch_add(1, std::memory_order_relaxed);
        unlock_bin(tail_bin);
      }

      _snatch_hit_count.fetch_add(1, std::memory_order_relaxed);
      return candidate;
    }
  }

  _snatch_miss_count.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void RedletFalline::print_on(outputStream* st) const {
  st->print_cr("RedletFalline: %zu emptyboxes, hits: " UINT64_FORMAT ", misses: " UINT64_FORMAT ", splits: " UINT64_FORMAT,
               _emptybox_count.load(), _snatch_hit_count.load(), _snatch_miss_count.load(), _split_count.load());
}
