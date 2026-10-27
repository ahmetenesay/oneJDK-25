/*
 * Copyright (c) 2018, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/parallelCleaning.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/atomic.hpp"

// oneJDK-25: TBB concurrent_queue for nmethod claim — eliminates Atomic::cmpxchg contention
// by pre-populating a lock-free queue at construction. Workers pop nmethods from the queue
// instead of competing on a single atomic pointer, eliminating cache-line bouncing.

CodeCacheUnloadingTask::CodeCacheUnloadingTask(uint num_workers, bool unloading_occurred) :
  _unloading_occurred(unloading_occurred),
  _num_workers(num_workers) {
  // oneJDK-25: Populate TBB concurrent_queue with all alive nmethods.
  // Single-threaded at construction time, then workers pop in parallel.
  NMethodIterator iter(NMethodIterator::all);
  while (iter.next()) {
    _tbb_nmethod_queue.push(iter.method());
  }
}

CodeCacheUnloadingTask::~CodeCacheUnloadingTask() {
  CodeCache::verify_clean_inline_caches();
}

// oneJDK-25: TBB concurrent_queue path — lock-free, no Atomic::cmpxchg contention.
// Each worker pops nmethods from the queue until exhausted.
// This eliminates the original claim_nmethods() loop with Atomic::cmpxchg contention.
void CodeCacheUnloadingTask::work(uint worker_id) {
  nmethod* nm;
  while (_tbb_nmethod_queue.try_pop(nm)) {
    nm->do_unloading(_unloading_occurred);
  }
}

KlassCleaningTask::KlassCleaningTask() :
  _clean_klass_tree_claimed(0),
  _klass_iterator() {
}

bool KlassCleaningTask::claim_clean_klass_tree_task() {
  if (_clean_klass_tree_claimed) {
    return false;
  }

  return Atomic::cmpxchg(&_clean_klass_tree_claimed, 0, 1) == 0;
}

InstanceKlass* KlassCleaningTask::claim_next_klass() {
  Klass* klass;
  do {
    klass =_klass_iterator.next_klass();
  } while (klass != nullptr && !klass->is_instance_klass());

  // this can be null so don't call InstanceKlass::cast
  return static_cast<InstanceKlass*>(klass);
}

void KlassCleaningTask::work() {
  ResourceMark rm;

  // One worker will clean the subklass/sibling klass tree.
  if (claim_clean_klass_tree_task()) {
    Klass::clean_subklass_tree();
  }

  // All workers will help cleaning the classes,
  InstanceKlass* klass;
  while ((klass = claim_next_klass()) != nullptr) {
    clean_klass(klass);
  }
}
