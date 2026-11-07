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

#include "gc/redjet/runtime/redjetBarrierSet.hpp"
#include "gc/redjet/runtime/redjetBarrierSetAssembler.hpp"
#include "gc/redjet/runtime/redjetThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "runtime/javaThread.hpp"
#ifdef COMPILER1
#include "gc/redjet/runtime/redjetBarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/redjet/runtime/redjetBarrierSetC2.hpp"
#endif

class RedjetBarrierSetC1;
class RedjetBarrierSetC2;

RedjetBarrierSet::RedjetBarrierSet() : BarrierSet(
          make_barrier_set_assembler<RedjetBarrierSetAssembler>(),
          make_barrier_set_c1<RedjetBarrierSetC1>(),
          make_barrier_set_c2<RedjetBarrierSetC2>(),
          nullptr /* barrier_set_nmethod */,
          nullptr /* barrier_set_stack_chunk */,
          BarrierSet::FakeRtti(BarrierSet::RedjetBarrierSet)) {}

void RedjetBarrierSet::on_thread_create(Thread* thread) {
  RedjetThreadLocalData::create(thread);
}

void RedjetBarrierSet::on_thread_destroy(Thread* thread) {
  RedjetThreadLocalData::destroy(thread);
}
