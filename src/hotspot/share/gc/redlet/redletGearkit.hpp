/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp core architecture.
 */

#ifndef SHARE_GC_REDLET_REDLETGEARKIT_HPP
#define SHARE_GC_REDLET_REDLETGEARKIT_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <atomic>

#ifndef MSIMD_API
#if defined(_WIN32)
#define MSIMD_API __declspec(dllexport)
#else
#define MSIMD_API __attribute__((visibility("default")))
#endif
#endif

#ifndef SACFOR_ALIGN_CL
#define SACFOR_ALIGN_CL alignas(64)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* 1. GEARKIT PROFILES & SCGC EXECUTION MODES                                */
/* ========================================================================= */

typedef enum msimd_profile_mode_t {
    MSIMD_PROFILE_STANDARD        = 0, // NORMAX: Dynamic SIMD throughput with oneTBB heterogeneous stealing
    MSIMD_PROFILE_SAFETY_CRITICAL = 1  // SSIOLSC: Hard real-time bounded latency deterministic mode
} msimd_profile_mode_t;

typedef enum SF_custominiVm_scgc_type_t {
    SF_SCGC_TYPE_DEFAULT      = 0, // Bureaucratic STW Phase GC, header chain alive
    SF_SCGC_TYPE_PRIMITIVE_RW = 1  // Unmanaged direct memory access, header bypass, raw sector R/W
} SF_custominiVm_scgc_type_t;

/* ========================================================================= */
/* 2. EBIN0 TELEMETRY CHANNELS / EVENT CATEGORIES                            */
/* ========================================================================= */

