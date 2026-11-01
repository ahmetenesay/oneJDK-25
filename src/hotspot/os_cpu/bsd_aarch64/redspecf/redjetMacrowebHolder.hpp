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

#ifndef OS_CPU_BSD_AARCH64_REDSPECF_REDJETMACROWEBHOLDER_HPP
#define OS_CPU_BSD_AARCH64_REDSPECF_REDJETMACROWEBHOLDER_HPP

#define REDJET_PAUSE_SMT() asm volatile("yield" ::: "memory")
#define REDJET_CYCLE_COUNTER() 0ULL
#define REDJET_CACHE_LINE_SIZE 64
#define REDJET_PAGE_SIZE 4096
#define REDJET_DEFAULT_SIMD_WIDTH 64
#define REDJET_PREFETCH_READ(addr) asm volatile("prfm pldl1keep, [%0]" :: "r"(addr))
#define REDJET_PREFETCH_WRITE(addr) asm volatile("prfm pstl1keep, [%0]" :: "r"(addr))
#define REDJET_HW_TOPOLOGY_SUPPORTED 1

#endif // OS_CPU_BSD_AARCH64_REDSPECF_REDJETMACROWEBHOLDER_HPP
