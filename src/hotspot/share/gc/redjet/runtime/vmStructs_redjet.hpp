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

#ifndef SHARE_GC_REDJET_RUNTIME_VMSTRUCTS_REDJET_HPP
#define SHARE_GC_REDJET_RUNTIME_VMSTRUCTS_REDJET_HPP

#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#include "gc/redjet/vfs/redjetVFSNode.hpp"
#include "gc/redjet/runtime/redjetScheduler.hpp"
#include "gc/redjet/runtime/redjetGCMemoryManager.hpp"
#include "gc/redjet/runtime/redjetMemoryPool.hpp"
#include "gc/redjet/runtime/redjetReferenceProcessor.hpp"
#include "gc/redjet/runtime/redjetStringDedup.hpp"
#include "gc/redjet/runtime/redjetStringTableCleaner.hpp"
#include "gc/redjet/runtime/redjetClassUnloader.hpp"
#include "gc/redjet/runtime/redjetBarrierSet.hpp"
#include "gc/redjet/runtime/redjetBarrierSetNMethod.hpp"
#include "gc/redjet/runtime/redjetGCPolicyCounters.hpp"
#include "gc/redjet/runtime/redjetBPUDirector.hpp"
#include "gc/redjet/runtime/redjetUnifiedLogger.hpp"
#include "gc/redjet/simd/redjetSIMDContentionGovernor.hpp"
#include "gc/redjet/engine/redjetRootScanner.hpp"
#include "gc/redjet/engine/redjetSeasonalCoordinator.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetOSInterruptShield.hpp"
#include "gc/redjet/allocator/redjet_rmrp.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/api/redjetFastJNI.hpp"
#include "gc/redjet/api/redjetFastFFM.hpp"

// ============================================================================
// Serviceability Agent (SA) Globals & Macro Telemetry Holder for RedjetGC
// ============================================================================
class RedjetGlobalsForVMStructs {
  static RedjetGlobalsForVMStructs _instance;

public:
  static RedjetGlobalsForVMStructs* _instance_p;

  RedjetGlobalsForVMStructs();

  size_t*      _RedjetGCMaxTLABSize;
  size_t*      _RedjetGCMinHeapExpand;
  const char** _RedjetGCScheduler;
  bool*        _RedjetGCStrictVFS;
  bool*        _RedjetGCRMRP;
  uint32_t*    _active_cores;
  bool*        _RedjetGCUseDeadReferenceSignals;
  bool*        _RedjetGCInvokesExplicitCleanHints;
  bool*        _RedjetGCAutoScalarOnSIMDContention;
  bool*        _RedjetGCCriticalSIMDOnly;
  bool*        _RedjetGCSwitchToVFSCompaction;
  bool*        _RedjetGCOSEmergencyDecommit;
};

// ============================================================================
// VM_TYPES_REDJETGC: Type exports for Serviceability Agent
// ============================================================================
#define VM_TYPES_REDJETGC(declare_type, declare_toplevel_type, declare_integer_type) \
  declare_toplevel_type(RedjetGlobalsForVMStructs)                                   \
  declare_toplevel_type(RedjetHeap)                                                  \
  declare_toplevel_type(RedjetVFSTree)                                               \
  declare_toplevel_type(RedjetVFSNode)                                               \
  declare_toplevel_type(RedjetCompressedSpaces)                                      \
  declare_toplevel_type(RedjetSIMDContentionGovernor)                                \
  declare_toplevel_type(RedjetBPUDirector)                                           \
  declare_toplevel_type(RedjetUnifiedLogger)                                         \
  declare_toplevel_type(RedjetBareMetalScheduler)                                    \
  declare_toplevel_type(RedjetDomesticScheduler)                                     \
  declare_toplevel_type(RedjetReferenceProcessor)                                    \
  declare_toplevel_type(RedjetStringDedup)                                           \
  declare_toplevel_type(RedjetStringTableCleaner)                                    \
  declare_toplevel_type(RedjetClassUnloader)                                         \
  declare_toplevel_type(RedjetRootScanner)                                           \
  declare_toplevel_type(RedjetGCMemoryManager)                                       \
  declare_toplevel_type(RedjetGCPolicyCounters)                                      \
  declare_toplevel_type(RedjetUnifiedHeapPool)                                       \
  declare_toplevel_type(RedjetSeasonalCoordinator)                                   \
  declare_toplevel_type(RedjetPointCoordinator)                                      \
  declare_toplevel_type(RedjetOSInterruptShield)                                     \
  declare_toplevel_type(RedjetFastJNI)                                               \
  declare_toplevel_type(RedjetFastFFM)

