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

#ifndef SHARE_GC_REDJET_VFS_REDJETCODESEALTRAP_HPP
#define SHARE_GC_REDJET_VFS_REDJETCODESEALTRAP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifdef REDJET_STANDALONE_BUILD
#include <cstdio>
#ifndef OUTPUTSTREAM_FALLBACK_DEFINED
#define OUTPUTSTREAM_FALLBACK_DEFINED
class outputStream {
public:
  template<typename... Args>
  void print(const char* fmt, Args... args) {
    printf(fmt, args...);
  }
  template<typename... Args>
  void print_cr(const char* fmt, Args... args) {
    printf(fmt, args...);
    printf("\n");
  }
};
#endif
#else
#include "memory/allocation.hpp"
#include "utilities/ostream.hpp"
#endif

// ============================================================================
// Redjet Code Seal Trap & Bit-Flip Tamper Detector (PLAN.md Section 4.2, 6.2, 9.1)
// - Cryptographically verifies W^X code pages in VFS code_boot branch
// - Monitors executable pages against unauthorized modification / Rowhammer bit flips
// - Computes high-throughput 64-bit SipHash/Poly1305 style cryptographic code seals
// - Triggers immediate hardware trap & trampoline invalidation on integrity failure
// ============================================================================

enum class RedjetSealStatus : uint8_t {
  SEAL_VALID           = 0,
  SEAL_BIT_FLIP_TRAPPED = 1,
  SEAL_INVALID_PAGE    = 2,
  SEAL_PAGE_MODIFIED   = 3
};

struct RedjetSealEntry {
  void*    code_ptr{ nullptr };
  size_t   code_size{ 0 };
  uint32_t method_id{ 0 };
  uint64_t expected_seal{ 0 };
  uint64_t last_verified_tsc{ 0 };
  bool     is_active{ false };
  bool     is_compromised{ false };
};

class RedjetCodeSealTrap {
public:
  static constexpr size_t MAX_SEALED_ENTRIES = 4096;
  static constexpr uint64_t GOLDEN_SEED_K0   = 0x736f6d6570736575ULL;
  static constexpr uint64_t GOLDEN_SEED_K1   = 0x646f72616e646f6dULL;

private:
  static RedjetCodeSealTrap* _instance;
  static std::atomic<bool>   _initialized;

  RedjetSealEntry            _entries[MAX_SEALED_ENTRIES];
  size_t                     _entry_count{ 0 };
  hs_ticket_lock_t           _trap_lock;

  // Telemetry
  std::atomic<uint64_t>      _total_pages_registered{ 0 };
  std::atomic<uint64_t>      _total_verifications{ 0 };
  std::atomic<uint64_t>      _total_bitflips_trapped{ 0 };
  std::atomic<uint64_t>      _total_scan_cycles{ 0 };

  static uint64_t compute_code_seal(const void* data, size_t size, uint64_t k0, uint64_t k1) noexcept;

public:
  RedjetCodeSealTrap();
  ~RedjetCodeSealTrap();

  static void initialize();
  static void destroy();
  static RedjetCodeSealTrap* get() noexcept { return _instance; }

  // 1. Register Code Page Seal
  RJ_API bool register_code_seal(void* code_ptr, size_t code_size, uint32_t method_id);

  // 2. Unregister Code Page Seal
  RJ_API bool unregister_code_seal(void* code_ptr);

  // 3. Verify Single Code Page
  RJ_API RedjetSealStatus verify_page(void* code_ptr);

  // 4. Background Full Scan (called periodically or during GC pauses)
  RJ_API size_t verify_all_pages();

  // Telemetry
  uint64_t pages_registered()  const noexcept { return _total_pages_registered.load(std::memory_order_relaxed); }
  uint64_t verifications()     const noexcept { return _total_verifications.load(std::memory_order_relaxed); }
  uint64_t bitflips_trapped()  const noexcept { return _total_bitflips_trapped.load(std::memory_order_relaxed); }
  uint64_t scan_cycles()       const noexcept { return _total_scan_cycles.load(std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_REDJET_VFS_REDJETCODESEALTRAP_HPP
