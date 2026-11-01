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

#ifndef SHARE_GC_REDLET_REDLETROOTSCANNER_HPP
#define SHARE_GC_REDLET_REDLETROOTSCANNER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include "utilities/macros.hpp"
#include "gc/redlet/redletMarkBitMap.hpp"

#ifndef REDLET_STANDALONE_BUILD
#include "memory/iterator.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/javaThread.hpp"
#include "code/codeBlob.hpp"
#include "classfile/classLoaderData.hpp"
#else
class OopClosure {};
class CLDClosure {};
class CodeBlobClosure {};
#endif

class RedletHeap;

class RedletRootScanner {
private:
  RedletHeap*       _heap;
  RedletMarkBitMap* _bitmap;
  size_t            _roots_scanned;
  size_t            _objects_marked;

public:
  RedletRootScanner(RedletHeap* heap, RedletMarkBitMap* bitmap);
  ~RedletRootScanner() = default;

  // Scan all HotSpot VM & Thread roots and trace live reachable graph
  size_t scan_roots_and_trace();

  // Root iteration without marking
  void roots_do(OopClosure* oop_cl, CLDClosure* cld_cl = nullptr, CodeBlobClosure* code_cl = nullptr);

  size_t roots_scanned() const { return _roots_scanned; }
  size_t objects_marked() const { return _objects_marked; }
};

#endif // SHARE_GC_REDLET_REDLETROOTSCANNER_HPP