typedef enum SF_custominiVm_event_category_e {
    // 0x01..0x10 Core Lifetime
    SF_Ebin0_ALLOC                  = 0x01,
    SF_Ebin0_FREE                   = 0x02,
    SF_Ebin0_PROMOTE                = 0x03,
    SF_Ebin0_DEMOTE                 = 0x04,
    SF_Ebin0_TO_FALLINE             = 0x05,
    SF_Ebin0_TO_LANDFILL            = 0x06,
    SF_Ebin0_EMPTYBOX               = 0x07,
    SF_Ebin0_LANDFILL_EVAC          = 0x08,
    SF_Ebin0_HOTSWAP                = 0x09,
    SF_Ebin0_HOTSWAP_FAIL           = 0x0A,
    SF_Ebin0_GC_SWEEP               = 0x0B,
    SF_Ebin0_GC_COMPACT             = 0x0C,
    SF_Ebin0_GC_ALIGN               = 0x0D,
    SF_Ebin0_SAFEPOINT              = 0x0E,
    SF_Ebin0_HOOK                   = 0x0F,
    SF_Ebin0_ERROR                  = 0x10,

    // 0x20..0x2F Runway & Allocation
    SF_Ebin0_ALLOC_BUMP             = 0x20,
    SF_Ebin0_ALLOC_FREELIST         = 0x21,
    SF_Ebin0_ALLOC_CHUNK_CHAIN      = 0x22,
    SF_Ebin0_ALLOC_REBATE           = 0x23,
    SF_Ebin0_ALLOC_PACE_STALL       = 0x24,
    SF_Ebin0_MOTHERLINE_EGG_ADMIT   = 0x25,
    SF_Ebin0_MOTHERLINE_FLEDGLING   = 0x26,
    SF_Ebin0_PITSTOP_EXCISE         = 0x27,
    SF_Ebin0_PITSTOP_SHADOW         = 0x28,
    SF_Ebin0_PITSTOP_RETURN         = 0x29,
    SF_Ebin0_PITSTOP_AWAIT_STALL    = 0x2A,
    SF_Ebin0_PITSTOP_CALLBACK       = 0x2B,
    SF_Ebin0_FALLINE_SNATCH_HIT     = 0x2C,
    SF_Ebin0_FALLINE_SNATCH_MISS    = 0x2D,
    SF_Ebin0_FALLINE_SPLIT          = 0x2E,
    SF_Ebin0_FALLINE_COALESCE       = 0x2F,

    // 0x30..0x3F Topology & 4-Points Execution
    SF_Ebin0_SAFEPOINT_RENDEZVOUS   = 0x30,
    SF_Ebin0_SAFEPOINT_RELEASE      = 0x31,
    SF_Ebin0_HOOKPOINT_ENTER        = 0x32,
    SF_Ebin0_HOOKPOINT_EXIT         = 0x33,
    SF_Ebin0_CONTROLPOINT_AUDIT     = 0x34,
    SF_Ebin0_CONTROLPOINT_PAGEIN    = 0x35,
    SF_Ebin0_PAUSEPOINT_HALT        = 0x36,
    SF_Ebin0_INTERRUPT_GATE_SET     = 0x37,

    // 0x40..0x4F sfGC & Generational Rooms
    SF_Ebin0_GC_PHASE_CHANGE        = 0x40,
    SF_Ebin0_ROOM_PROMOTE_YOUNG_ADOL = 0x41,
    SF_Ebin0_ROOM_PROMOTE_ADOL_SEN  = 0x42,
    SF_Ebin0_REGION_MIGRATE         = 0x43,
    SF_Ebin0_RMRP_SPAN_TRIM         = 0x44,
    SF_Ebin0_RMRP_COALESCE_DECOMMIT = 0x45,
    SF_Ebin0_RESURRECT_REQUEST      = 0x46,
    SF_Ebin0_RESURRECT_RESTORED     = 0x47,
    SF_Ebin0_RESURRECT_QUEUE_DROP   = 0x48,
    SF_Ebin0_LANDFILL_CRITICAL_WARN = 0x49,
    SF_Ebin0_SLICEVIEW_SALVAGED     = 0x4A,

    // 0x50..0x5F Scheduler & ALU Stealing
    SF_Ebin0_ALU_DISPATCH_DIRECT    = 0x50,
    SF_Ebin0_ALU_SATURATE_STEAL     = 0x51,
    SF_Ebin0_GROUP_COMPUTE_BEGIN    = 0x52,
    SF_Ebin0_GROUP_COMPUTE_RESET    = 0x53,
    SF_Ebin0_CORE_PIN_PERF          = 0x54,
    SF_Ebin0_CORE_UNPIN_ANY         = 0x55,
    SF_Ebin0_UPCLOCKER_BOOST_ON     = 0x56,
    SF_Ebin0_UPCLOCKER_BOOST_OFF    = 0x57,
    SF_Ebin0_FOCUS_MODE_SWITCH      = 0x58,

    // 0x60..0x6F SRAM & Streaming Stores
    SF_Ebin0_SRAM_PREWARM_START     = 0x60,
    SF_Ebin0_STORE_FENCE_BROADCAST  = 0x61,
    SF_Ebin0_STREAMING_STORE_ENGAGE = 0x62,
    SF_Ebin0_STREAMING_STORE_FLUSH  = 0x63,
    SF_Ebin0_SCRATCHPAD_ALLOC       = 0x64,
    SF_Ebin0_SRAM_COMMIT_DRAM       = 0x65,

    // 0x70..0x7F JIT & Pipeline Fusion
    SF_Ebin0_BITCODE_DECOMPRESS_OK  = 0x70,
    SF_Ebin0_ORCJIT_MODULE_ADDED    = 0x71,
    SF_Ebin0_ORCJIT_RESOLVE_SUCCESS = 0x72,
    SF_Ebin0_ENTANGLE_TX_BEGIN      = 0x73,
    SF_Ebin0_ENTANGLE_TX_COMMIT     = 0x74,
    SF_Ebin0_LLVM_PASS_INJECTED     = 0x75,

    // 0x80..0x8F Self-Healing & Parity Recovery
    SF_Ebin0_HEAL_HEADER_REPAIRED   = 0x80,
    SF_Ebin0_HEAL_BROOKS_SEVERED    = 0x81,
    SF_Ebin0_HEAL_CANARY_QUARANTINE = 0x82,
    SF_Ebin0_HEAL_NUMERIC_DIVERGE   = 0x83,
    SF_Ebin0_HEAL_BITCODE_EXCEPTION = 0x84,

    // 0x90..0x9F VFS & Paging
    SF_Ebin0_VFS_MKDIR_SUCCESS      = 0x90,
    SF_Ebin0_VFS_BIND_TENSOR        = 0x91,
    SF_Ebin0_VFS_UNLINK_TENSOR      = 0x92,
    SF_Ebin0_VFS_EVICT_TREE         = 0x93,
    SF_Ebin0_PAGEFILE_PAGEOUT       = 0x94,
    SF_Ebin0_PAGEFILE_PAGEIN        = 0x95,
    SF_Ebin0_HOST_GC_INTERCEPT      = 0x96
} SF_custominiVm_event_category_e;

