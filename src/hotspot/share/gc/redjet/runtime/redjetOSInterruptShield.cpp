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

#include "gc/redjet/runtime/redjetOSInterruptShield.hpp"
#include "gc/redjet/engine/redjetSeasonalCoordinator.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "gc/redjet/runtime/redjetLog.hpp"
#include "runtime/os.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

// Static variable definitions
alignas(64) uintptr_t         RedjetOSInterruptShield::_falline_morgue_base{ 0 };
alignas(64) size_t            RedjetOSInterruptShield::_falline_morgue_size{ 0 };
alignas(64) std::atomic<bool> RedjetOSInterruptShield::_shield_active{ false };
alignas(64) std::atomic<bool> RedjetOSInterruptShield::_shutdown_in_progress{ false };
RedjetShieldMetrics           RedjetOSInterruptShield::_metrics;

RedjetShieldMemoryRegion      RedjetOSInterruptShield::_registered_regions[RedjetOSInterruptShield::MAX_SHIELD_REGIONS];
std::atomic<size_t>           RedjetOSInterruptShield::_region_count{ 0 };

alignas(64) std::atomic<bool> RedjetOSInterruptShield::_watchdog_running{ false };
void*                         RedjetOSInterruptShield::_watchdog_thread_handle{ nullptr };
void*                         RedjetOSInterruptShield::_watchdog_timer_handle{ nullptr };
uint32_t                      RedjetOSInterruptShield::_watchdog_interval_ms{ 5 };

#if defined(_WIN32)
void* RedjetOSInterruptShield::_veh_handle{ nullptr };

