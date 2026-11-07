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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETSTRINGTABLECLEANER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETSTRINGTABLECLEANER_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/macros.hpp"

#ifndef REDJET_STANDALONE_BUILD
#include "memory/allStatic.hpp"
#include "runtime/thread.hpp"
#include "utilities/ostream.hpp"
#else
class AllStatic {};
class JavaThread {};
class outputStream {
public:
  template<typename... Args>
  void print_cr(const char* fmt, Args... args);
};
#endif

// ============================================================================
// REDJET STRING TABLE CLEANER (PLAN.md Section 8.10.2)
// Concurrent unlinking and purging of dead StringTable and SymbolTable entries
// with JavaThread::current() validation and 2-Points cooperative checks.
// ============================================================================
class RedjetStringTableCleaner : public AllStatic {
private:
  static std::atomic<uint64_t> _dead_strings_cleaned;
  static std::atomic<uint64_t> _dead_symbols_cleaned;
  static std::atomic<uint64_t> _total_cleaning_cycles;
  static std::atomic<uint64_t> _total_cleaning_time_us;
  static std::atomic<uint64_t> _skipped_cycles;
  static std::atomic<uint64_t> _table_rehash_checks;
  static std::atomic<uint64_t> _table_rehashes_triggered;
  static std::atomic<uint64_t> _dockpoint_checks;

public:
  // Execute concurrent cleaning of dead string table and symbol table entries
  static void clean_tables(bool process_symbols = true);

  // Individual cleaning phases with explicit JavaThread context
  static size_t clean_dead_strings(JavaThread* jt = nullptr);
  static size_t clean_dead_symbols(JavaThread* jt = nullptr);

  // Checks table load factors and triggers rehashing if bucket chains are long
  static bool check_and_rehash_tables();

  // Telemetry & metrics accessors
  static uint64_t dead_strings_cleaned()      noexcept { return _dead_strings_cleaned.load(std::memory_order_relaxed); }
  static uint64_t dead_symbols_cleaned()      noexcept { return _dead_symbols_cleaned.load(std::memory_order_relaxed); }
  static uint64_t total_cleaning_cycles()     noexcept { return _total_cleaning_cycles.load(std::memory_order_relaxed); }
  static uint64_t total_cleaning_time_us()    noexcept { return _total_cleaning_time_us.load(std::memory_order_relaxed); }
  static uint64_t skipped_cycles()            noexcept { return _skipped_cycles.load(std::memory_order_relaxed); }
  static uint64_t table_rehash_checks()       noexcept { return _table_rehash_checks.load(std::memory_order_relaxed); }
  static uint64_t table_rehashes_triggered()  noexcept { return _table_rehashes_triggered.load(std::memory_order_relaxed); }
  static uint64_t dockpoint_checks()          noexcept { return _dockpoint_checks.load(std::memory_order_relaxed); }

  static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETSTRINGTABLECLEANER_HPP
