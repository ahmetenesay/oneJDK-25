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

#include "gc/redjet/engine/redjetRootScanner.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/oopStorageSetParState.inline.hpp"
#include "memory/universe.hpp"
#include "runtime/threads.hpp"
#include "runtime/safepoint.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"

class RedjetMarkOopClosure : public BasicOopIterateClosure {
private:
  RedjetHeap*       _heap;
  RedjetMarkBitMap* _bitmap;
  std::vector<oop>& _worklist;
  size_t&           _marked_count;

  template <class T>
  inline void process_oop(T* p) {
    T o = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(o)) {
      oop obj = CompressedOops::decode_not_null(o);
      if (_heap->is_in(obj) && _bitmap->par_mark((HeapWord*)obj)) {
        _marked_count++;
        _worklist.push_back(obj);
      }
    }
  }

public:
  RedjetMarkOopClosure(RedjetHeap* heap, RedjetMarkBitMap* bitmap, std::vector<oop>& worklist, size_t& marked_count) :
    _heap(heap), _bitmap(bitmap), _worklist(worklist), _marked_count(marked_count) {}

  void do_oop(oop* p) override {
    process_oop(p);
  }

  void do_oop(narrowOop* p) override {
    process_oop(p);
  }
};

class RedjetCLDMarkClosure : public CLDClosure {
private:
  RedjetMarkOopClosure* _oop_cl;

public:
  RedjetCLDMarkClosure(RedjetMarkOopClosure* oop_cl) : _oop_cl(oop_cl) {}

  void do_cld(ClassLoaderData* cld) override {
    cld->oops_do(_oop_cl, ClassLoaderData::_claim_strong);
  }
};

class RedjetCodeBlobMarkClosure : public CodeBlobClosure {
private:
  RedjetMarkOopClosure* _oop_cl;

public:
  RedjetCodeBlobMarkClosure(RedjetMarkOopClosure* oop_cl) : _oop_cl(oop_cl) {}

  void do_code_blob(CodeBlob* cb) override {
    nmethod* nm = cb->as_nmethod_or_null();
    if (nm != nullptr) {
      nm->oops_do(_oop_cl);
    }
  }
};
#endif

RedjetRootScanner::RedjetRootScanner(RedjetHeap* heap, RedjetMarkBitMap* bitmap) :
  _heap(heap),
  _bitmap(bitmap),
  _roots_scanned(0),
  _objects_marked(0) {}

size_t RedjetRootScanner::scan_roots_and_trace() {
  _roots_scanned = 0;
  _objects_marked = 0;

#ifndef REDJET_STANDALONE_BUILD
  if (_heap == nullptr || _bitmap == nullptr) {
    return 0;
  }

  assert(SafepointSynchronize::is_at_safepoint(), "RedjetRootScanner must run at safepoint");

  std::vector<oop> worklist;
  worklist.reserve(65536);

  RedjetMarkOopClosure mark_cl(_heap, _bitmap, worklist, _objects_marked);
  RedjetCLDMarkClosure cld_cl(&mark_cl);
  RedjetCodeBlobMarkClosure code_cl(&mark_cl);

  // 1. Scan Thread Stack Roots
  Threads::possibly_parallel_oops_do(false, &mark_cl, &code_cl);

  // 2. Scan HotSpot Strong OopStorages (JNI Handles, VM Global, StringTable)
  OopStorageSetStrongParState<false, false> strong_roots;
  strong_roots.oops_do(&mark_cl);

  // 3. Scan ClassLoaderDataGraph Roots
  ClassLoaderDataGraph::roots_cld_do(&cld_cl, &cld_cl);

  // 4. Scan Universe Roots
  Universe::oops_do(&mark_cl);

  // 5. CodeCache roots
  CodeCache::blobs_do(&code_cl);

  _roots_scanned = worklist.size();

  // 6. Transitive Graph Tracing: Drain worklist until complete
  while (!worklist.empty()) {
    oop obj = worklist.back();
    worklist.pop_back();
    obj->oop_iterate(&mark_cl);
  }

  log_info(gc)("Redjet RootScan: %zu roots scanned, %zu total live objects marked",
               _roots_scanned, _objects_marked);
#endif

  return _objects_marked;
}

void RedjetRootScanner::roots_do(OopClosure* oop_cl, CLDClosure* cld_cl, CodeBlobClosure* code_cl) {
#ifndef REDJET_STANDALONE_BUILD
  if (oop_cl != nullptr) {
    Threads::possibly_parallel_oops_do(false, oop_cl, code_cl);
    OopStorageSetStrongParState<false, false> strong_roots;
    strong_roots.oops_do(oop_cl);
    if (cld_cl != nullptr) {
      ClassLoaderDataGraph::roots_cld_do(cld_cl, cld_cl);
    }
    Universe::oops_do(oop_cl);
  }
#endif
}
