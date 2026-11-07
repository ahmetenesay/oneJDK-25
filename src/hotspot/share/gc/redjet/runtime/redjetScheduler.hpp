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

#ifndef SHARE_GC_REDJET_RUNTIME_REDJETSCHEDULER_HPP
#define SHARE_GC_REDJET_RUNTIME_REDJETSCHEDULER_HPP

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
// Redjet Scheduler Architecture: Domestic vs. TBB (PLAN.md Section 8.8.1)
//
// STRICT MUTUAL EXCLUSION ENFORCED:
// - Domestic Scheduler: Deterministic in-order silicon pipeline (DSP), DutySetSetup,
//   Duty Fusion & Deduplication, duty_precedence (0-lock in-order execution),
//   Chained Threads with Zero-Store Hot Handoff, 64-byte false sharing shield,
//   static silicon arenas.
// - TBB Scheduler: Intel oneTBB heterogeneous task_arena work-stealing.
// - Mutual Exclusion: When Domestic is active, TBB is 100% silenced/suspended.
//   CLI Flag: -XX:RedjetGCScheduler=<Domestic|TBB> (default: Domestic).
// ============================================================================

enum class RedjetSchedulerMode : uint8_t {
  DOMESTIC = 0, // Deterministic in-order silicon pipeline (Default & Recommended)
  TBB      = 1  // Intel oneTBB work-stealing task arenas (Legacy compatibility)
};

// Low-level Duty types for uOps & L1-D cache planning
enum class RedjetDutyType : uint8_t {
  DUTY_CARD_SCAN       = 0,
  DUTY_BITMAP_MARK     = 1,
  DUTY_OBJECT_TRACE    = 2,
  DUTY_EMPTYBOX_PURGE  = 3,
  DUTY_RMRP_RECLAIM    = 4,
  DUTY_STRING_DEDUP    = 5,
  DUTY_ROOT_SCAN       = 6,
  DUTY_FUSED_SUPERDUTY = 7
};

// uOp opcodes for DutySetSetup micro-architecture simulation
enum class RedjetUOpCode : uint16_t {
  UOP_NOP              = 0x00,
  UOP_LOAD_CARDBYTE    = 0x01,
  UOP_TEST_DIRTY_MASK  = 0x02,
  UOP_BITMAP_BITSCAN   = 0x03,
  UOP_SIMD_BROADCAST   = 0x04,
  UOP_VECTOR_CMP_MASK  = 0x05,
  UOP_PREFETCH_L1      = 0x06,
  UOP_PREFETCH_NTA     = 0x07,
  UOP_HOT_FORWARD_REG  = 0x08,
  UOP_EMPTYBOX_CLAIM   = 0x09,
  UOP_BARRIER_FENCE_64 = 0x0A,
  UOP_ATOMIC_INC_64    = 0x0B,
  UOP_VECTOR_STREAM_STORE = 0x0C,
  UOP_BRANCH_COND      = 0x0D,
  UOP_CYCLE_PAUSE      = 0x0E
};

// Execution Port bitmasks simulating modern superscalar cores (Ports 0..7)
enum RedjetExecutionPort : uint32_t {
  PORT_0_ALU_VEC   = 0x01, // ALU / Vector FMA
  PORT_1_ALU_VEC   = 0x02, // ALU / Vector FMA
  PORT_2_LOAD      = 0x04, // Load AGU
  PORT_3_LOAD      = 0x08, // Load AGU
  PORT_4_STORE_DATA= 0x10, // Store Data
  PORT_5_ALU_VEC   = 0x20, // ALU / Vector Shuffle
  PORT_6_ALU_BRANCH= 0x40, // ALU / Branch
  PORT_7_STORE_ADDR= 0x80  // Store Address AGU
};

// Hardware Register Bank for Zero-Store Hot Handoff emulation (64-byte aligned)
struct alignas(64) RedjetHotHandoffRegisterBank {
  uint64_t gpr[8];             // R0..R7 (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI)
  uint64_t zmm_low[4][8];      // 4 x 512-bit ZMM vector registers (ZMM0..ZMM3)
  uint64_t predicate_k[2];     // 2 x 64-bit AVX-512 mask registers (k0, k1)
  uint64_t flags_register;     // Status & overflow flags
  uint64_t cycle_timestamp;    // TSC timestamp of last register update
  uint32_t active_register_mask; // Bitmask of valid registers

