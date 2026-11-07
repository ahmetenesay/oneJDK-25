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

#include "gc/redjet/runtime/redjetStringTableCleaner.hpp"
#include <chrono>

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/thread.hpp"
#include "runtime/javaThread.hpp"
#include "gc/shared/oopStorageSet.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#endif

std::atomic<uint64_t> RedjetStringTableCleaner::_dead_strings_cleaned{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_dead_symbols_cleaned{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_total_cleaning_cycles{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_total_cleaning_time_us{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_skipped_cycles{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_table_rehash_checks{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_table_rehashes_triggered{ 0 };
std::atomic<uint64_t> RedjetStringTableCleaner::_dockpoint_checks{ 0 };

size_t RedjetStringTableCleaner::clean_dead_strings(JavaThread* jt) {
  size_t count = 0;
#ifndef REDJET_STANDALONE_BUILD
  JavaThread* current_jt = jt;
  if (current_jt == nullptr) {
    Thread* thread = Thread::current_or_null();
    if (thread != nullptr && thread->is_Java_thread()) {
      current_jt = JavaThread::cast(thread);
    }
  }

  if (current_jt != nullptr) {
    size_t before = OopStorageSet::string_table_weak()->allocation_count();
    // StringTable::clean_dead_entries unlinks dead interned strings from the concurrent hash table
    StringTable::clean_dead_entries(current_jt);
    size_t after = OopStorageSet::string_table_weak()->allocation_count();
    count = (before > after) ? (before - after) : 0;
  } else {
    _skipped_cycles.fetch_add(1, std::memory_order_relaxed);
  }
#else
  count = 0;
#endif
  _dead_strings_cleaned.fetch_add(count, std::memory_order_relaxed);
  return count;
}

size_t RedjetStringTableCleaner::clean_dead_symbols(JavaThread* jt) {
  size_t count = 0;
#ifndef REDJET_STANDALONE_BUILD
  JavaThread* current_jt = jt;
  if (current_jt == nullptr) {
    Thread* thread = Thread::current_or_null();
    if (thread != nullptr && thread->is_Java_thread()) {
      current_jt = JavaThread::cast(thread);
    }
  }

  if (current_jt != nullptr) {
    size_t before = SymbolTable::table_size();
    // SymbolTable::clean_dead_entries unlinks unreferenced symbols from the concurrent hash table
    SymbolTable::clean_dead_entries(current_jt);
    size_t after = SymbolTable::table_size();
    count = (before > after) ? (before - after) : 0;
  } else {
    _skipped_cycles.fetch_add(1, std::memory_order_relaxed);
  }
#else
  count = 0;
#endif
  _dead_symbols_cleaned.fetch_add(count, std::memory_order_relaxed);
  return count;
}

bool RedjetStringTableCleaner::check_and_rehash_tables() {
  _table_rehash_checks.fetch_add(1, std::memory_order_relaxed);
#ifndef REDJET_STANDALONE_BUILD
  // Check if StringTable or SymbolTable needs rehashing
  bool string_rehash = StringTable::needs_rehashing();
  bool symbol_rehash = SymbolTable::needs_rehashing();

  if (string_rehash) {
    StringTable::rehash_table();
  }
  if (symbol_rehash) {
    SymbolTable::rehash_table();
  }

  if (string_rehash || symbol_rehash) {
    _table_rehashes_triggered.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
#else
  return false;
#endif
}

void RedjetStringTableCleaner::clean_tables(bool process_symbols) {
  auto start = std::chrono::steady_clock::now();

#ifndef REDJET_STANDALONE_BUILD
  Thread* thread = Thread::current_or_null();
  JavaThread* jt = (thread != nullptr && thread->is_Java_thread()) ? JavaThread::cast(thread) : nullptr;
#else
  JavaThread* jt = nullptr;
#endif

  _dockpoint_checks.fetch_add(1, std::memory_order_relaxed);

  size_t strings_cleaned = clean_dead_strings(jt);
  size_t symbols_cleaned = 0;

  if (process_symbols) {
    symbols_cleaned = clean_dead_symbols(jt);
  }

  // Periodic table health & rehash inspection
  check_and_rehash_tables();

  auto end = std::chrono::steady_clock::now();
  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

  _total_cleaning_cycles.fetch_add(1, std::memory_order_relaxed);
  _total_cleaning_time_us.fetch_add(elapsed_us, std::memory_order_relaxed);

  log_info(gc, stringtable)("Redjet StringTable Clean: dead strings=%zu, dead symbols=%zu (%lld us)",
                           strings_cleaned, symbols_cleaned, (long long)elapsed_us);
}

void RedjetStringTableCleaner::print_on(outputStream* st) {
  if (st == nullptr) return;
  st->print_cr("--- Redjet String & Symbol Table Cleaner ---");
  st->print_cr("  Cleaning Cycles:         %llu", (unsigned long long)_total_cleaning_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Total Cleaning Time:     %llu us", (unsigned long long)_total_cleaning_time_us.load(std::memory_order_relaxed));
  st->print_cr("  Dead String Batches:     %llu", (unsigned long long)_dead_strings_cleaned.load(std::memory_order_relaxed));
  st->print_cr("  Dead Symbol Batches:     %llu", (unsigned long long)_dead_symbols_cleaned.load(std::memory_order_relaxed));
  st->print_cr("  Skipped Cycles (No JT):  %llu", (unsigned long long)_skipped_cycles.load(std::memory_order_relaxed));
  st->print_cr("  Rehash Inspection Ticks: %llu", (unsigned long long)_table_rehash_checks.load(std::memory_order_relaxed));
  st->print_cr("  Rehashes Triggered:      %llu", (unsigned long long)_table_rehashes_triggered.load(std::memory_order_relaxed));
  st->print_cr("  Dockpoint Checks:        %llu", (unsigned long long)_dockpoint_checks.load(std::memory_order_relaxed));
}
