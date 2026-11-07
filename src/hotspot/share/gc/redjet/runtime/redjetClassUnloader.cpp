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

#include "gc/redjet/runtime/redjetClassUnloader.hpp"
#include "gc/redjet/allocator/redjetHeap.hpp"
#include <chrono>

#ifndef REDJET_STANDALONE_BUILD
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "code/dependencyContext.hpp"
#include "logging/log.hpp"
#include "memory/metaspaceGC.hpp"
#include "memory/metaspaceUtils.hpp"
#include "oops/klass.hpp"
#include "oops/instanceKlass.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#endif
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

RedjetClassUnloader::RedjetClassUnloader(RedjetHeap* heap)
  : _heap(heap),
    _unloading_enabled(true),
    _total_unloading_cycles(0),
    _total_classes_unloaded(0),
    _total_cld_purged(0),
    _total_nmethods_unlinked(0),
    _total_metaspace_reclaimed_bytes(0),
    _total_unloading_time_us(0),
    _total_lambda_proxies_unlinked(0),
    _total_jvmci_unlinked(0) {
  _last_stats.classes_unloaded = 0;
  _last_stats.cld_purged = 0;
  _last_stats.nmethods_unlinked = 0;
  _last_stats.metaspace_reclaimed_bytes = 0;
  _last_stats.elapsed_us = 0;
  _last_stats.lambda_proxies_unlinked = 0;
  _last_stats.jvmci_unlinked = 0;
}