/* ========================================================================= */
/* 3. EBIN1 TELEMETRY SEVERITY LEVELS                                        */
/* ========================================================================= */

typedef enum SF_custominiVm_severity_e {
    SF_Ebin1_TRACE = 0,
    SF_Ebin1_INFO  = 1,
    SF_Ebin1_WARN  = 2,
    SF_Ebin1_ERROR = 3,
    SF_Ebin1_FATAL = 4
} SF_custominiVm_severity_e;

/* ========================================================================= */
/* 4. ROOMS & REGIONS CONSTANTS                                              */
/* ========================================================================= */

#define SACFOR_ROOM_COUNT   4
#define SACFOR_REGION_COUNT 8

typedef enum sf_room_id_t {
    SF_ROOM_YOUNG      = 0,
    SF_ROOM_ADOLESCENT = 1,
    SF_ROOM_SENIOR     = 2,
    SF_ROOM_IMMORTAL   = 3
} sf_room_id_t;

/* ========================================================================= */
/* 5. 64-BYTE ALIGNED HOTLOG & COLDLOG TELEMETRY ENTRIES                     */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct SACFOR_ALIGN_CL SF_custominiVm_hotlog_entry_t {
    uint64_t ts_ns;
    uint64_t tsc;
    uint64_t gc_sweep_no;
    uint32_t fn_id;
    uint32_t heap_used_kb;
    uint16_t slot;
    uint16_t current_core;
    uint8_t  generation;
    uint8_t  region;
    uint8_t  event;     // SF_Ebin0_*
    uint8_t  severity;  // SF_Ebin1_*
    char     msg[23];
    uint8_t  ready;
} SF_custominiVm_hotlog_entry_t;
#pragma pack(pop)

static_assert(sizeof(SF_custominiVm_hotlog_entry_t) == 64,
    "SF_custominiVm_hotlog_entry_t must be exactly 64 bytes");

#pragma pack(push, 1)
typedef struct SACFOR_ALIGN_CL SF_custominiVm_coldlog_entry_t {
    uint64_t ts_ns;
    uint32_t fn_id;
    uint8_t  event;     // SF_Ebin0_*
    uint8_t  severity;  // SF_Ebin1_*
    char     msg[50];
} SF_custominiVm_coldlog_entry_t;
#pragma pack(pop)

static_assert(sizeof(SF_custominiVm_coldlog_entry_t) == 64,
    "SF_custominiVm_coldlog_entry_t must be exactly 64 bytes");

/* ========================================================================= */
/* 6. GEARKIT CONTROL APIS                                                   */
/* ========================================================================= */

MSIMD_API int SacFor_Gearkit_NORMAX(void);
MSIMD_API int SacFor_Gearkit_SSIOLSC(void);
MSIMD_API int SacFor_Gearkit_SSIOLSC_Ex(SF_custominiVm_scgc_type_t scgc_type);
MSIMD_API msimd_profile_mode_t SacFor_Gearkit_GetMode(void);

MSIMD_API int  SF_custominiVm_set_mode_normax(void);
MSIMD_API int  SF_custominiVm_set_mode_ssiolsc(void);
MSIMD_API void SF_custominiVm_set_scgc_type(SF_custominiVm_scgc_type_t type);
MSIMD_API SF_custominiVm_scgc_type_t SF_custominiVm_get_scgc_type(void);

/* ========================================================================= */
/* 7. EBIN TELEMETRY & STRING LOGGING FUNCTIONS                              */
/* ========================================================================= */

MSIMD_API const char* sf_event_category_name(uint8_t ev);
MSIMD_API const char* sf_severity_str(uint8_t sev);
MSIMD_API const char* sf_severity_ansi_color(uint8_t sev);

MSIMD_API void sf_hotlog_emit(SF_custominiVm_event_category_e ev,
                              SF_custominiVm_severity_e sev,
                              uint32_t fn_id, uint16_t slot, uint8_t gen, uint8_t reg,
                              uint16_t core, uint64_t sweep_no, uint32_t heap_kb,
                              const char* msg);

MSIMD_API void sf_unified_log_emit(SF_custominiVm_event_category_e ev,
                                   SF_custominiVm_severity_e sev,
                                   uint32_t fn_id,
                                   const char* short_tag,
                                   const char* full_text);

