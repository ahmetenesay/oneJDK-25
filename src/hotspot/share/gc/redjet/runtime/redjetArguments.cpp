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

#include <cstdlib>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetIntentCLI.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "gc/redjet/runtime/redjetArguments.hpp"
#include "gc/redjet/runtime/redjetFlags.hpp"
#include "gc/redjet/runtime/redjet_globals.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

size_t RedjetArguments::conservative_max_heap_alignment() {
  return UseLargePages ? os::large_page_size() : os::vm_page_size();
}

void RedjetArguments::initialize() {
  GCArguments::initialize();

  assert(UseRedjetGC, "Sanity");

  RedjetFlags::init_defaults();
  const bool has_redjet_opts = (RedjetOptions != nullptr && RedjetOptions[0] != '\0');
  const bool has_hs_env = (std::getenv("MATHVANTAGE_OPTS") != nullptr) ||
                          (std::getenv("HS_VM_ARGS") != nullptr) ||
                          (std::getenv("HSVM_SC") != nullptr);

  if (has_redjet_opts) {
    RedjetFlags::parse_flag_sequence(RedjetOptions);
  }

  if (has_redjet_opts || has_hs_env) {
    hs_vm_config_t hs_cfg = parse_all_hs_flags(has_redjet_opts ? RedjetOptions : nullptr, true);
    if (hs_cfg.has_flag_conflict) {
      log_warning(gc)("RedjetGC +#HS| flag conflict detected & reconciled: %s", hs_cfg.conflict_msg);
    }
    if (hs_cfg.is_safety_critical && FLAG_IS_DEFAULT(RedjetSCMode)) {
      FLAG_SET_ERGO(RedjetSCMode, true);
    }
    if (hs_cfg.strings.scgc_type == hs_scgc_type_e::PRIMITIVE_RW && FLAG_IS_DEFAULT(RedjetSCGCType)) {
      FLAG_SET_ERGO(RedjetSCGCType, "PrimitiveRW");
    }
    if (hs_cfg.Forbid_Dynamic_Growth && FLAG_IS_DEFAULT(RedjetForbidDynamicGrowth)) {
      FLAG_SET_ERGO(RedjetForbidDynamicGrowth, true);
    }
    if (!hs_cfg.strings.strict_vfs_branch_validation && FLAG_IS_DEFAULT(RedjetVFSStrict)) {
      FLAG_SET_ERGO(RedjetVFSStrict, false);
    }
    if (hs_cfg.strings.reduce_memory_resident_propagation && FLAG_IS_DEFAULT(RedjetRMRP)) {
      FLAG_SET_ERGO(RedjetRMRP, true);
    }
    if (hs_cfg.strings.rmrp_method_explicit && FLAG_IS_DEFAULT(RedjetRMRPMethod)) {
      FLAG_SET_ERGO(RedjetRMRPMethod,
                    hs_cfg.strings.rmrp_method == hs_rmrp_method_e::SEQUENTIAL ? "Sequential" : "Plump");
    }
    if (hs_cfg.strings.enable_huge_pages && FLAG_IS_DEFAULT(UseLargePages)) {
      FLAG_SET_ERGO(UseLargePages, true);
    }
    if (hs_cfg.strings.Advance_Payment && FLAG_IS_DEFAULT(AlwaysPreTouch)) {
      FLAG_SET_ERGO(AlwaysPreTouch, true);
    }
  }

  if (RedjetMaxTLABSize < MinTLABSize) {
    log_warning(gc)("RedjetMaxTLABSize < MinTLABSize, adjusting it to %zu", MinTLABSize);
    RedjetMaxTLABSize = MinTLABSize;
  }

  // Evaluate execution profile: NORMAX (Dynamic SIMD) vs SSIOLSC (Safety-Critical Deterministic)
  bool is_ssiolsc = RedjetSCMode ||
                    (RedjetProfile != nullptr && (strcasecmp(RedjetProfile, "SSIOLSC") == 0 ||
                                                  strcasecmp(RedjetProfile, "SC") == 0 ||
                                                  strcasecmp(RedjetProfile, "SafetyCritical") == 0));

  if (is_ssiolsc) {
    if (FLAG_IS_DEFAULT(RedjetSCMode)) {
      FLAG_SET_ERGO(RedjetSCMode, true);
    }
    if (FLAG_IS_DEFAULT(AlwaysPreTouch)) {
      FLAG_SET_ERGO(AlwaysPreTouch, true);
    }
    if (FLAG_IS_DEFAULT(RedjetForbidDynamicGrowth)) {
      FLAG_SET_ERGO(RedjetForbidDynamicGrowth, true);
    }
    if (FLAG_IS_DEFAULT(RedjetVFSStrict)) {
      FLAG_SET_ERGO(RedjetVFSStrict, true);
    }
    HS_hotspot_scgc_type_t sc_type =
        (RedjetSCGCType != nullptr && strcasecmp(RedjetSCGCType, "PrimitiveRW") == 0)
            ? HS_SCGC_TYPE_PRIMITIVE_RW
            : HS_SCGC_TYPE_DEFAULT;
    SacFor_Gearkit_SSIOLSC_Ex(sc_type);
    log_info(gc)("RedjetGC Safety-Critical (SSIOLSC) Profile engaged:");
    log_info(gc)("  - Hard real-time determinism active (replacing soft heuristics)");
    log_info(gc)("  - Page pre-touch (AlwaysPreTouch) enforced to prevent page faults");
    log_info(gc)("  - Dynamic heap expansion forbidden (Zero-jitter execution envelope)");
    log_info(gc)("  - Strict VFS branch validation and memory isolation enforced");
  } else {
    SacFor_Gearkit_NORMAX();
    log_info(gc)("RedjetGC NORMAX Profile engaged: Dynamic SIMD throughput with oneTBB heterogeneous work-stealing.");
  }

#ifdef COMPILER2
  if (FLAG_IS_DEFAULT(UseCountedLoopSafepoints)) {
    FLAG_SET_DEFAULT(UseCountedLoopSafepoints, true);
    if (FLAG_IS_DEFAULT(LoopStripMiningIter)) {
      FLAG_SET_DEFAULT(LoopStripMiningIter, 1000);
    }
  }
#endif
}

void RedjetArguments::initialize_alignments() {
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  size_t align = MAX2(os::vm_allocation_granularity(), page_size);
  SpaceAlignment = align;
  HeapAlignment  = align;
}

CollectedHeap* RedjetArguments::create_heap() {
  return new RedjetHeap();
}
