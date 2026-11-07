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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETHEAPVERIFIER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETHEAPVERIFIER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/shared/verifyOption.hpp"
#include "utilities/ostream.hpp"
#endif

class RedjetHeap;

struct RedjetVerificationReport {
  bool     passed{ true };
  uint64_t objects_verified{ 0 };
  uint64_t vfs_branches_verified{ 0 };
  uint64_t compressed_space_checks{ 0 };
  uint64_t tailtraces_slots_checked{ 0 };
  uint64_t rehab_candidates_scanned{ 0 };
  uint64_t anomalies_detected{ 0 };
};

class RedjetHeapVerifier {
private:
  static std::atomic<uint64_t> _total_verifications;
  static std::atomic<uint64_t> _total_anomalies;
  static std::atomic<uint64_t> _total_rehab_scans;

public:
#ifndef REDJET_STANDALONE_BUILD
  static RedjetVerificationReport verify_heap(RedjetHeap* heap, VerifyOption option);
#endif

  static size_t scan_rehab_arena(RedjetHeap* heap) noexcept;
  static bool   request_object_resurrection(RedjetHeap* heap, void* obj_ptr) noexcept;

  static uint64_t total_verifications() noexcept { return _total_verifications.load(std::memory_order_relaxed); }
  static uint64_t total_anomalies() noexcept     { return _total_anomalies.load(std::memory_order_relaxed); }
  static uint64_t total_rehab_scans() noexcept   { return _total_rehab_scans.load(std::memory_order_relaxed); }
};

#ifdef __cplusplus
extern "C" {
#endif

RJ_API int    HS_hotspot_request_resurrection(void* obj_ptr);
RJ_API size_t HS_hotspot_scan_rehab_arena(void);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_RUNTIME_REDJETHEAPVERIFIER_HPP
