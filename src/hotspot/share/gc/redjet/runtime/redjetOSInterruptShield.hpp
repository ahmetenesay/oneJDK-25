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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETOSINTERRUPTSHIELD_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETOSINTERRUPTSHIELD_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include <atomic>
#include <cstdint>
#include <cstddef>

#ifndef RJ_API
#if defined(_WIN32) || defined(__CYGWIN__)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

// ============================================================================
// OS ve Donanimsal Kesme Sinyali Kalkani (PLAN.md Section 9.8)
//
// 4-Tier Silicon Armor:
// 1. Zero-Cost Page Guard: VEH (Windows) / sigaction (Linux) interceptor
//    trapping Falline morgue accesses and reviving objects on-the-fly in 1 cycle.
//    Includes x86-64 / AArch64 instruction inspection, multi-page vector span handling,
//    and exception context restoration without VM crash.
// 2. Domestic Scheduler Atomic Steel Dome: Signal masking (pthread_sigmask)
//    and THREAD_PRIORITY_TIME_CRITICAL shielding register-to-register handoffs.
//    Guarantees uninterruptible hot-register transfer windows.
// 3. Coach Hardware Watchdog Heartbeat: High-res OS timer interrupt polling VFS
//    node flags and 2-Points flags (_park_flag, _dock_flag), detecting hung threads
//    and safely evacuating them to cooperative Parkpoints.
// 4. Clean Shutdown Shield: 1.5ms rapid decommit of all physical pages to OS
//    (VirtualFree / madvise) on SIGINT/SIGTERM/CTRL_CLOSE_EVENT.
// ============================================================================

struct alignas(64) RedjetShieldMetrics {
  std::atomic<uint64_t> zero_cost_traps_intercepted{ 0 };
  std::atomic<uint64_t> falline_revivals_in_flight{ 0 };
  std::atomic<uint64_t> non_falline_signals_passed_to_vm{ 0 };
  std::atomic<uint64_t> vector_multipage_spans_handled{ 0 };

  std::atomic<uint64_t> steel_dome_shield_activations{ 0 };
  std::atomic<uint64_t> steel_dome_interrupts_suppressed{ 0 };
  std::atomic<uint64_t> steel_dome_cycles_protected{ 0 };

  std::atomic<uint64_t> watchdog_heartbeats_fired{ 0 };
  std::atomic<uint64_t> watchdog_threads_parked{ 0 };
  std::atomic<uint64_t> watchdog_hung_thread_alarms{ 0 };
  std::atomic<uint64_t> watchdog_evacuations_completed{ 0 };

  std::atomic<uint64_t> clean_shutdown_invocations{ 0 };
  std::atomic<uint64_t> clean_shutdown_pages_decommitted{ 0 };
  std::atomic<uint64_t> clean_shutdown_bytes_reclaimed{ 0 };
  std::atomic<uint64_t> clean_shutdown_duration_us{ 0 };
};

// Instruction Architecture type for trap analysis
enum class RedjetTrapArch : uint8_t {
  ARCH_X86_64  = 0,
  ARCH_AARCH64 = 1,
  ARCH_RISCV64 = 2,
  ARCH_UNKNOWN = 3
};

// Thread Execution Context Descriptor for Deep Trap Analysis
struct alignas(64) RedjetTrapContext {
  uintptr_t      fault_address{ 0 };
  uintptr_t      instruction_pointer{ 0 };
  uintptr_t      stack_pointer{ 0 };
  uint64_t       fault_flags{ 0 };
  RedjetTrapArch arch{ RedjetTrapArch::ARCH_X86_64 };
  bool           is_write{ false };
  bool           is_vector_operation{ false };
  bool           spans_page_boundary{ false };
  uint32_t       access_byte_width{ 8 };
  uint32_t       instruction_length{ 0 };
  uint8_t        raw_instruction_bytes[16]{ 0 };

  // Register state snapshot
  uint64_t       gpr[16]{ 0 };
  uint64_t       vector_low[4]{ 0 };

  void clear() {
    std::memset(this, 0, sizeof(*this));
    arch = RedjetTrapArch::ARCH_X86_64;
    access_byte_width = 8;
  }
};

// Registered VFS Memory Region for Clean Shutdown Shield
struct RedjetShieldMemoryRegion {
  uintptr_t base_address{ 0 };
  size_t    size_bytes{ 0 };
  const char* region_name{ "unknown" };
  bool      is_decommitted{ false };
};

