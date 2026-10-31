/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OPTO_C2_GLOBALS_PD_HPP
#define SHARE_OPTO_C2_GLOBALS_PD_HPP

#include "runtime/globals_shared.hpp"
#include "utilities/macros.hpp"

// TableGen-generated C2 globals
// Replaces legacy adlc-generated CPU_HEADER(c2_globals)
#if defined(TARGET_ARCH_x86)
#include "X86GenC2Globals.inc"
#elif defined(TARGET_ARCH_aarch64)
#include "AArch64GenC2Globals.inc"
#elif defined(TARGET_ARCH_riscv)
#include "RISCVGenC2Globals.inc"
#else
#include CPU_HEADER(c2_globals)
#endif
#include OS_HEADER(c2_globals)

#endif // SHARE_OPTO_C2_GLOBALS_PD_HPP