  RedjetHotHandoffRegisterBank() {
    clear();
  }

  void clear() {
    std::memset(this, 0, sizeof(*this));
    active_register_mask = 0;
  }

  void set_gpr(uint32_t reg_idx, uint64_t val) {
    if (reg_idx < 8) {
      gpr[reg_idx] = val;
      active_register_mask |= (1U << reg_idx);
    }
  }

  uint64_t get_gpr(uint32_t reg_idx) const {
    return (reg_idx < 8) ? gpr[reg_idx] : 0;
  }

  void set_simd_qword(uint32_t zmm_idx, uint32_t qword_lane, uint64_t val) {
    if (zmm_idx < 4 && qword_lane < 8) {
      zmm_low[zmm_idx][qword_lane] = val;
      active_register_mask |= (1U << (8 + zmm_idx));
    }
  }

  uint64_t get_simd_qword(uint32_t zmm_idx, uint32_t qword_lane) const {
    return (zmm_idx < 4 && qword_lane < 8) ? zmm_low[zmm_idx][qword_lane] : 0;
  }
};

// uOps-level task design & L1-D cache planning descriptor (DutySetSetup)
struct alignas(64) DutySetSetup {
  uint32_t       duty_id{ 0 };
  RedjetDutyType duty_type{ RedjetDutyType::DUTY_CARD_SCAN };
  uintptr_t      cacheline_addr{ 0 };
  size_t         byte_stride{ 64 };
  uint32_t       register_mask{ 0 };
  uint32_t       target_core{ UINT32_MAX };
  RedjetUOpCode  uop_opcode{ RedjetUOpCode::UOP_LOAD_CARDBYTE };
  uint16_t       src_reg_0{ 0 }; // e.g. 0=RAX, 1=RDX, 2=ZMM0
  uint16_t       src_reg_1{ 0 };
  uint16_t       dst_reg{ 0 };
  uint64_t       immediate_val{ 0 };
  uint32_t       port_binding_mask{ PORT_0_ALU_VEC | PORT_1_ALU_VEC };
  uint16_t       l1_latency_cycles{ 1 };
  uint16_t       issue_width{ 4 };
  void (*func)(void* ctx, void* hot_reg_data){ nullptr };
  void*          context{ nullptr };
  uint64_t       setup_tsc{ 0 };

  DutySetSetup() = default;

  void configure(uint32_t id, RedjetDutyType type, uintptr_t cl_addr,
                 RedjetUOpCode uop, uint32_t ports, uint16_t latency) {
    duty_id = id;
    duty_type = type;
    cacheline_addr = cl_addr;
    uop_opcode = uop;
    port_binding_mask = ports;
    l1_latency_cycles = latency;
    setup_tsc = sacculus_rdtsc();
  }
};

// Concrete Duty descriptor for Domestic Scheduler
struct alignas(64) RedjetDuty {
  uint32_t       id{ 0 };
  RedjetDutyType type{ RedjetDutyType::DUTY_CARD_SCAN };
  uintptr_t      cacheline_index{ 0 };
  void*          addr_start{ nullptr };
  size_t         length_bytes{ 0 };
  void (*execute_fn)(void* ctx, void* hot_handoff_data){ nullptr };
  void*          context{ nullptr };
  uint32_t       precedence_order{ 0 };
  bool           is_fused{ false };
  uint32_t       fused_duty_count{ 1 };
  uint32_t       chained_sequence_id{ 0 };
  uint32_t       uop_count{ 4 };
  uint32_t       primary_port_mask{ PORT_0_ALU_VEC | PORT_1_ALU_VEC };
  uint64_t       predicted_cycles{ 1 };

  RedjetDuty() = default;

  void init(uint32_t d_id, RedjetDutyType d_type, void* addr, size_t len,
            void (*fn)(void*, void*), void* ctx, uint32_t precedence) {
    id = d_id;
    type = d_type;
    addr_start = addr;
    cacheline_index = reinterpret_cast<uintptr_t>(addr) >> 6;
    length_bytes = len;
    execute_fn = fn;
    context = ctx;
    precedence_order = precedence;
    is_fused = false;
    fused_duty_count = 1;
    chained_sequence_id = 0;
    uop_count = 4;
    primary_port_mask = PORT_0_ALU_VEC | PORT_1_ALU_VEC;
    predicted_cycles = 1;
  }
};