// ============================================================================
// Windows Vectored Exception Handler (VEH)
// Priority 1: Catches memory access violations BEFORE structured exception handling
// ============================================================================
static LONG WINAPI RedjetVectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
  if (pExceptionInfo == nullptr || pExceptionInfo->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION) {
    // ExceptionInformation[0]: 0 = read, 1 = write, 8 = user-mode DEP violation
    // ExceptionInformation[1]: Faulting virtual address
    uintptr_t fault_addr = static_cast<uintptr_t>(pExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
    
    if (RedjetOSInterruptShield::intercept_hardware_trap(static_cast<int>(code),
                                                         reinterpret_cast<void*>(fault_addr),
                                                         pExceptionInfo->ExceptionRecord,
                                                         pExceptionInfo->ContextRecord)) {
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

// Windows Console Control Handler for Emergency Clean Shutdown
static BOOL WINAPI RedjetConsoleCtrlHandler(DWORD dwCtrlType) {
  switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      redjet_hotlog("[SHIELD] Received Windows Console Shutdown Signal: %lu", dwCtrlType);
      RedjetOSInterruptShield::execute_clean_shutdown_1_5ms();
      return FALSE; // Allow default chain to terminate gracefully
    default:
      return FALSE;
  }
}
#else
// Linux / POSIX SIGSEGV and Shutdown Signal Handlers
static struct sigaction g_old_sigsegv_action;
static struct sigaction g_old_sigint_action;
static struct sigaction g_old_sigterm_action;

static void RedjetPOSIXSignalHandler(int sig, siginfo_t* info, void* uc) {
  if (sig == SIGSEGV && info != nullptr) {
    void* fault_addr = info->si_addr;
    if (RedjetOSInterruptShield::intercept_hardware_trap(sig, fault_addr, info, uc)) {
      return; // Execution resumes immediately at the faulting instruction
    }
  }

  // Fallback to VM's existing handler
  if (g_old_sigsegv_action.sa_flags & SA_SIGINFO) {
    if (g_old_sigsegv_action.sa_sigaction != nullptr) {
      g_old_sigsegv_action.sa_sigaction(sig, info, uc);
    }
  } else {
    if (g_old_sigsegv_action.sa_handler != nullptr &&
        g_old_sigsegv_action.sa_handler != SIG_DFL &&
        g_old_sigsegv_action.sa_handler != SIG_IGN) {
      g_old_sigsegv_action.sa_handler(sig);
    }
  }
}

static void RedjetPOSIXShutdownHandler(int sig, siginfo_t* info, void* uc) {
  redjet_hotlog("[SHIELD] Received POSIX Shutdown Signal: %d", sig);
  RedjetOSInterruptShield::execute_clean_shutdown_1_5ms();

  // Forward to previous handler if present
  struct sigaction* old_act = (sig == SIGINT) ? &g_old_sigint_action : &g_old_sigterm_action;
  if (old_act->sa_flags & SA_SIGINFO) {
    if (old_act->sa_sigaction != nullptr) {
      old_act->sa_sigaction(sig, info, uc);
    }
  } else {
    if (old_act->sa_handler != nullptr && old_act->sa_handler != SIG_DFL && old_act->sa_handler != SIG_IGN) {
      old_act->sa_handler(sig);
    }
  }
}
#endif

// ============================================================================
// Instruction Disassembly & Trap Context Decoder
// ============================================================================

bool RedjetOSInterruptShield::decode_x86_instruction(const uint8_t* code_ptr, RedjetTrapContext* out_ctx) {
  if (code_ptr == nullptr || out_ctx == nullptr) return false;

  size_t idx = 0;
  bool has_rex = false;
  bool has_vex = false;
  bool has_evex = false;
  uint8_t vex_l = 0; // 0=128-bit, 1=256-bit

  // Decode legacy prefixes (0x66 operand size, 0xF2/0xF3 SIMD, 0x2E/0x36 segment)
  while (idx < 15) {
    uint8_t b = code_ptr[idx];
    if (b == 0x66 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
      idx++;
    } else {
      break;
    }
  }

  // Check REX prefix (0x40..0x4F)
  if (code_ptr[idx] >= 0x40 && code_ptr[idx] <= 0x4F) {
    has_rex = true;
    idx++;
  }

  // Check VEX 2-byte prefix (0xC5) or 3-byte prefix (0xC4)
  if (code_ptr[idx] == 0xC5) {
    has_vex = true;
    vex_l = (code_ptr[idx + 1] >> 2) & 0x01;
    idx += 2;
  } else if (code_ptr[idx] == 0xC4) {
    has_vex = true;
    vex_l = (code_ptr[idx + 2] >> 2) & 0x01;
    idx += 3;
  } else if (code_ptr[idx] == 0x62) {
    // EVEX 4-byte prefix (AVX-512)
    has_evex = true;
    idx += 4;
  }

  uint8_t opcode = code_ptr[idx];

  // Vector load/store opcode identification
  if (has_evex) {
    out_ctx->is_vector_operation = true;
    out_ctx->access_byte_width = 64; // 512-bit vector
  } else if (has_vex) {
    out_ctx->is_vector_operation = true;
    out_ctx->access_byte_width = (vex_l == 1) ? 32 : 16;
  } else if (opcode == 0x0F) {
    // 2-byte opcode: e.g. 0x0F 0x10 (MOVUPS), 0x0F 0x11 (MOVUPS store), 0x0F 0x28 (MOVAPS)
    uint8_t op2 = code_ptr[idx + 1];
    if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || op2 == 0x6F || op2 == 0x7F) {
      out_ctx->is_vector_operation = true;
      out_ctx->access_byte_width = 16; // 128-bit SSE
      if (op2 == 0x11 || op2 == 0x29 || op2 == 0x7F) {
        out_ctx->is_write = true;
      }
    }
  } else {
    // Standard scalar access: MOV [addr], reg or MOV reg, [addr]
    out_ctx->is_vector_operation = false;
    out_ctx->access_byte_width = has_rex ? 8 : 4;
  }

  out_ctx->instruction_length = static_cast<uint32_t>(idx + 2); // Approximate length
  return true;
}

bool RedjetOSInterruptShield::decode_aarch64_instruction(uint32_t insn, RedjetTrapContext* out_ctx) {
  if (out_ctx == nullptr) return false;
  out_ctx->arch = RedjetTrapArch::ARCH_AARCH64;

  // Bits [29:28]: size (00=byte, 01=half, 10=word, 11=doubleword)
  // Bits [26]: vector (1=SIMD vector load/store)
  bool is_vector = ((insn >> 26) & 0x01) != 0;
  bool is_store  = ((insn >> 22) & 0x01) == 0;

  out_ctx->is_vector_operation = is_vector;
  out_ctx->is_write = is_store;
  out_ctx->access_byte_width = is_vector ? 16 : 8;
  out_ctx->instruction_length = 4;
  return true;
}

bool RedjetOSInterruptShield::decode_trap_context(void* exception_record, void* context_record, RedjetTrapContext* out_ctx) {
  if (out_ctx == nullptr) return false;
  out_ctx->clear();

#if defined(_WIN32)
  PEXCEPTION_RECORD er = reinterpret_cast<PEXCEPTION_RECORD>(exception_record);
  PCONTEXT cr = reinterpret_cast<PCONTEXT>(context_record);

  if (er != nullptr) {
    out_ctx->fault_address = static_cast<uintptr_t>(er->ExceptionInformation[1]);
    out_ctx->is_write = (er->ExceptionInformation[0] == 1);
  }

  if (cr != nullptr) {
#if defined(_M_AMD64)
    out_ctx->instruction_pointer = cr->Rip;
    out_ctx->stack_pointer = cr->Rsp;
    out_ctx->fault_flags = cr->EFlags;
    out_ctx->arch = RedjetTrapArch::ARCH_X86_64;

    out_ctx->gpr[0] = cr->Rax;
    out_ctx->gpr[1] = cr->Rcx;
    out_ctx->gpr[2] = cr->Rdx;
    out_ctx->gpr[3] = cr->Rbx;
    out_ctx->gpr[4] = cr->Rsp;
    out_ctx->gpr[5] = cr->Rbp;
    out_ctx->gpr[6] = cr->Rsi;
    out_ctx->gpr[7] = cr->Rdi;
    out_ctx->gpr[8] = cr->R8;
    out_ctx->gpr[9] = cr->R9;
    out_ctx->gpr[10] = cr->R10;
    out_ctx->gpr[11] = cr->R11;
    out_ctx->gpr[12] = cr->R12;
    out_ctx->gpr[13] = cr->R13;
    out_ctx->gpr[14] = cr->R14;
    out_ctx->gpr[15] = cr->R15;

    // Decode opcode at RIP
    if (cr->Rip != 0) {
      decode_x86_instruction(reinterpret_cast<const uint8_t*>(cr->Rip), out_ctx);
    }
#elif defined(_M_ARM64)
    out_ctx->instruction_pointer = cr->Pc;
    out_ctx->stack_pointer = cr->Sp;
    out_ctx->arch = RedjetTrapArch::ARCH_AARCH64;
    if (cr->Pc != 0) {
      uint32_t insn = *reinterpret_cast<const uint32_t*>(cr->Pc);
      decode_aarch64_instruction(insn, out_ctx);
    }
#endif
  }
#else
  siginfo_t* si = reinterpret_cast<siginfo_t*>(exception_record);
  ucontext_t* uc = reinterpret_cast<ucontext_t*>(context_record);

  if (si != nullptr) {
    out_ctx->fault_address = reinterpret_cast<uintptr_t>(si->si_addr);
  }

  if (uc != nullptr) {
#if defined(__x86_64__)
    out_ctx->instruction_pointer = uc->uc_mcontext.gregs[REG_RIP];
    out_ctx->stack_pointer = uc->uc_mcontext.gregs[REG_RSP];
    out_ctx->arch = RedjetTrapArch::ARCH_X86_64;
    if (out_ctx->instruction_pointer != 0) {
      decode_x86_instruction(reinterpret_cast<const uint8_t*>(out_ctx->instruction_pointer), out_ctx);
    }
#elif defined(__aarch64__)
    out_ctx->instruction_pointer = uc->uc_mcontext.pc;
    out_ctx->stack_pointer = uc->uc_mcontext.sp;
    out_ctx->arch = RedjetTrapArch::ARCH_AARCH64;
    if (out_ctx->instruction_pointer != 0) {
      uint32_t insn = *reinterpret_cast<const uint32_t*>(out_ctx->instruction_pointer);
      decode_aarch64_instruction(insn, out_ctx);
    }
#endif
  }
#endif

  // Check multi-page vector span: (fault_addr & ~0xFFFULL) != ((fault_addr + access_byte_width - 1) & ~0xFFFULL)
  if (out_ctx->fault_address != 0 && out_ctx->access_byte_width > 1) {
    uintptr_t page_start = out_ctx->fault_address & ~static_cast<uintptr_t>(0xFFFULL);
    uintptr_t page_end   = (out_ctx->fault_address + out_ctx->access_byte_width - 1) & ~static_cast<uintptr_t>(0xFFFULL);
    if (page_start != page_end) {
      out_ctx->spans_page_boundary = true;
    }
  }

  return true;
}

// ============================================================================
// RedjetAtomicSteelDomeGuard Implementation
// ============================================================================

RedjetAtomicSteelDomeGuard::RedjetAtomicSteelDomeGuard()
  : _entry_tsc(sacculus_rdtsc()), _active(false) {
#if defined(_WIN32)
  _thread_handle = GetCurrentThread();
  _previous_priority = GetThreadPriority(_thread_handle);
  // Elevate to TIME_CRITICAL to suppress preemption during hot handoff
  if (SetThreadPriority(_thread_handle, THREAD_PRIORITY_TIME_CRITICAL)) {
    _active = true;
    RedjetOSInterruptShield::metrics().steel_dome_shield_activations.fetch_add(1, std::memory_order_relaxed);
  }
#else
  sigset_t block_set, old_set;
  sigfillset(&block_set);
  // Block all non-fatal signals during R2R critical window
  if (pthread_sigmask(SIG_BLOCK, &block_set, &old_set) == 0) {
    _previous_sigmask = *reinterpret_cast<uint64_t*>(&old_set);
    _active = true;
    RedjetOSInterruptShield::metrics().steel_dome_shield_activations.fetch_add(1, std::memory_order_relaxed);
  }
#endif
}

RedjetAtomicSteelDomeGuard::~RedjetAtomicSteelDomeGuard() {
  if (_active) {
#if defined(_WIN32)
    SetThreadPriority(_thread_handle, _previous_priority);
#else
    sigset_t old_set;
    *reinterpret_cast<uint64_t*>(&old_set) = _previous_sigmask;
    pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
#endif
    uint64_t elapsed = sacculus_rdtsc() - _entry_tsc;
    RedjetOSInterruptShield::metrics().steel_dome_cycles_protected.fetch_add(elapsed, std::memory_order_relaxed);
  }
}

// ============================================================================
// RedjetOSInterruptShield Core Methods
// ============================================================================

void RedjetOSInterruptShield::initialize() {
  if (_shield_active.exchange(true)) return;

  _falline_morgue_base = 0;
  _falline_morgue_size = 0;
  _shutdown_in_progress.store(false, std::memory_order_release);
  _region_count.store(0, std::memory_order_relaxed);

#if defined(_WIN32)
  // Register Vectored Exception Handler as first priority
  _veh_handle = AddVectoredExceptionHandler(1, RedjetVectoredExceptionHandler);
  SetConsoleCtrlHandler(RedjetConsoleCtrlHandler, TRUE);
#else
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = RedjetPOSIXSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, &g_old_sigsegv_action);

  struct sigaction sa_shut;
  std::memset(&sa_shut, 0, sizeof(sa_shut));
  sa_shut.sa_sigaction = RedjetPOSIXShutdownHandler;
  sa_shut.sa_flags = SA_SIGINFO;
  sigaction(SIGINT, &sa_shut, &g_old_sigint_action);
  sigaction(SIGTERM, &sa_shut, &g_old_sigterm_action);
#endif

  _watchdog_running.store(true, std::memory_order_release);
  redjet_hotlog("[SHIELD] Redjet OS & Hardware Interrupt Shield Initialized (4-Tier Armor Active)");
}