// ============================================================================
// VM_STRUCTS_REDJETGC: Verified C++ Field Exports for Serviceability Agent
// ============================================================================
#define VM_STRUCTS_REDJETGC(nonstatic_field, volatile_nonstatic_field, static_field)              \
  static_field(RedjetGlobalsForVMStructs,   _instance_p,                        RedjetGlobalsForVMStructs*) \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCMaxTLABSize,               size_t*)                    \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCMinHeapExpand,             size_t*)                    \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCScheduler,                 const char**)               \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCStrictVFS,                 bool*)                      \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCRMRP,                      bool*)                      \
  nonstatic_field(RedjetGlobalsForVMStructs, _active_cores,                      uint32_t*)                  \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCUseDeadReferenceSignals,   bool*)                      \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCInvokesExplicitCleanHints, bool*)                      \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCAutoScalarOnSIMDContention, bool*)                     \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCCriticalSIMDOnly,          bool*)                      \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCSwitchToVFSCompaction,     bool*)                      \
  nonstatic_field(RedjetGlobalsForVMStructs, _RedjetGCOSEmergencyDecommit,       bool*)                      \
                                                                                                              \
  nonstatic_field(RedjetHeap,                _space,                             ContiguousSpace*)           \
  nonstatic_field(RedjetHeap,                _virtual_space,                     VirtualSpace)               \
  nonstatic_field(RedjetHeap,                _max_tlab_size,                     size_t)                     \
  nonstatic_field(RedjetHeap,                _vfs_tree,                          RedjetVFSTree)              \
  nonstatic_field(RedjetHeap,                _falline,                           RedjetFalline)              \
  nonstatic_field(RedjetHeap,                _runway,                            RedjetRunway)               \
  nonstatic_field(RedjetHeap,                _slab_allocator,                    RedjetSlabAllocator)        \
  nonstatic_field(RedjetHeap,                _landfill,                          RedjetLandfill)             \
  nonstatic_field(RedjetHeap,                _gc,                                RedjetGC)                   \
                                                                                                              \
  nonstatic_field(RedjetVFSTree,             _root,                              RedjetVFSNode*)             \
  nonstatic_field(RedjetVFSNode,             _name,                              char[64])                   \
  nonstatic_field(RedjetVFSNode,             _full_path,                         char[192])                  \
  nonstatic_field(RedjetVFSNode,             _quota_bytes,                       size_t)                     \
  nonstatic_field(RedjetVFSNode,             _is_on_heap,                        bool)                       \
  nonstatic_field(RedjetVFSNode,             _parent,                            RedjetVFSNode*)             \
                                                                                                              \
  nonstatic_field(RedjetBareMetalScheduler,  _active_cores,                      uint32_t)                   \
  nonstatic_field(RedjetDomesticScheduler,   _core_count,                        uint32_t)

// ============================================================================
// VM_INT_CONSTANTS_REDJETGC: Scoped Enum Constants Exported via Explicit Values
// ============================================================================
#define VM_INT_CONSTANTS_REDJETGC(declare_constant, declare_constant_with_value)                             \
  declare_constant_with_value("RedjetSchedulerMode::DOMESTIC", static_cast<int>(RedjetSchedulerMode::DOMESTIC)) \
  declare_constant_with_value("RedjetSchedulerMode::TBB",      static_cast<int>(RedjetSchedulerMode::TBB))      \
  declare_constant_with_value("RedjetMemoryDomain::JAVA_MEMORY",    static_cast<int>(RedjetMemoryDomain::JAVA_MEMORY))    \
  declare_constant_with_value("RedjetMemoryDomain::HOTSPOT_MEMORY", static_cast<int>(RedjetMemoryDomain::HOTSPOT_MEMORY)) \
  declare_constant_with_value("RedjetDutyType::DUTY_CARD_SCAN",       static_cast<int>(RedjetDutyType::DUTY_CARD_SCAN))       \
  declare_constant_with_value("RedjetDutyType::DUTY_BITMAP_MARK",     static_cast<int>(RedjetDutyType::DUTY_BITMAP_MARK))     \
  declare_constant_with_value("RedjetDutyType::DUTY_OBJECT_TRACE",    static_cast<int>(RedjetDutyType::DUTY_OBJECT_TRACE))    \
  declare_constant_with_value("RedjetDutyType::DUTY_EMPTYBOX_PURGE",  static_cast<int>(RedjetDutyType::DUTY_EMPTYBOX_PURGE))  \
  declare_constant_with_value("RedjetDutyType::DUTY_RMRP_RECLAIM",    static_cast<int>(RedjetDutyType::DUTY_RMRP_RECLAIM))    \
  declare_constant_with_value("RedjetDutyType::DUTY_STRING_DEDUP",    static_cast<int>(RedjetDutyType::DUTY_STRING_DEDUP))    \
  declare_constant_with_value("RedjetDutyType::DUTY_ROOT_SCAN",       static_cast<int>(RedjetDutyType::DUTY_ROOT_SCAN))       \
  declare_constant_with_value("RedjetDutyType::DUTY_FUSED_SUPERDUTY", static_cast<int>(RedjetDutyType::DUTY_FUSED_SUPERDUTY)) \
  declare_constant_with_value("RedjetSeason::SPRING_WARMUP",          static_cast<int>(RedjetSeason::SPRING_WARMUP))          \
  declare_constant_with_value("RedjetSeason::SUMMER_STEADY_STATE",    static_cast<int>(RedjetSeason::SUMMER_STEADY_STATE))    \
  declare_constant_with_value("RedjetSeason::AUTUMN_DIMENSION_SHIFT", static_cast<int>(RedjetSeason::AUTUMN_DIMENSION_SHIFT)) \
  declare_constant_with_value("RedjetSeason::WINTER_IDLE_REBATE",     static_cast<int>(RedjetSeason::WINTER_IDLE_REBATE))

#endif // SHARE_GC_REDJET_RUNTIME_VMSTRUCTS_REDJET_HPP