// Hardware Core Task (Common interface)
struct alignas(64) RedjetHardwareTask {
  std::atomic<void (*)(void*)> func{ nullptr };
  void*    data{ nullptr };
  uint32_t affinity_core{ UINT32_MAX };
  uint32_t priority{ 0 };
  uint64_t enqueue_tsc{ 0 };

  RedjetHardwareTask() = default;
  RedjetHardwareTask(const RedjetHardwareTask& other) {
    func.store(other.func.load(std::memory_order_relaxed), std::memory_order_relaxed);
    data = other.data;
    affinity_core = other.affinity_core;
    priority = other.priority;
    enqueue_tsc = other.enqueue_tsc;
  }
  RedjetHardwareTask& operator=(const RedjetHardwareTask& other) {
    if (this != &other) {
      func.store(other.func.load(std::memory_order_relaxed), std::memory_order_relaxed);
      data = other.data;
      affinity_core = other.affinity_core;
      priority = other.priority;
      enqueue_tsc = other.enqueue_tsc;
    }
    return *this;
  }
};

// Circular ring buffer queue per hardware core
class alignas(64) RedjetPerCoreQueue {
public:
  static constexpr size_t QUEUE_CAPACITY = 512;
  static constexpr size_t QUEUE_MASK     = QUEUE_CAPACITY - 1;

private:
  RedjetHardwareTask                 _tasks[QUEUE_CAPACITY];
  alignas(64) std::atomic<uint64_t>  _head{ 0 };
  alignas(64) std::atomic<uint64_t>  _tail{ 0 };
  hs_ticket_lock_t                   _queue_lock;

  uint32_t                           _core_id{ 0 };
  uint32_t                           _ccd_id{ 0 };
  uint32_t                           _socket_id{ 0 };

  std::atomic<uint64_t>              _tasks_executed{ 0 };
  std::atomic<uint64_t>              _tasks_stolen_out{ 0 };
  std::atomic<uint64_t>              _tasks_stolen_in{ 0 };

public:
  RedjetPerCoreQueue() = default;

  void init(uint32_t core_id, uint32_t ccd_id, uint32_t socket_id);

  bool push_local(void (*func)(void*), void* data, uint32_t priority = 0);
  bool pop_local(RedjetHardwareTask* out_task);
  bool steal_remote(RedjetHardwareTask* out_task);

  size_t   size() const noexcept;
  uint32_t core_id() const noexcept { return _core_id; }
  uint32_t ccd_id() const noexcept { return _ccd_id; }
  uint32_t socket_id() const noexcept { return _socket_id; }

  uint64_t tasks_executed() const noexcept { return _tasks_executed.load(std::memory_order_relaxed); }
  uint64_t tasks_stolen_out() const noexcept { return _tasks_stolen_out.load(std::memory_order_relaxed); }
  uint64_t tasks_stolen_in() const noexcept { return _tasks_stolen_in.load(std::memory_order_relaxed); }
};

// ============================================================================
// Domestic Scheduler: Fixed Static Silicon Arena
// 0% Crash guarantee: Static pre-allocated 64KB arena per worker thread
// Multi-tier alignments: 8B, 16B, 64B, 512B
// ============================================================================
class alignas(64) RedjetStaticSiliconArena {
public:
  static constexpr size_t ARENA_SIZE_PER_THREAD = 64 * 1024; // 64 KB L1/L2 resident scratchpad

private:
  alignas(64) uint8_t _buffer[ARENA_SIZE_PER_THREAD];
  alignas(64) std::atomic<size_t> _offset{ 0 };
  std::atomic<uint64_t>           _total_allocations{ 0 };
  std::atomic<uint64_t>           _exhaustion_events{ 0 };

public:
  RedjetStaticSiliconArena();
  ~RedjetStaticSiliconArena() = default;

  void*  allocate(size_t size, size_t alignment = 64);
  void   reset();
  size_t used_bytes() const noexcept { return _offset.load(std::memory_order_relaxed); }
  size_t capacity() const noexcept { return ARENA_SIZE_PER_THREAD; }
  size_t remaining_bytes() const noexcept {
    size_t u = used_bytes();
    return (u < ARENA_SIZE_PER_THREAD) ? (ARENA_SIZE_PER_THREAD - u) : 0;
  }
  uint64_t total_allocations() const noexcept { return _total_allocations.load(std::memory_order_relaxed); }
  uint64_t exhaustion_events() const noexcept { return _exhaustion_events.load(std::memory_order_relaxed); }
};

