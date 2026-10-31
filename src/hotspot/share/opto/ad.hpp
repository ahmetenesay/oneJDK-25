/*
 * Copyright (c) 2014, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OPTO_AD_HPP
#define SHARE_OPTO_AD_HPP

#include "utilities/macros.hpp"

// TableGen-generated instruction and register info
// Replaces legacy adlc-generated adfiles/ad
#if defined(TARGET_ARCH_x86)
#include "X86GenMachNode.inc"
#include "X86GenRegisterInfo.inc"
#elif defined(TARGET_ARCH_aarch64)
#include "AArch64GenMachNode.inc"
#include "AArch64GenRegisterInfo.inc"
#elif defined(TARGET_ARCH_riscv)
#include "RISCVGenMachNode.inc"
#include "RISCVGenRegisterInfo.inc"
#else
#include CPU_HEADER(adfiles/ad)
#endif

#endif // SHARE_OPTO_AD_HPP