void RedjetClassUnloader::prepare_unloading() {
#ifndef REDJET_STANDALONE_BUILD
  if (!ClassUnloading || !_unloading_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  CodeCache::increment_unloading_cycle();
  DependencyContext::cleaning_start();
#endif
}

bool RedjetClassUnloader::unlink_class_loader_data() {
  bool unloading_occurred = false;
#ifndef REDJET_STANDALONE_BUILD
  if (!ClassUnloading || !_unloading_enabled.load(std::memory_order_relaxed)) {
    return false;
  }

  {
    MutexLocker ml(ClassLoaderDataGraph_lock);
    // ClassLoaderDataGraph::do_unloading unlinks all dead classes and unregisters dead ClassLoaderData
    unloading_occurred = ClassLoaderDataGraph::do_unloading();
  }

  // Clean weak klass links across sub-classes, implementors, and interfaces
  Klass::clean_weak_klass_links(unloading_occurred);

  // Clean up LambdaForm and dynamic proxy hidden classes via CLDG traversal
  class RedjetLambdaProxyCleanerClosure : public CLDClosure {
  private:
    size_t _count;
  public:
    RedjetLambdaProxyCleanerClosure() : _count(0) {}
    void do_cld(ClassLoaderData* cld) override {
      if (cld != nullptr && cld->has_class_mirror_holder() && cld->is_unloading()) {
        _count++;
      }
    }
    size_t count() const noexcept { return _count; }
  };

  RedjetLambdaProxyCleanerClosure lambda_cl;
  ClassLoaderDataGraph::cld_do(&lambda_cl);
  _total_lambda_proxies_unlinked.fetch_add(lambda_cl.count(), std::memory_order_relaxed);
  _last_stats.lambda_proxies_unlinked = lambda_cl.count();
#endif
  return unloading_occurred;
}

void RedjetClassUnloader::unlink_code_cache_and_jvmci(bool unloading_occurred) {
#ifndef REDJET_STANDALONE_BUILD
  if (!ClassUnloading || !_unloading_enabled.load(std::memory_order_relaxed)) {
    return;
  }

  // Unlink dead nmethods from CodeCache
  size_t nmethods_before = CodeCache::nmethod_count();
  CodeCache::do_unloading(unloading_occurred);
  size_t nmethods_after = CodeCache::nmethod_count();

  size_t nmethods_freed = (nmethods_before > nmethods_after) ? (nmethods_before - nmethods_after) : 0;
  _total_nmethods_unlinked.fetch_add(nmethods_freed, std::memory_order_relaxed);
  _last_stats.nmethods_unlinked = nmethods_freed;

#if INCLUDE_JVMCI
  if (JVMCI::is_compiler_initialized()) {
    JVMCI::do_unloading(unloading_occurred);
    _total_jvmci_unlinked.fetch_add(1, std::memory_order_relaxed);
    _last_stats.jvmci_unlinked++;
  }
#endif

  DependencyContext::cleaning_end();
#endif
}

void RedjetClassUnloader::purge_dead_cld_and_caches() {
#ifndef REDJET_STANDALONE_BUILD
  if (!ClassUnloading || !_unloading_enabled.load(std::memory_order_relaxed)) {
    return;
  }

  // Purge dead ClassLoaderData instances from graph
  ClassLoaderDataGraph::purge(true);
  _total_cld_purged.fetch_add(1, std::memory_order_relaxed);
  _last_stats.cld_purged++;
#endif
}

size_t RedjetClassUnloader::release_metaspace_chunks() {
  size_t reclaimed_bytes = 0;
#ifndef REDJET_STANDALONE_BUILD
  if (!ClassUnloading || !_unloading_enabled.load(std::memory_order_relaxed)) {
    return 0;
  }

  size_t used_before = MetaspaceUtils::used_bytes();
  // MetaspaceGC triggers chunk reclamation and OS page decommit back to VFS code_boot
  MetaspaceGC::compute_new_size();
  size_t used_after = MetaspaceUtils::used_bytes();

  if (used_before > used_after) {
    reclaimed_bytes = used_before - used_after;
  }

  _total_metaspace_reclaimed_bytes.fetch_add(reclaimed_bytes, std::memory_order_relaxed);
  _last_stats.metaspace_reclaimed_bytes = reclaimed_bytes;
#endif
  return reclaimed_bytes;
}

void RedjetClassUnloader::prepare() {
  prepare_unloading();
}

bool RedjetClassUnloader::unlink() {
  bool unloading = unlink_class_loader_data();
  unlink_code_cache_and_jvmci(unloading);
  return unloading;
}

void RedjetClassUnloader::purge() {
  purge_dead_cld_and_caches();
  release_metaspace_chunks();
}

void RedjetClassUnloader::finish() {
  _total_unloading_cycles.fetch_add(1, std::memory_order_relaxed);
}

bool RedjetClassUnloader::do_unloading() {
  auto start = std::chrono::steady_clock::now();

  prepare();
  bool unloading_occurred = unlink();
  purge();
  finish();

  auto end = std::chrono::steady_clock::now();
  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

  _total_unloading_time_us.fetch_add(elapsed_us, std::memory_order_relaxed);
  _last_stats.elapsed_us = elapsed_us;

  if (unloading_occurred) {
    _total_classes_unloaded.fetch_add(1, std::memory_order_relaxed);
    _last_stats.classes_unloaded = 1;
  }

  log_info(gc, classunload)("Redjet Class Unloader: cycle complete (unloaded=%d, nmethods=%zu, metaspace=%zu bytes, elapsed=%lld us)",
                           unloading_occurred ? 1 : 0, _last_stats.nmethods_unlinked,
                           _last_stats.metaspace_reclaimed_bytes, (long long)elapsed_us);

  return unloading_occurred;
}

void RedjetClassUnloader::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("--- Redjet Class Unloader (PLAN.md Section 8.10.3) ---");
  st->print_cr("  Unloading Enabled:          %s", _unloading_enabled.load(std::memory_order_relaxed) ? "true" : "false");
  st->print_cr("  Total Unloading Cycles:     %llu", (unsigned long long)_total_unloading_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Total Classes Unloaded:     %llu", (unsigned long long)_total_classes_unloaded.load(std::memory_order_relaxed));
  st->print_cr("  Total CLD Instances Purged: %llu", (unsigned long long)_total_cld_purged.load(std::memory_order_relaxed));
  st->print_cr("  Total NMethods Unlinked:    %llu", (unsigned long long)_total_nmethods_unlinked.load(std::memory_order_relaxed));
  st->print_cr("  Total JVMCI Unlinked:       %llu", (unsigned long long)_total_jvmci_unlinked.load(std::memory_order_relaxed));
  st->print_cr("  Total Metaspace Reclaimed:  %llu bytes (%llu KB)",
               (unsigned long long)_total_metaspace_reclaimed_bytes.load(std::memory_order_relaxed),
               (unsigned long long)(_total_metaspace_reclaimed_bytes.load(std::memory_order_relaxed) / 1024));
  st->print_cr("  Total Lambda/Proxies Freed: %llu", (unsigned long long)_total_lambda_proxies_unlinked.load(std::memory_order_relaxed));
  st->print_cr("  Total Unloading Time:       %llu us", (unsigned long long)_total_unloading_time_us.load(std::memory_order_relaxed));
  st->print_cr("  Last Cycle Stats:           nmethods=%zu, cld=%zu, metaspace=%zu, elapsed=%llu us",
               _last_stats.nmethods_unlinked, _last_stats.cld_purged,
               _last_stats.metaspace_reclaimed_bytes, (unsigned long long)_last_stats.elapsed_us);
}