// ============================================================================
// Domestic Scheduler: Duty Fusion & Deduplication Engine
// Inspects batch duties: if Card Table Scan and Bitmap Mark touch the same
// 64-byte cache line, fuses them into a single Fused Super-Duty for single L1 hit.
// ============================================================================
class RedjetDutyFusionEngine {
public:
  static constexpr size_t MAX_BATCH_DUTIES = 256;

private:
  RedjetDuty            _batch_duties[MAX_BATCH_DUTIES];
  size_t                _batch_count{ 0 };
  std::atomic<uint64_t> _total_fused_superduties{ 0 };
  std::atomic<uint64_t> _total_cachelines_saved{ 0 };
  std::atomic<uint64_t> _total_duties_inspected{ 0 };
  std::atomic<uint64_t> _total_uops_eliminated{ 0 };
  std::atomic<uint64_t> _total_fusion_cycles_saved{ 0 };

public:
  RedjetDutyFusionEngine() = default;

  void clear();
  bool add_duty(const RedjetDuty& duty);

  // Pre-Flight Inspection: multi-pass cacheline deduplication & duty fusion
  size_t perform_preflight_fusion();

  const RedjetDuty* duties() const noexcept { return _batch_duties; }
  size_t            duty_count() const noexcept { return _batch_count; }

  uint64_t total_fused_superduties()   const noexcept { return _total_fused_superduties.load(std::memory_order_relaxed); }
  uint64_t total_cachelines_saved()     const noexcept { return _total_cachelines_saved.load(std::memory_order_relaxed); }
  uint64_t total_duties_inspected()     const noexcept { return _total_duties_inspected.load(std::memory_order_relaxed); }
  uint64_t total_uops_eliminated()      const noexcept { return _total_uops_eliminated.load(std::memory_order_relaxed); }
  uint64_t total_fusion_cycles_saved()  const noexcept { return _total_fusion_cycles_saved.load(std::memory_order_relaxed); }
};

// ============================================================================
// Domestic Scheduler: Chained Thread (Zero-Store Hot Handoff)
// Chains data-dependent duties in strict order, passing results directly in
// CPU registers / hot scratchpad pointers without memory round-trips.
// ============================================================================
class alignas(64) RedjetChainedThread {
private:
  uint32_t                     _thread_id{ 0 };
  uint32_t                     _pinned_core_id{ 0 };
  std::atomic<bool>            _running{ false };
  void*                        _hot_handoff_register{ nullptr };
  RedjetHotHandoffRegisterBank _register_bank;
  RedjetStaticSiliconArena     _arena;

  std::atomic<uint64_t>        _chained_duties_executed{ 0 };
  std::atomic<uint64_t>        _hot_handoffs_completed{ 0 };
  std::atomic<uint64_t>        _zero_store_forwardings{ 0 };
  std::atomic<uint64_t>        _uops_executed{ 0 };
  std::atomic<uint64_t>        _total_cycles_spent{ 0 };

public:
  RedjetChainedThread() = default;

  void init(uint32_t thread_id, uint32_t pinned_core_id);
  void execute_chain(const RedjetDuty* duties, size_t count);

  uint32_t thread_id() const noexcept { return _thread_id; }
  uint32_t pinned_core_id() const noexcept { return _pinned_core_id; }
  uint64_t chained_duties_executed() const noexcept { return _chained_duties_executed.load(std::memory_order_relaxed); }
  uint64_t hot_handoffs_completed()  const noexcept { return _hot_handoffs_completed.load(std::memory_order_relaxed); }
  uint64_t zero_store_forwardings()  const noexcept { return _zero_store_forwardings.load(std::memory_order_relaxed); }
  uint64_t uops_executed()           const noexcept { return _uops_executed.load(std::memory_order_relaxed); }
  uint64_t total_cycles_spent()      const noexcept { return _total_cycles_spent.load(std::memory_order_relaxed); }
  RedjetStaticSiliconArena& arena()  noexcept { return _arena; }
  RedjetHotHandoffRegisterBank& register_bank() noexcept { return _register_bank; }
};