MSIMD_API void sacculus_coldlog_write(void* log,
                                      SF_custominiVm_event_category_e ev,
                                      SF_custominiVm_severity_e sev,
                                      uint32_t fn_id,
                                      const char* msg,
                                      bool broadcast_raw = true);

MSIMD_API size_t SF_custominiVm_hotlog_format_cli(const SF_custominiVm_hotlog_entry_t* entry,
                                                  char* buffer, size_t buffer_size);
MSIMD_API size_t SF_custominiVm_hotlog_format_json(const SF_custominiVm_hotlog_entry_t* entry,
                                                   char* buffer, size_t buffer_size);
MSIMD_API size_t SF_custominiVm_coldlog_format_cli(const SF_custominiVm_coldlog_entry_t* entry,
                                                   char* buffer, size_t buffer_size);
MSIMD_API size_t SF_custominiVm_coldlog_format_json(const SF_custominiVm_coldlog_entry_t* entry,
                                                    char* buffer, size_t buffer_size);

/* ========================================================================= */
/* 8. 4-POINTS EXECUTION COORDINATOR (SAFE / CONTROL / HOOK / PAUSE)         */
/* ========================================================================= */

#define SF_HOOKPOINT_KERNEL_ENTER   1
#define SF_HOOKPOINT_KERNEL_EXIT    2
#define SF_HOOKPOINT_GC_PRE_SWEEP   3
#define SF_HOOKPOINT_GC_POST_SWEEP  4
#define SF_HOOKPOINT_GC_PRE_COMPACT 5
#define SF_HOOKPOINT_GC_POST_COMPACT 6
#define SF_HOOKPOINT_DEOPT_TRIGGER  7

typedef struct SF_custominiVm_point_telemetry_t {
    uint64_t safepoint_polls;
    uint64_t safepoint_rendezvous_stalls;
    uint32_t safepoint_parked_threads;
    uint32_t active_in_flight_kernels;
    uint64_t controlpoint_checks;
    uint64_t controlpoint_pacing_triggers;
    uint64_t controlpoint_auto_pageins;
    uint64_t controlpoint_bytes_validated;
    uint64_t hookpoint_enter_count;
    uint64_t hookpoint_exit_count;
    uint64_t hookpoint_auto_pinned_tensors;
    uint64_t hookpoint_canary_faults_caught;
    uint64_t pausepoint_polls;
    uint64_t pausepoint_preemptions;
} SF_custominiVm_point_telemetry_t;

struct SACFOR_ALIGN_CL sf_point_coordinator_t {
    // 1. Safepoint: In-flight cooperative kernel rendezvous ticket
    alignas(64) std::atomic<uint64_t> safepoint_polls{ 0 };
    alignas(64) std::atomic<uint64_t> safepoint_rendezvous_stalls{ 0 };
    alignas(64) std::atomic<uint32_t> safepoint_parked_threads{ 0 };
    alignas(64) std::atomic<uint32_t> active_in_flight_kernels{ 0 };
    alignas(64) std::atomic<uint64_t> rendezvous_ticket{ 0 };

    inline void notify_rendezvous_change() noexcept {
        rendezvous_ticket.fetch_add(1, std::memory_order_acq_rel);
        rendezvous_ticket.notify_all();
    }

    // 2. Controlpoint: Allocation pacing, proactive page-in, byte validation
    alignas(64) std::atomic<uint64_t> controlpoint_checks{ 0 };
    alignas(64) std::atomic<uint64_t> controlpoint_pacing_triggers{ 0 };
    alignas(64) std::atomic<uint64_t> controlpoint_auto_pageins{ 0 };
    alignas(64) std::atomic<uint64_t> controlpoint_bytes_validated{ 0 };

    // 3. Hookpoint: Object auto-pinning, nesting depth, canary protection
    alignas(64) std::atomic<uint64_t> hookpoint_enter_count{ 0 };
    alignas(64) std::atomic<uint64_t> hookpoint_exit_count{ 0 };
    alignas(64) std::atomic<uint64_t> hookpoint_auto_pinned_tensors{ 0 };
    alignas(64) std::atomic<uint32_t> hookpoint_active_pinned{ 0 };
    alignas(64) std::atomic<uint64_t> hookpoint_canary_faults_caught{ 0 };