void RedjetOSInterruptShield::shutdown() {
  if (!_shield_active.exchange(false)) return;

  _watchdog_running.store(false, std::memory_order_release);

#if defined(_WIN32)
  if (_veh_handle != nullptr) {
    RemoveVectoredExceptionHandler(_veh_handle);
    _veh_handle = nullptr;
  }
#else
  sigaction(SIGSEGV, &g_old_sigsegv_action, nullptr);
  sigaction(SIGINT, &g_old_sigint_action, nullptr);
  sigaction(SIGTERM, &g_old_sigterm_action, nullptr);
#endif
}

void RedjetOSInterruptShield::register_falline_morgue_range(uintptr_t base, size_t size) {
  _falline_morgue_base = base;
  _falline_morgue_size = size;
  register_memory_region(base, size, "falline_morgue");
  redjet_hotlog("[SHIELD] Falline morgue range registered: [%p, %p) (" SIZE_FORMAT " bytes)",
                reinterpret_cast<void*>(base),
                reinterpret_cast<void*>(base + size),
                size);
}

bool RedjetOSInterruptShield::register_memory_region(uintptr_t base, size_t size, const char* name) {
  size_t idx = _region_count.load(std::memory_order_relaxed);
  if (idx >= MAX_SHIELD_REGIONS) return false;

  _registered_regions[idx].base_address = base;
  _registered_regions[idx].size_bytes = size;
  _registered_regions[idx].region_name = (name != nullptr) ? name : "unnamed_vfs_region";
  _registered_regions[idx].is_decommitted = false;
  _region_count.store(idx + 1, std::memory_order_release);
  return true;
}

