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

#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletMemoryPool.hpp"

RedletMemoryPool::RedletMemoryPool(RedletHeap* heap) :
  CollectedMemoryPool("Redlet Heap",
                      heap->capacity(),
                      heap->max_capacity(),
                      false),
  _heap(heap) {
  assert(UseRedletGC, "sanity");
}

size_t RedletMemoryPool::committed_in_bytes() {
  return _heap->capacity();
}

size_t RedletMemoryPool::used_in_bytes() {
  return _heap->used();
}

size_t RedletMemoryPool::max_size() const {
  return _heap->max_capacity();
}

MemoryUsage RedletMemoryPool::get_memory_usage() {
  size_t initial_sz = initial_size();
  size_t max_sz     = max_size();
  size_t used       = used_in_bytes();
  size_t committed  = committed_in_bytes();

  return MemoryUsage(initial_sz, used, committed, max_sz);
}
