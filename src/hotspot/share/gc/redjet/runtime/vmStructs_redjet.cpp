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

#include "precompiled.hpp"
#include "gc/redjet/runtime/vmStructs_redjet.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"

static uint32_t s_active_cores_shadow = 1;
static bool     s_switch_vfs_compaction = true;
static bool     s_os_emergency_decommit = true;

RedjetGlobalsForVMStructs::RedjetGlobalsForVMStructs()
  : _RedjetGCMaxTLABSize(&RedjetGCMaxTLABSize),
    _RedjetGCMinHeapExpand(&RedjetGCMinHeapExpand),
    _RedjetGCScheduler(&RedjetGCScheduler),
    _RedjetGCStrictVFS(&RedjetGCStrictVFS),
    _RedjetGCRMRP(&RedjetGCRMRP),
    _active_cores(&s_active_cores_shadow),
    _RedjetGCUseDeadReferenceSignals(&RedjetGCUseDeadReferenceSignals),
    _RedjetGCInvokesExplicitCleanHints(&RedjetGCInvokesExplicitCleanHints),
    _RedjetGCAutoScalarOnSIMDContention(&RedjetGCAutoScalarOnSIMDContention),
    _RedjetGCCriticalSIMDOnly(&RedjetGCCriticalSIMDOnly),
    _RedjetGCSwitchToVFSCompaction(&s_switch_vfs_compaction),
    _RedjetGCOSEmergencyDecommit(&s_os_emergency_decommit) {
}

RedjetGlobalsForVMStructs RedjetGlobalsForVMStructs::_instance;
RedjetGlobalsForVMStructs* RedjetGlobalsForVMStructs::_instance_p = &RedjetGlobalsForVMStructs::_instance;
