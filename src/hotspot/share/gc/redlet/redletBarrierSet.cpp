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

#include "gc/redlet/redletBarrierSet.hpp"
#include "gc/redlet/redletThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "runtime/javaThread.hpp"
#ifdef COMPILER1
#include "gc/redlet/c1/redletBarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/redlet/c2/redletBarrierSetC2.hpp"
#endif

class RedletBarrierSetC1;
class RedletBarrierSetC2;

RedletBarrierSet::RedletBarrierSet() : BarrierSet(
          make_barrier_set_assembler<BarrierSetAssembler>(),
          make_barrier_set_c1<RedletBarrierSetC1>(),
          make_barrier_set_c2<RedletBarrierSetC2>(),
          nullptr /* barrier_set_nmethod */,
          nullptr /* barrier_set_stack_chunk */,
          BarrierSet::FakeRtti(BarrierSet::RedletBarrierSet)) {}

void RedletBarrierSet::on_thread_create(Thread* thread) {
  RedletThreadLocalData::create(thread);
}

void RedletBarrierSet::on_thread_destroy(Thread* thread) {
  RedletThreadLocalData::destroy(thread);
}
