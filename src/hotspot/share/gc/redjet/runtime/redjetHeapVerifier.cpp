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

#include "gc/redjet/runtime/redjetHeapVerifier.hpp"
#include "gc/redjet/allocator/redjetCompressedSpaces.hpp"
#include "gc/redjet/api/redjetTailTraces.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "gc/redjet/allocator/redjetHeap.hpp"
#include "logging/log.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#endif

std::atomic<uint64_t> RedjetHeapVerifier::_total_verifications{ 0 };
std::atomic<uint64_t> RedjetHeapVerifier::_total_anomalies{ 0 };
std::atomic<uint64_t> RedjetHeapVerifier::_total_rehab_scans{ 0 };

#ifndef REDJET_STANDALONE_BUILD
namespace {
class RedjetVerifyObjectClosure : public ObjectClosure {
public:
  RedjetHeap*               _heap;
  RedjetVerificationReport* _rep;

  RedjetVerifyObjectClosure(RedjetHeap* heap, RedjetVerificationReport* rep)
    : _heap(heap), _rep(rep) {}

  void do_object(oop obj) override {
    if (obj == nullptr) return;
    _rep->objects_verified++;
    if (!_heap->is_in(obj)) {
      _rep->anomalies_detected++;
      _rep->passed = false;
      log_error(gc, verify)("RedjetHeapVerifier: oop " PTR_FORMAT " outside RedjetHeap bounds!", p2i(obj));
      return;
    }
    Klass* k = obj->klass_or_null();
    if (k == nullptr || !k->is_klass()) {
      _rep->anomalies_detected++;
      _rep->passed = false;
      log_error(gc, verify)("RedjetHeapVerifier: oop " PTR_FORMAT " has corrupt Klass pointer!", p2i(obj));
    }
  }
};
} // namespace

RedjetVerificationReport RedjetHeapVerifier::verify_heap(RedjetHeap* heap, VerifyOption option) {
  (void)option;
  RedjetVerificationReport rep{};
  if (heap == nullptr) {
    rep.passed = false;
    return rep;
  }

  _total_verifications.fetch_add(1, std::memory_order_relaxed);

  // 1. Verify Golden VFS 40-Branch Tetrasexagesimal Integrity
  if (!heap->vfs_tree()->verify_and_restore_golden_vfs()) {
    rep.passed = false;
    rep.anomalies_detected++;
  } else {
    rep.vfs_branches_verified = 40;
  }

  // 2. Verify CompressedSpaces & VFS CodeBoot Zero-Collision Invariants
  rep.compressed_space_checks = 4;
  if (RedjetCompressedSpaces::instance().is_initialized() &&
      !RedjetCompressedSpaces::instance().verify_isolation_invariants()) {
    rep.passed = false;
    rep.anomalies_detected++;
    log_error(gc, verify)("RedjetHeapVerifier: VFS CodeBoot / HeaderPane address collision detected!");
  }

  // 3. Walk live objects in allocation space
  RedjetVerifyObjectClosure cl(heap, &rep);
  heap->object_iterate(&cl);

  // 4. Verify TailTraces RPSS ring bounds
  uint64_t pending = g_redjet_tail_traces.pending_signals();
  rep.tailtraces_slots_checked = pending;
  if (pending > RedjetTailTraces::RPSS_RING_CAPACITY * 4) {
    log_warning(gc, verify)("RedjetHeapVerifier: High pending TailTraces RPSS backlog (%llu)",
                            static_cast<unsigned long long>(pending));
  }

  // 5. Audit Rehab Arena & Falline Morgue
  rep.rehab_candidates_scanned = scan_rehab_arena(heap);

  if (rep.anomalies_detected > 0) {
    _total_anomalies.fetch_add(rep.anomalies_detected, std::memory_order_relaxed);
  }
  log_info(gc, verify)("RedjetHeapVerifier: Verified %llu objects, %llu VFS branches, anomalies=%llu (%s)",
                       static_cast<unsigned long long>(rep.objects_verified),
                       static_cast<unsigned long long>(rep.vfs_branches_verified),
                       static_cast<unsigned long long>(rep.anomalies_detected),
                       rep.passed ? "PASSED" : "FAILED");
  return rep;
}
#endif // !REDJET_STANDALONE_BUILD

size_t RedjetHeapVerifier::scan_rehab_arena(RedjetHeap* heap) noexcept {
  _total_rehab_scans.fetch_add(1, std::memory_order_relaxed);
#ifndef REDJET_STANDALONE_BUILD
  if (heap == nullptr) return 0;
  size_t morgue_cnt = heap->falline()->morgue_count();
  size_t rehab_bytes = heap->vfs_tree()->get_used_bytes("/heap/on_heap/landfill/trash_pit/rehab");
  return morgue_cnt + (rehab_bytes / 64);
#else
  (void)heap;
  return 0;
#endif
}

bool RedjetHeapVerifier::request_object_resurrection(RedjetHeap* heap, void* obj_ptr) noexcept {
  if (obj_ptr == nullptr) return false;
  bool rescued_tt = g_redjet_tail_traces.rehabilitate_quarantined_object(obj_ptr);
#ifndef REDJET_STANDALONE_BUILD
  if (heap != nullptr && heap->falline() != nullptr) {
    bool rescued_fl = heap->falline()->attempt_resurrection(obj_ptr);
    return rescued_tt || rescued_fl;
  }
#else
  (void)heap;
#endif
  return rescued_tt;
}

extern "C" {

RJ_API int HS_hotspot_request_resurrection(void* obj_ptr) {
#ifndef REDJET_STANDALONE_BUILD
  if (Universe::heap() != nullptr && Universe::heap()->kind() == CollectedHeap::Redjet) {
    return RedjetHeapVerifier::request_object_resurrection(RedjetHeap::heap(), obj_ptr) ? 1 : 0;
  }
#endif
  return g_redjet_tail_traces.rehabilitate_quarantined_object(obj_ptr) ? 1 : 0;
}

RJ_API size_t HS_hotspot_scan_rehab_arena(void) {
#ifndef REDJET_STANDALONE_BUILD
  if (Universe::heap() != nullptr && Universe::heap()->kind() == CollectedHeap::Redjet) {
    return RedjetHeapVerifier::scan_rehab_arena(RedjetHeap::heap());
  }
#endif
  return 0;
}

} // extern "C"
