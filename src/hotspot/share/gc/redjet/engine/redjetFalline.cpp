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

#include "gc/redjet/engine/redjetFalline.hpp"
#include "gc/redjet/puzzler/redjetSelfAdvanceHeuristics.hpp"
#include <new>

#ifdef REDJET_STANDALONE_BUILD
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

RedjetFalline::RedjetFalline() :
  _emptybox_count(0),
  _is_compacting(false),
  _snatch_hit_count(0),
  _snatch_miss_count(0),
  _split_count(0),
  _coalesced_merge_count(0),
  _morgue_head(nullptr),
  _morgue_tail(nullptr),
  _morgue_count(0),
  _morgue_bytes(0),
  _resurrection_requests(0),
  _resurrections_succeeded(0),
  _resurrections_rejected(0),
  _morgue_expired_swept(0) {

  for (size_t i = 0; i < NUM_EMPTYBOX_BINS; ++i) {
    _bin_heads[i] = nullptr;
    _bin_counts[i] = 0;
    _bin_locks[i].store(false, std::memory_order_relaxed);
  }
}

RedjetFalline::~RedjetFalline() {
  for (size_t i = 0; i < NUM_EMPTYBOX_BINS; ++i) {
    _bin_heads[i] = nullptr;
    _bin_counts[i] = 0;
  }

  hs_ticket_guard_t guard(_morgue_lock);
  RedjetMorgueNode* cur = _morgue_head;
  while (cur != nullptr) {
    RedjetMorgueNode* next = cur->next;
    delete cur;
    cur = next;
  }
  _morgue_head = nullptr;
  _morgue_tail = nullptr;
}