    // 4. Pausepoint: Microsecond preemption gate without OS thread suspension
    alignas(64) std::atomic<uint64_t> pausepoint_polls{ 0 };
    alignas(64) std::atomic<uint64_t> pausepoint_preemptions{ 0 };
    alignas(64) std::atomic<bool>     pausepoint_gate{ false };
};

struct SF_custominiVm_live_header_t;

struct sf_thread_point_ctx_t {
    static constexpr size_t MAX_PINNED_HDRS = 4;

    uint32_t                      active_fn_id{ 0 };
    void*                         active_buffer_ptr{ nullptr };
    size_t                        active_byte_size{ 0 };
    uint64_t                      enter_tsc{ 0 };
    SF_custominiVm_live_header_t* pinned_hdrs[MAX_PINNED_HDRS]{ nullptr };
    uint32_t                      pinned_hdr_count{ 0 };
    SF_custominiVm_live_header_t* pinned_hdr{ nullptr };
    uint32_t                      nesting_depth{ 0 };
    uint32_t                      irq_poll_counter{ 0 };
    bool                          prefer_streaming_stores{ false };
    bool                          in_safepoint{ false };

    // Telemetry batching counters
    uint64_t                      tl_safepoint_polls{ 0 };
    uint64_t                      tl_controlpoint_checks{ 0 };
    uint64_t                      tl_controlpoint_bytes{ 0 };
    uint64_t                      tl_pausepoint_polls{ 0 };
    uint64_t                      tl_poll_step_counter{ 0 };

    bool has_pinned_hdr(const SF_custominiVm_live_header_t* hdr) const noexcept;
    void add_pinned_hdr(SF_custominiVm_live_header_t* hdr);
    void clear_pinned_hdrs();
    void unpin_and_clear_all(uint32_t fn_id = 0);
    void flush_telemetry(void);
    ~sf_thread_point_ctx_t();
};

MSIMD_API void sf_gc_safepoint_poll(void);
MSIMD_API void sf_gc_safepoint_rendezvous_wait(void);
MSIMD_API int  sf_gc_controlpoint_check(size_t allocated_bytes);
MSIMD_API int  sf_gc_controlpoint_check_tensor(void* ptr, size_t bytes);
MSIMD_API void sf_vm_hookpoint_pin_tensor(void* ptr);
MSIMD_API void sf_vm_hookpoint_trigger(uint32_t event_id, uint32_t fn_id, void* context, size_t byte_size);
MSIMD_API void sf_gc_hookpoint_trigger(uint32_t event_id, uint32_t fn_id, void* context, size_t byte_size);
MSIMD_API void sf_gc_hookpoint_enter(uint32_t fn_id, void* tensor_hdr);
MSIMD_API void sf_gc_hookpoint_exit(void* tensor_hdr);
MSIMD_API int  sf_gc_pausepoint_poll(void);
MSIMD_API int  sf_vm_pausepoint_poll(void);
MSIMD_API void sf_gc_pausepoint_set_gate(bool armed);
MSIMD_API void SF_custominiVm_set_pausepoint_gate(int armed);
MSIMD_API int  SF_custominiVm_get_pausepoint_gate(void);
MSIMD_API void SF_custominiVm_point_telemetry_query(SF_custominiVm_point_telemetry_t* out_stat);
MSIMD_API void sf_flush_thread_point_telemetry(void);

MSIMD_API sf_point_coordinator_t* sf_get_point_coordinator(void);

#ifdef __cplusplus
}
#endif

/* ========================================================================= */
/* 9. REDLET GEARKIT C++ HELPER CLASS                                        */
/* ========================================================================= */

class RedletGearkit {
public:
    static inline int switch_to_normax() {
        return SacFor_Gearkit_NORMAX();
    }

    static inline int switch_to_ssiolsc(SF_custominiVm_scgc_type_t scgc = SF_SCGC_TYPE_DEFAULT) {
        return SacFor_Gearkit_SSIOLSC_Ex(scgc);
    }

    static inline msimd_profile_mode_t current_mode() {
        return SacFor_Gearkit_GetMode();
    }

    static inline bool is_safety_critical() {
        return current_mode() == MSIMD_PROFILE_SAFETY_CRITICAL;
    }

    static inline sf_point_coordinator_t* point_coordinator() {
        return sf_get_point_coordinator();
    }
};

#endif // SHARE_GC_REDLET_REDLETGEARKIT_HPP
