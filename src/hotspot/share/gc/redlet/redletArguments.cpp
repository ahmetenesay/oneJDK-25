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
#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletIntentCLI.hpp"
#include "gc/redlet/redletHeap.hpp"
#include "gc/redlet/redletArguments.hpp"
#include "gc/redlet/redletFlags.hpp"
#include "gc/redlet/redlet_globals.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

size_t RedletArguments::conservative_max_heap_alignment() {
  return UseLargePages ? os::large_page_size() : os::vm_page_size();
}

void RedletArguments::initialize() {
  GCArguments::initialize();

  assert(UseRedletGC, "Sanity");

  RedletFlags::init_defaults();
  const bool has_redlet_opts = (RedletOptions != nullptr && RedletOptions[0] != '\0');
  const bool has_sf_env = (std::getenv("MATHVANTAGE_OPTS") != nullptr) ||
                          (std::getenv("SF_VM_ARGS") != nullptr) ||
                          (std::getenv("SFVM_SC") != nullptr);

  if (has_redlet_opts) {
    RedletFlags::parse_flag_sequence(RedletOptions);
  }

  if (has_redlet_opts || has_sf_env) {
    sf_vm_config_t sf_cfg = parse_all_sf_flags(has_redlet_opts ? RedletOptions : nullptr, true);
    if (sf_cfg.has_flag_conflict) {
      log_warning(gc)("RedletGC +#SF| flag conflict detected & reconciled: %s", sf_cfg.conflict_msg);
    }
    if (sf_cfg.is_safety_critical && FLAG_IS_DEFAULT(RedletSCMode)) {
      FLAG_SET_ERGO(RedletSCMode, true);
    }
    if (sf_cfg.strings.scgc_type == sf_scgc_type_e::PRIMITIVE_RW && FLAG_IS_DEFAULT(RedletSCGCType)) {
      FLAG_SET_ERGO(RedletSCGCType, "PrimitiveRW");
    }
    if (sf_cfg.Forbid_Dynamic_Growth && FLAG_IS_DEFAULT(RedletForbidDynamicGrowth)) {
      FLAG_SET_ERGO(RedletForbidDynamicGrowth, true);
    }
    if (!sf_cfg.strings.strict_vfs_branch_validation && FLAG_IS_DEFAULT(RedletVFSStrict)) {
      FLAG_SET_ERGO(RedletVFSStrict, false);
    }
    if (sf_cfg.strings.reduce_memory_resident_propagation && FLAG_IS_DEFAULT(RedletRMRP)) {
      FLAG_SET_ERGO(RedletRMRP, true);
    }
    if (sf_cfg.strings.rmrp_method_explicit && FLAG_IS_DEFAULT(RedletRMRPMethod)) {
      FLAG_SET_ERGO(RedletRMRPMethod,
                    sf_cfg.strings.rmrp_method == sf_rmrp_method_e::SEQUENTIAL ? "Sequential" : "Plump");
    }
    if (sf_cfg.strings.enable_huge_pages && FLAG_IS_DEFAULT(UseLargePages)) {
      FLAG_SET_ERGO(UseLargePages, true);
    }
    if (sf_cfg.strings.Advance_Payment && FLAG_IS_DEFAULT(AlwaysPreTouch)) {
      FLAG_SET_ERGO(AlwaysPreTouch, true);
    }
  }

  if (RedletMaxTLABSize < MinTLABSize) {
    log_warning(gc)("RedletMaxTLABSize < MinTLABSize, adjusting it to %zu", MinTLABSize);
    RedletMaxTLABSize = MinTLABSize;
  }

  // Evaluate execution profile: NORMAX (Dynamic SIMD) vs SSIOLSC (Safety-Critical Deterministic)
  bool is_ssiolsc = RedletSCMode ||
                    (RedletProfile != nullptr && (strcasecmp(RedletProfile, "SSIOLSC") == 0 ||
                                                  strcasecmp(RedletProfile, "SC") == 0 ||
                                                  strcasecmp(RedletProfile, "SafetyCritical") == 0));

  if (is_ssiolsc) {
    if (FLAG_IS_DEFAULT(RedletSCMode)) {
      FLAG_SET_ERGO(RedletSCMode, true);
    }
    if (FLAG_IS_DEFAULT(AlwaysPreTouch)) {
      FLAG_SET_ERGO(AlwaysPreTouch, true);
    }
    if (FLAG_IS_DEFAULT(RedletForbidDynamicGrowth)) {
      FLAG_SET_ERGO(RedletForbidDynamicGrowth, true);
    }
    if (FLAG_IS_DEFAULT(RedletVFSStrict)) {
      FLAG_SET_ERGO(RedletVFSStrict, true);
    }
    SF_custominiVm_scgc_type_t sc_type =
        (RedletSCGCType != nullptr && strcasecmp(RedletSCGCType, "PrimitiveRW") == 0)
            ? SF_SCGC_TYPE_PRIMITIVE_RW
            : SF_SCGC_TYPE_DEFAULT;
    SacFor_Gearkit_SSIOLSC_Ex(sc_type);
    log_info(gc)("RedletGC Safety-Critical (SSIOLSC) Profile engaged:");
    log_info(gc)("  - Hard real-time determinism active (replacing soft heuristics)");
    log_info(gc)("  - Page pre-touch (AlwaysPreTouch) enforced to prevent page faults");
    log_info(gc)("  - Dynamic heap expansion forbidden (Zero-jitter execution envelope)");
    log_info(gc)("  - Strict VFS branch validation and memory isolation enforced");
  } else {
    SacFor_Gearkit_NORMAX();
    log_info(gc)("RedletGC NORMAX Profile engaged: Dynamic SIMD throughput with oneTBB heterogeneous work-stealing.");
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

void RedletArguments::initialize_alignments() {
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  size_t align = MAX2(os::vm_allocation_granularity(), page_size);
  SpaceAlignment = align;
  HeapAlignment  = align;
}

CollectedHeap* RedletArguments::create_heap() {
  return new RedletHeap();
}