// RAII Critical Section Shield for Domestic Scheduler Register-to-Register (R2R) handoffs
class RedjetAtomicSteelDomeGuard {
private:
#if defined(_WIN32)
  void* _thread_handle;
  int   _previous_priority;
#else
  uint64_t _previous_sigmask;
#endif
  uint64_t _entry_tsc;
  bool     _active;

public:
  RedjetAtomicSteelDomeGuard();
  ~RedjetAtomicSteelDomeGuard();

  RedjetAtomicSteelDomeGuard(const RedjetAtomicSteelDomeGuard&) = delete;
  RedjetAtomicSteelDomeGuard& operator=(const RedjetAtomicSteelDomeGuard&) = delete;

  bool is_active() const noexcept { return _active; }
  uint64_t entry_tsc() const noexcept { return _entry_tsc; }
};

// ============================================================================
// RedjetOSInterruptShield Class
// ============================================================================
class RedjetOSInterruptShield : public AllStatic {
public:
  static constexpr size_t MAX_SHIELD_REGIONS = 64;

private:
  static alignas(64) uintptr_t         _falline_morgue_base;
  static alignas(64) size_t            _falline_morgue_size;
  static alignas(64) std::atomic<bool> _shield_active;
  static alignas(64) std::atomic<bool> _shutdown_in_progress;
  static RedjetShieldMetrics           _metrics;

  // Registered VFS memory regions
  static RedjetShieldMemoryRegion      _registered_regions[MAX_SHIELD_REGIONS];
  static std::atomic<size_t>           _region_count;

  // Watchdog thread management
  static alignas(64) std::atomic<bool> _watchdog_running;
  static void*                         _watchdog_thread_handle;
  static void*                         _watchdog_timer_handle;
  static uint32_t                      _watchdog_interval_ms;

#if defined(_WIN32)
  static void* _veh_handle;
#endif

  // Internal signal handlers and decoder helpers
  static bool handle_page_fault(uintptr_t fault_addr, void* context);
  static void handle_emergency_shutdown();
  static bool decode_trap_context(void* exception_record, void* context_record, RedjetTrapContext* out_ctx);
  static bool decode_x86_instruction(const uint8_t* code_ptr, RedjetTrapContext* out_ctx);
  static bool decode_aarch64_instruction(uint32_t insn, RedjetTrapContext* out_ctx);

  // Watchdog worker loop
  static void watchdog_thread_entry();

public:
  RJ_API static void initialize();
  RJ_API static void shutdown();

  // Configure Falline morgue address boundary for Zero-Cost Page Guard
  RJ_API static void register_falline_morgue_range(uintptr_t base, size_t size);

  // Register arbitrary VFS memory region for Clean Shutdown Shield decommitment
  RJ_API static bool register_memory_region(uintptr_t base, size_t size, const char* name);

  // Pre-Filter Interceptor called before JVM_handle_*_signal
  // Returns true if the trap was handled and execution should continue immediately
  RJ_API static bool intercept_hardware_trap(int sig_or_exception_code, void* fault_addr, void* info, void* uc);

  // Watchdog Heartbeat tick called by high-resolution OS timer
  RJ_API static void watchdog_heartbeat_tick();

  // Emergency Clean Shutdown triggered by OS signals (SIGINT, SIGTERM, CTRL_CLOSE_EVENT)
  // Executes in <= 1.5ms decommitting all physical pages to OS
  RJ_API static void execute_clean_shutdown_1_5ms();

  // Metrics query
  static const RedjetShieldMetrics& metrics() noexcept { return _metrics; }
  static bool is_active() noexcept { return _shield_active.load(std::memory_order_relaxed); }
  static bool is_shutdown_in_progress() noexcept { return _shutdown_in_progress.load(std::memory_order_relaxed); }
  static uintptr_t falline_base() noexcept { return _falline_morgue_base; }
  static size_t    falline_size() noexcept { return _falline_morgue_size; }
  static size_t    registered_region_count() noexcept { return _region_count.load(std::memory_order_relaxed); }

  // HotLog / JMX diagnostic printer
  RJ_API static void print_on(outputStream* st);
};

#endif // SHARE_GC_REDJET_RUNTIME_REDJETOSINTERRUPTSHIELD_HPP