// ============================================================================
// Domestic Scheduler: Singular Thread (64-byte Isolated Fence)
// Executes independent duties on isolated Runway/Slab blocks with zero false sharing.
// ============================================================================
class alignas(64) RedjetSingularThread {
private:
  uint32_t                 _thread_id{ 0 };
  uint32_t                 _pinned_core_id{ 0 };
  std::atomic<bool>        _running{ false };
  RedjetStaticSiliconArena _arena;
  std::atomic<uint64_t>    _singular_duties_executed{ 0 };
  std::atomic<uint64_t>    _cacheline_fences_enforced{ 0 };
  std::atomic<uint64_t>    _total_cycles_spent{ 0 };

public:
  RedjetSingularThread() = default;

  void init(uint32_t thread_id, uint32_t pinned_core_id);
  void execute_duty(const RedjetDuty& duty);

  uint32_t thread_id() const noexcept { return _thread_id; }
  uint32_t pinned_core_id() const noexcept { return _pinned_core_id; }
  uint64_t singular_duties_executed()  const noexcept { return _singular_duties_executed.load(std::memory_order_relaxed); }
  uint64_t cacheline_fences_enforced() const noexcept { return _cacheline_fences_enforced.load(std::memory_order_relaxed); }
  uint64_t total_cycles_spent()        const noexcept { return _total_cycles_spent.load(std::memory_order_relaxed); }
  RedjetStaticSiliconArena& arena() noexcept { return _arena; }
};

// ============================================================================
// Domestic Scheduler: Pre-Flight Dependency & SMT Sibling Inspector
// ============================================================================
class RedjetPreFlightInspector {
public:
  struct CoreSiblingState {
    uint32_t core_id{ 0 };
    uint32_t sibling_core_id{ UINT32_MAX };
    bool     is_vector_heavy{ false };
    bool     is_sleeping{ false };
    uint32_t consecutive_heavy_epochs{ 0 };
  };

private:
  CoreSiblingState _siblings[256];
  size_t           _tracked_cores{ 0 };
  hs_ticket_lock_t _inspector_lock;

public:
  RedjetPreFlightInspector();

  void init(size_t core_count);
  void mark_vector_heavy(uint32_t core_id, bool heavy);
  bool should_throttle_sibling(uint32_t core_id) const noexcept;
  uint32_t get_optimal_chained_core() const noexcept;
  uint32_t get_optimal_singular_core(uint32_t preferred_numa_node) const noexcept;
};

// ============================================================================
// Redjet Domestic Scheduler (PLAN.md Section 8.8.1 - 100% In-Order DSP Engine)
// ============================================================================
class RedjetDomesticScheduler {
public:
  static constexpr size_t MAX_DOMESTIC_CORES = 256;

private:
  RedjetDutyFusionEngine   _fusion_engine;
  RedjetPreFlightInspector _preflight_inspector;
  RedjetChainedThread      _chained_threads[MAX_DOMESTIC_CORES];
  RedjetSingularThread     _singular_threads[MAX_DOMESTIC_CORES];
  uint32_t                 _core_count{ 1 };
  std::atomic<bool>        _active{ false };

  // Telemetry
  std::atomic<uint64_t>    _total_duties_dispatched{ 0 };
  std::atomic<uint64_t>    _in_order_precedence_checks{ 0 };
  std::atomic<uint64_t>    _zero_lock_handoff_cycles{ 0 };
  std::atomic<uint64_t>    _precedence_violations_prevented{ 0 };
  std::atomic<uint64_t>    _smt_sibling_pauses{ 0 };
  std::atomic<uint64_t>    _total_dsp_pipeline_stalls{ 0 };

public:
  RedjetDomesticScheduler();
  ~RedjetDomesticScheduler();

  RJ_API void init(uint32_t core_count);
  RJ_API void shutdown();

  RJ_API bool dispatch_duty_chain(const RedjetDuty* duties, size_t count, uint32_t core_target);
  RJ_API bool dispatch_singular_duty(const RedjetDuty& duty, uint32_t core_target);

  RedjetDutyFusionEngine&   fusion_engine() noexcept { return _fusion_engine; }
  RedjetPreFlightInspector& preflight_inspector() noexcept { return _preflight_inspector; }
  bool is_active() const noexcept { return _active.load(std::memory_order_relaxed); }

