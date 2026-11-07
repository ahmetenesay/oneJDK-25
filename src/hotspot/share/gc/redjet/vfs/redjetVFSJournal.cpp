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

#include "gc/redjet/vfs/redjetVFSJournal.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetVFSJournal* RedjetVFSJournal::_instance = nullptr;
std::atomic<bool> RedjetVFSJournal::_initialized{ false };

RedjetVFSJournal::RedjetVFSJournal()
  : _head_seq(0),
    _total_entries_written(0),
    _total_checkpoints_created(0),
    _total_heals_executed(0),
    _total_checksum_errors(0) {
  hs_ticket_lock_init(&_journal_lock);
  std::memset(_entries, 0, sizeof(_entries));
}

RedjetVFSJournal::~RedjetVFSJournal() {
}

void RedjetVFSJournal::initialize() {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetVFSJournal();
  }
}

void RedjetVFSJournal::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

uint64_t RedjetVFSJournal::log_op(RedjetJournalOp op, const char* path, size_t bytes_delta, uint32_t branch_hash) {
  uint64_t seq = _head_seq.fetch_add(1, std::memory_order_relaxed);
  size_t idx = seq & JOURNAL_MASK;

  RedjetJournalEntry* e = &_entries[idx];
  e->timestamp_tsc = hs_read_tsc();
  e->sequence_id = seq;
  e->branch_hash = branch_hash;
  e->op = op;
  e->status = 0;
  e->reserved = 0;
  e->bytes_delta = bytes_delta;

  if (path != nullptr) {
    std::strncpy(e->node_path, path, sizeof(e->node_path) - 1);
    e->node_path[sizeof(e->node_path) - 1] = '\0';
  } else {
    e->node_path[0] = '\0';
  }

  // Compute CRC-style 64-bit checksum
  uint64_t chk = 0xCBF29CE484222325ULL;
  chk = RedjetVFSCrypto::poly_eval_step(chk, seq);
  chk = RedjetVFSCrypto::poly_eval_step(chk, branch_hash);
  chk = RedjetVFSCrypto::poly_eval_step(chk, static_cast<uint64_t>(op));
  chk = RedjetVFSCrypto::poly_eval_step(chk, bytes_delta);
  e->checksum = chk;

  _total_entries_written.fetch_add(1, std::memory_order_relaxed);
  return seq;
}

uint64_t RedjetVFSJournal::commit_checkpoint(uint64_t golden_seed) {
  uint64_t seq = log_op(RedjetJournalOp::OP_CHECKPOINT, "checkpoint", 0, static_cast<uint32_t>(golden_seed));
  _total_checkpoints_created.fetch_add(1, std::memory_order_relaxed);
  return seq;
}

bool RedjetVFSJournal::verify_integrity() noexcept {
  uint64_t cur_head = _head_seq.load(std::memory_order_relaxed);
  size_t count = std::min<size_t>(cur_head, JOURNAL_CAPACITY);
  size_t start = (cur_head > count) ? (cur_head - count) : 0;

  bool ok = true;
  for (size_t i = 0; i < count; ++i) {
    size_t idx = (start + i) & JOURNAL_MASK;
    const RedjetJournalEntry* e = &_entries[idx];

    uint64_t chk = 0xCBF29CE484222325ULL;
    chk = RedjetVFSCrypto::poly_eval_step(chk, e->sequence_id);
    chk = RedjetVFSCrypto::poly_eval_step(chk, e->branch_hash);
    chk = RedjetVFSCrypto::poly_eval_step(chk, static_cast<uint64_t>(e->op));
    chk = RedjetVFSCrypto::poly_eval_step(chk, e->bytes_delta);

    if (chk != e->checksum) {
      _total_checksum_errors.fetch_add(1, std::memory_order_relaxed);
      ok = false;
    }
  }

  return ok;
}

bool RedjetVFSJournal::rollback_to_checkpoint(uint64_t checkpoint_seq) {
  uint64_t cur_head = _head_seq.load(std::memory_order_relaxed);
  if (checkpoint_seq >= cur_head) return false;

  hs_ticket_lock_acquire(&_journal_lock);

  // Rewind operations back to checkpoint
  for (uint64_t s = cur_head; s > checkpoint_seq; --s) {
    size_t idx = (s - 1) & JOURNAL_MASK;
    _entries[idx].status = 1; // Mark as rolled back
  }

  _head_seq.store(checkpoint_seq, std::memory_order_relaxed);
  _total_heals_executed.fetch_add(1, std::memory_order_relaxed);

  hs_ticket_lock_release(&_journal_lock);
  return true;
}

void RedjetVFSJournal::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet VFS Write-Ahead Log (WAL) & Journal ---");
  st->print_cr("  Journal Capacity:     %zu entries (%zu bytes)", JOURNAL_CAPACITY, JOURNAL_CAPACITY * sizeof(RedjetJournalEntry));
  st->print_cr("  Current Sequence:     %llu", static_cast<unsigned long long>(_head_seq.load(std::memory_order_relaxed)));
  st->print_cr("  Entries Written:      %llu", static_cast<unsigned long long>(_total_entries_written.load(std::memory_order_relaxed)));
  st->print_cr("  Checkpoints Created:  %llu", static_cast<unsigned long long>(_total_checkpoints_created.load(std::memory_order_relaxed)));
  st->print_cr("  Heals Executed:       %llu", static_cast<unsigned long long>(_total_heals_executed.load(std::memory_order_relaxed)));
  st->print_cr("  Checksum Errors:      %llu", static_cast<unsigned long long>(_total_checksum_errors.load(std::memory_order_relaxed)));
}