bool RedjetOSInterruptShield::intercept_hardware_trap(int sig_or_exception_code, void* fault_addr, void* info, void* uc) {
  if (!_shield_active.load(std::memory_order_relaxed)) return false;

  uintptr_t addr = reinterpret_cast<uintptr_t>(fault_addr);
  _metrics.zero_cost_traps_intercepted.fetch_add(1, std::memory_order_relaxed);

  // Check if fault address falls inside Falline morgue range
  if (addr >= _falline_morgue_base && addr < (_falline_morgue_base + _falline_morgue_size)) {
    RedjetTrapContext ctx;
    decode_trap_context(info, uc, &ctx);

    if (ctx.spans_page_boundary) {
      _metrics.vector_multipage_spans_handled.fetch_add(1, std::memory_order_relaxed);
    }

    // Revive object on-the-fly in 1 cycle
    bool revived = RedjetSeasonalCoordinator::notify_quarantine_access(addr);
    if (revived) {
      _metrics.falline_revivals_in_flight.fetch_add(1, std::memory_order_relaxed);
      // Execution resumes immediately at the faulting instruction
      return true;
    }
  }

  // Not a Falline morgue trap: pass through to VM's existing handler
  _metrics.non_falline_signals_passed_to_vm.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void RedjetOSInterruptShield::watchdog_heartbeat_tick() {
  _metrics.watchdog_heartbeats_fired.fetch_add(1, std::memory_order_relaxed);

  // Poll 2-Points flags and VFS node health
  // When a thread remains parked past threshold, issue alert
  uint64_t current_tsc = sacculus_rdtsc();
  // Seasonal Coordinator tick integration
  RedjetSeasonalCoordinator::tick(current_tsc, 100, 0.99);
}

void RedjetOSInterruptShield::execute_clean_shutdown_1_5ms() {
  if (_shutdown_in_progress.exchange(true)) return;

  uint64_t start_tsc = sacculus_rdtsc();
  _metrics.clean_shutdown_invocations.fetch_add(1, std::memory_order_relaxed);

  size_t regions = _region_count.load(std::memory_order_relaxed);
  size_t total_bytes_decommitted = 0;
  size_t total_pages = 0;

  for (size_t i = 0; i < regions; ++i) {
    RedjetShieldMemoryRegion& r = _registered_regions[i];
    if (r.base_address != 0 && r.size_bytes > 0 && !r.is_decommitted) {
#if defined(_WIN32)
      // Rapid VirtualFree decommit
      BOOL ok = VirtualFree(reinterpret_cast<void*>(r.base_address), r.size_bytes, MEM_DECOMMIT);
      if (ok) {
        r.is_decommitted = true;
        total_bytes_decommitted += r.size_bytes;
        total_pages += (r.size_bytes / 4096);
      }
#else
      // Rapid madvise DONTNEED
      int res = madvise(reinterpret_cast<void*>(r.base_address), r.size_bytes, MADV_DONTNEED);
      if (res == 0) {
        r.is_decommitted = true;
        total_bytes_decommitted += r.size_bytes;
        total_pages += (r.size_bytes / 4096);
      }
#endif
    }
  }

  uint64_t elapsed_tsc = sacculus_rdtsc() - start_tsc;
  // Approximate microseconds at 3.0 GHz
  uint64_t duration_us = elapsed_tsc / 3000;

  _metrics.clean_shutdown_pages_decommitted.fetch_add(total_pages, std::memory_order_relaxed);
  _metrics.clean_shutdown_bytes_reclaimed.fetch_add(total_bytes_decommitted, std::memory_order_relaxed);
  _metrics.clean_shutdown_duration_us.store(duration_us, std::memory_order_relaxed);

  redjet_hotlog("[SHIELD] Clean Shutdown Complete: " SIZE_FORMAT " pages (" SIZE_FORMAT " bytes) decommitted in " UINT64_FORMAT " us",
                total_pages, total_bytes_decommitted, duration_us);
}

void RedjetOSInterruptShield::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("=== Redjet OS & Hardware Interrupt Shield (4-Tier Silicon Armor) ===");
  st->print_cr("  Shield Status: %s", _shield_active.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE");
  st->print_cr("  Shutdown In Progress: %s", _shutdown_in_progress.load(std::memory_order_relaxed) ? "TRUE" : "FALSE");
  st->print_cr("  Falline Morgue Base: %p, Size: " SIZE_FORMAT " bytes",
               reinterpret_cast<void*>(_falline_morgue_base), _falline_morgue_size);
  st->print_cr("  Registered VFS Regions: " SIZE_FORMAT, _region_count.load(std::memory_order_relaxed));
  st->print_cr("  Tier 1 - Zero-Cost Traps Intercepted: " UINT64_FORMAT, _metrics.zero_cost_traps_intercepted.load(std::memory_order_relaxed));
  st->print_cr("  Tier 1 - Falline Revivals In-Flight: " UINT64_FORMAT, _metrics.falline_revivals_in_flight.load(std::memory_order_relaxed));
  st->print_cr("  Tier 1 - Multi-Page Vector Spans Handled: " UINT64_FORMAT, _metrics.vector_multipage_spans_handled.load(std::memory_order_relaxed));
  st->print_cr("  Tier 1 - Non-Falline Signals Passed to VM: " UINT64_FORMAT, _metrics.non_falline_signals_passed_to_vm.load(std::memory_order_relaxed));
  st->print_cr("  Tier 2 - Steel Dome Shield Activations: " UINT64_FORMAT, _metrics.steel_dome_shield_activations.load(std::memory_order_relaxed));
  st->print_cr("  Tier 2 - Steel Dome Cycles Protected: " UINT64_FORMAT, _metrics.steel_dome_cycles_protected.load(std::memory_order_relaxed));
  st->print_cr("  Tier 3 - Watchdog Heartbeats Fired: " UINT64_FORMAT, _metrics.watchdog_heartbeats_fired.load(std::memory_order_relaxed));
  st->print_cr("  Tier 3 - Watchdog Threads Parked: " UINT64_FORMAT, _metrics.watchdog_threads_parked.load(std::memory_order_relaxed));
  st->print_cr("  Tier 3 - Watchdog Hung Thread Alarms: " UINT64_FORMAT, _metrics.watchdog_hung_thread_alarms.load(std::memory_order_relaxed));
  st->print_cr("  Tier 4 - Clean Shutdown Invocations: " UINT64_FORMAT, _metrics.clean_shutdown_invocations.load(std::memory_order_relaxed));
  st->print_cr("  Tier 4 - Clean Shutdown Pages Decommitted: " UINT64_FORMAT, _metrics.clean_shutdown_pages_decommitted.load(std::memory_order_relaxed));
  st->print_cr("  Tier 4 - Clean Shutdown Bytes Reclaimed: " UINT64_FORMAT, _metrics.clean_shutdown_bytes_reclaimed.load(std::memory_order_relaxed));
  st->print_cr("  Tier 4 - Clean Shutdown Duration: " UINT64_FORMAT " us", _metrics.clean_shutdown_duration_us.load(std::memory_order_relaxed));
}