  uint64_t total_duties_dispatched() const noexcept { return _total_duties_dispatched.load(std::memory_order_relaxed); }
  uint64_t in_order_precedence_checks() const noexcept { return _in_order_precedence_checks.load(std::memory_order_relaxed); }
  uint64_t zero_lock_handoff_cycles() const noexcept { return _zero_lock_handoff_cycles.load(std::memory_order_relaxed); }
  uint64_t precedence_violations_prevented() const noexcept { return _precedence_violations_prevented.load(std::memory_order_relaxed); }
  uint64_t smt_sibling_pauses() const noexcept { return _smt_sibling_pauses.load(std::memory_order_relaxed); }
  uint64_t total_dsp_pipeline_stalls() const noexcept { return _total_dsp_pipeline_stalls.load(std::memory_order_relaxed); }

  void record_precedence_violation() noexcept { _precedence_violations_prevented.fetch_add(1, std::memory_order_relaxed); }
  void record_sibling_pause() noexcept { _smt_sibling_pauses.fetch_add(1, std::memory_order_relaxed); }

  void print_on(outputStream* st) const;
};

// ============================================================================
// Master Redjet Bare-Metal Scheduler (Host coordinator & Mutual Exclusion)
// ============================================================================
class RedjetBareMetalScheduler {
public:
  static constexpr size_t MAX_HARDWARE_CORES = 256;

private:
  RedjetPerCoreQueue       _core_queues[MAX_HARDWARE_CORES];
  uint32_t                 _active_cores{ 1 };
  RedjetSchedulerMode      _mode{ RedjetSchedulerMode::DOMESTIC };
  std::atomic<bool>        _running{ false };

  // Domestic Scheduler engine (Primary)
  RedjetDomesticScheduler  _domestic_scheduler;

  // Mutual exclusion flags
  std::atomic<bool>        _domestic_active{ false };
  std::atomic<bool>        _tbb_active{ false };

  std::atomic<uint64_t>    _total_scheduled_tasks{ 0 };
  std::atomic<uint64_t>    _total_mesh_steals{ 0 };
  std::atomic<uint64_t>    _total_tbb_suppressed_tasks{ 0 };

public:
  RedjetBareMetalScheduler();
  ~RedjetBareMetalScheduler();

  RJ_API void init(uint32_t core_count = 0, RedjetSchedulerMode mode = RedjetSchedulerMode::DOMESTIC);
  RJ_API void shutdown();

  // Enforces strict mutual exclusion between Domestic and TBB
  RJ_API void set_mode(RedjetSchedulerMode mode);

  RJ_API bool schedule(void (*func)(void*), void* data, uint32_t core_hint = UINT32_MAX, uint32_t priority = 0);
  RJ_API bool dispatch_next_task(uint32_t core_id);
  RJ_API bool steal_mesh_aware(uint32_t thief_core, RedjetHardwareTask* out_task);

  // Status & Telemetry
  uint32_t active_cores() const noexcept { return _active_cores; }
  RedjetSchedulerMode mode() const noexcept { return _mode; }
  bool is_running() const noexcept { return _running.load(std::memory_order_relaxed); }
  bool is_domestic_active() const noexcept { return _domestic_active.load(std::memory_order_relaxed); }
  bool is_tbb_active() const noexcept { return _tbb_active.load(std::memory_order_relaxed); }

  uint64_t total_scheduled_tasks()       const noexcept { return _total_scheduled_tasks.load(std::memory_order_relaxed); }
  uint64_t total_mesh_steals()           const noexcept { return _total_mesh_steals.load(std::memory_order_relaxed); }
  uint64_t total_tbb_suppressed_tasks()   const noexcept { return _total_tbb_suppressed_tasks.load(std::memory_order_relaxed); }

  RedjetDomesticScheduler& domestic_scheduler() noexcept { return _domestic_scheduler; }

  RedjetPerCoreQueue* get_core_queue(uint32_t core_id) {
    return (core_id < _active_cores) ? &_core_queues[core_id] : nullptr;
  }

  void print_on(outputStream* st) const;
};

extern RedjetBareMetalScheduler g_redjet_scheduler;

#endif // SHARE_GC_REDJET_RUNTIME_REDJETSCHEDULER_HPP
