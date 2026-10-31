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

#ifndef SHARE_GC_REDLET_REDLETBARRIERSET_HPP
#define SHARE_GC_REDLET_REDLETBARRIERSET_HPP

#include "gc/shared/barrierSet.hpp"

class RedletBarrierSet: public BarrierSet {
  friend class VMStructs;

public:
  RedletBarrierSet();

  void print_on(outputStream *st) const override {}

  void on_thread_create(Thread* thread) override;
  void on_thread_destroy(Thread* thread) override;

  template <DecoratorSet decorators, typename BarrierSetT = RedletBarrierSet>
  class AccessBarrier: public BarrierSet::AccessBarrier<decorators, BarrierSetT> {};
};

template<>
struct BarrierSet::GetName<RedletBarrierSet> {
  static const BarrierSet::Name value = BarrierSet::RedletBarrierSet;
};

template<>
struct BarrierSet::GetType<BarrierSet::RedletBarrierSet> {
  typedef ::RedletBarrierSet type;
};

#endif // SHARE_GC_REDLET_REDLETBARRIERSET_HPP