void RedjetFalline::register_emptybox(size_t aligned_size, RedjetEmptyBox* eb, uint64_t sweep_no) {
  if (eb == nullptr || aligned_size < sizeof(RedjetEmptyBox)) {
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

RedjetEmptyBox* RedjetFalline::snatch_emptybox(size_t needed_bytes, uint64_t current_sweep) {
  if (_is_compacting.load(std::memory_order_acquire)) {
    return nullptr;
  }

  // Ensure minimum alignment
  size_t needed_aligned = (needed_bytes + 63ULL) & ~63ULL;
  int start_bin = find_emptybox_bin(needed_aligned);

  for (int bin = start_bin; bin < (int)NUM_EMPTYBOX_BINS; ++bin) {
    RedjetEmptyBox* candidate = nullptr;
    size_t rem = 0;

    lock_bin(bin);
    RedjetEmptyBox* cur = _bin_heads[bin];
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
        RedjetEmptyBox* tail = reinterpret_cast<RedjetEmptyBox*>(tail_ptr);
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

/* ========================================================================= */
/* MORGUE LIFECYCLE & RESURRECTION IMPLEMENTATION                            */
/* ========================================================================= */

bool RedjetFalline::stage_in_morgue(void* obj_ptr, size_t footprint, uint32_t function_id, uint64_t current_epoch) {
  if (!obj_ptr || footprint == 0) return false;

  auto* node = new (std::nothrow) RedjetMorgueNode();
  if (!node) return false;

  node->obj_ptr = obj_ptr;
  node->footprint_bytes = footprint;
  node->function_id = function_id;
  node->condemned_epoch = current_epoch;
  node->condemned_timestamp_ns = sacculus_now_ns();
  node->is_resurrected.store(false, std::memory_order_relaxed);

  hs_ticket_guard_t guard(_morgue_lock);
  node->next = nullptr;
  node->prev = _morgue_tail;
  if (_morgue_tail != nullptr) {
    _morgue_tail->next = node;
  } else {
    _morgue_head = node;
  }
  _morgue_tail = node;

  _morgue_count.fetch_add(1, std::memory_order_relaxed);
  _morgue_bytes.fetch_add(footprint, std::memory_order_relaxed);

  hs_hotlog_emit(HS_Ebin0_EMPTYBOX, HS_Ebin1_INFO, function_id, 0,
                 0, 0, 0, current_epoch,
                 static_cast<uint32_t>(footprint / 1024), "falline_morgue_staged");
  return true;
}

bool RedjetFalline::is_in_morgue(const void* obj_ptr) const noexcept {
  if (!obj_ptr) return false;
  hs_ticket_guard_t guard(const_cast<hs_ticket_lock_t&>(_morgue_lock));
  RedjetMorgueNode* cur = _morgue_head;
  while (cur != nullptr) {
    if (cur->obj_ptr == obj_ptr && !cur->is_resurrected.load(std::memory_order_acquire)) {
      return true;
    }
    cur = cur->next;
  }
  return false;
}

size_t RedjetFalline::advance_morgue_epoch(uint64_t current_epoch, uint64_t min_epoch_delay) {
  hs_ticket_guard_t guard(_morgue_lock);
  size_t expired_count = 0;

  RedjetMorgueNode* cur = _morgue_head;
  while (cur != nullptr) {
    RedjetMorgueNode* next = cur->next;

    if (!cur->is_resurrected.load(std::memory_order_relaxed) &&
        (current_epoch >= cur->condemned_epoch + min_epoch_delay)) {

      // Unlink expired node
      if (cur->prev != nullptr) {
        cur->prev->next = cur->next;
      } else {
        _morgue_head = cur->next;
      }
      if (cur->next != nullptr) {
        cur->next->prev = cur->prev;
      } else {
        _morgue_tail = cur->prev;
      }

      size_t fp = cur->footprint_bytes;
      void* ptr = cur->obj_ptr;

      if (_morgue_count.load(std::memory_order_relaxed) > 0) {
        _morgue_count.fetch_sub(1, std::memory_order_relaxed);
      }
      if (_morgue_bytes.load(std::memory_order_relaxed) >= fp) {
        _morgue_bytes.fetch_sub(fp, std::memory_order_relaxed);
      }

      // Convert expired morgue slot into an EmptyBox in the appropriate bin
      if (ptr != nullptr && fp >= sizeof(RedjetEmptyBox)) {
        auto* eb = reinterpret_cast<RedjetEmptyBox*>(ptr);
        register_emptybox(fp, eb, current_epoch);
      }

      delete cur;
      _morgue_expired_swept.fetch_add(1, std::memory_order_relaxed);
      expired_count++;
    }
    cur = next;
  }
  return expired_count;
}

bool RedjetFalline::attempt_resurrection(void* obj_ptr) {
  if (!obj_ptr) return false;
  _resurrection_requests.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_guard_t guard(_morgue_lock);
  RedjetMorgueNode* cur = _morgue_head;

  while (cur != nullptr) {
    if (cur->obj_ptr == obj_ptr && !cur->is_resurrected.load(std::memory_order_relaxed)) {
      // Check heuristic quota
      uint32_t active_q = static_cast<uint32_t>(_morgue_count.load(std::memory_order_relaxed));
      bool admit = g_redjet_resurrection_heuristics.should_admit_resurrection(active_q);
      if (!admit) {
        _resurrections_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      // Validate tail canary if object is live header
      auto* hdr = reinterpret_cast<HS_hotspot_live_header_t*>(obj_ptr);
      if (hdr->hsMN == HS_LIVE_MAGIC && hdr->memory_footprint.load(std::memory_order_relaxed) > 0) {
        if (!hdr->isTailCanaryValid()) {
          _resurrections_rejected.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
      }

      // Mark resurrected
      cur->is_resurrected.store(true, std::memory_order_release);
      hdr->falline_marked = 0;

      // Unlink from morgue
      if (cur->prev != nullptr) {
        cur->prev->next = cur->next;
      } else {
        _morgue_head = cur->next;
      }
      if (cur->next != nullptr) {
        cur->next->prev = cur->prev;
      } else {
        _morgue_tail = cur->prev;
      }

      size_t fp = cur->footprint_bytes;
      if (_morgue_count.load(std::memory_order_relaxed) > 0) {
        _morgue_count.fetch_sub(1, std::memory_order_relaxed);
      }
      if (_morgue_bytes.load(std::memory_order_relaxed) >= fp) {
        _morgue_bytes.fetch_sub(fp, std::memory_order_relaxed);
      }
      delete cur;

      _resurrections_succeeded.fetch_add(1, std::memory_order_relaxed);
      hs_hotlog_emit(HS_Ebin0_PROBE, HS_Ebin1_INFO, hdr->function_id, 0,
                     0, 0, 0, 0, 0, "falline_resurrection_ok");
      return true;
    }
    cur = cur->next;
  }

  _resurrections_rejected.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void RedjetFalline::print_on(outputStream* st) const {
  st->print_cr("RedjetFalline: %zu emptyboxes, hits: " UINT64_FORMAT ", misses: " UINT64_FORMAT ", splits: " UINT64_FORMAT
               ", morgue: %zu objects (%zu bytes), resurrections: " UINT64_FORMAT ", expired swept: " UINT64_FORMAT,
               _emptybox_count.load(), _snatch_hit_count.load(), _snatch_miss_count.load(), _split_count.load(),
               _morgue_count.load(), _morgue_bytes.load(), _resurrections_succeeded.load(), _morgue_expired_swept.load());
}
