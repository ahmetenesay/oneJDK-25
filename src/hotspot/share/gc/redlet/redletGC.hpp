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

#ifndef SHARE_GC_REDLET_REDLETGC_HPP
#define SHARE_GC_REDLET_REDLETGC_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <vector>
#include <utility>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

#include "gc/redlet/redletGearkit.hpp"
#include "gc/redlet/redletHardware.hpp"
#include "gc/redlet/redletEmptyBox.hpp"
#include "gc/redlet/redletFalline.hpp"
#include "gc/redlet/redletMotherline.hpp"
#include "gc/redlet/redletRunway.hpp"
#include "gc/redlet/redletPagefile.hpp"
#include "gc/redlet/redletPitstop.hpp"
#include "gc/redlet/redletTBBArena.hpp"
#include "gc/redlet/redletTensorViews.hpp"

#ifndef SF_SENSOR_TYPE_E_DEFINED
#define SF_SENSOR_TYPE_E_DEFINED
typedef enum SF_custominiVm_sensor_type_e {
    SF_SENSOR_GENERIC_ANALOG      = 0x01,
    SF_SENSOR_TEMP_HUMIDITY       = 0x02,
    SF_SENSOR_WATER_LEVEL         = 0x03,
    SF_SENSOR_IR_OPTICAL          = 0x04,
    SF_SENSOR_IMU_ACCEL_GYRO      = 0x05,
    SF_SENSOR_ROTARY_ENCODER      = 0x06,
    SF_SENSOR_LOGIC_ANALYZER_32CH = 0x07,
} SF_custominiVm_sensor_type_e;
typedef SF_custominiVm_sensor_type_e sfSensorType;
#endif

#ifndef SF_SENSOR_CONFIG_T_DEFINED
#define SF_SENSOR_CONFIG_T_DEFINED
typedef struct SF_custominiVm_sensor_config_t {
    SF_custominiVm_sensor_type_e sensor_type;
    uint32_t pin_primary;
    uint32_t pin_secondary;
    uint32_t sample_rate_hz;
    uint32_t buffer_sample_count;
    uint8_t  dma_enabled;
    uint8_t  gc_exempt_pinning;
    uint8_t  _pad[2];
} SF_custominiVm_sensor_config_t;
typedef SF_custominiVm_sensor_config_t sfSensorConfig;
#endif

/* ========================================================================= */
/* 1. CONSTANTS & PHASE DEFINITIONS                                          */
/* ========================================================================= */

#ifndef SACFOR_LANDFILL_CRIT
#define SACFOR_LANDFILL_CRIT 0.85f
#endif

#ifndef SACFOR_GEN_YOUNG_MAX
#define SACFOR_GEN_YOUNG_MAX 4
#endif

#ifndef SACFOR_GEN_ADOL_MAX
#define SACFOR_GEN_ADOL_MAX 8
#endif

#ifndef SACFOR_HEAT_COLD
#define SACFOR_HEAT_COLD 0.20f
#endif

#ifndef SACFOR_HEAT_HOT
#define SACFOR_HEAT_HOT 2.0f
#endif

#ifndef SF_REHAB_SCAN_INTERVAL
#define SF_REHAB_SCAN_INTERVAL 10
#endif

#ifndef SF_FALLINE_COMPACT_RATIO
#define SF_FALLINE_COMPACT_RATIO 0.30f
#endif

#ifndef SACFOR_GC_INTERVAL_NS
#define SACFOR_GC_INTERVAL_NS 10000000ULL // 10 ms default background interval
#endif

enum sf_gc_phase_e : uint8_t {
    SF_GC_PHASE_IDLE     = 0,
    SF_GC_PHASE_SWEEP    = 1,
    SF_GC_PHASE_DRAIN    = 2,
    SF_GC_PHASE_REHAB    = 3,
    SF_GC_PHASE_COMPACT  = 4,
    SF_GC_PHASE_EVACUATE = 5
};

enum sf_gc_run_mode_e : uint8_t {
    SF_GC_RUN_SINGLE_STEP      = 0,
    SF_GC_RUN_CONTINUE_FINISH  = 1,
    SF_GC_RUN_STEPPING_MTP     = 2
};
typedef sf_gc_run_mode_e SF_custominiVm_gc_run_mode_t;


enum SF_custominiVm_gc_hook_event_e : uint8_t {
    SF_GC_HOOK_PRE_SWEEP    = 1,
    SF_GC_HOOK_POST_SWEEP   = 2,
    SF_GC_HOOK_PRE_COMPACT  = 3,
    SF_GC_HOOK_POST_COMPACT = 4,
    SF_GC_HOOK_EVACUATE     = 5
};

typedef void (*SF_custominiVm_gc_hook_fn_t)(SF_custominiVm_gc_hook_event_e ev,
                                            uint64_t sweep_no,
                                            size_t total_heap_bytes,
                                            void* user_data);

/* ========================================================================= */
/* 2. STEP RESULT & TELEMETRY MANIFEST TYPES                                 */
/* ========================================================================= */

struct sf_step_result_t {
    size_t swept{ 0 };
    size_t drained{ 0 };
    size_t resurrected{ 0 };
    size_t rebated{ 0 };
};

struct SF_custominiVm_gc_manifest_t {
    uint64_t sweep_id{ 0 };
    size_t   rooms_inspected{ 3 };
    size_t   cells_swept{ 0 };
    size_t   cells_drained{ 0 };
    size_t   bytes_reclaimed{ 0 };
    size_t   heap_bytes_after{ 0 };
    uint64_t elapsed_ns{ 0 };
    uint8_t  quiescent{ 1 };
    uint8_t  trailing_debris_cleared{ 0 };
    char     narrative[256]{ 0 };
};

struct sf_env_snapshot_t {
    size_t   runway_bump{ 0 };
    size_t   allocated_bytes{ 0 };
    size_t   pacing_stalls{ 0 };
    uint64_t total_calls{ 0 };
    uint64_t resurrections{ 0 };
    size_t   paged_out_count{ 0 };
    size_t   total_heap{ 0 };
    uint64_t cp_bytes_validated{ 0 };
    uint64_t cp_pacing_triggers{ 0 };
    uint64_t hp_canary_faults{ 0 };
    uint64_t sp_rendezvous_stalls{ 0 };
};

struct sf_mltc_report_t {
    double   velocity_mbps{ 0.0 };
    size_t   heap_bytes{ 0 };
    uint32_t steady_increase_count{ 0 };
    size_t   total_tensors_scanned{ 0 };
    size_t   sram_misaligned_count{ 0 };
    size_t   dram_misaligned_count{ 0 };
    bool     leak_detected{ false };
    bool     traffic_congestion_detected{ false };
    bool     alignment_hazard_detected{ false };
};

typedef struct SF_custominiVm_anatomy_stat_t {
    /* 1. On-Heap (Garbage-Collected Unified Memory) */
    size_t onheap_reserved_bytes;
    size_t onheap_committed_bytes;
    size_t onheap_allocated_bytes;

    /* 1.1 Runway Nursery Domain & Split Zones */
    size_t runway_total_bytes;
    size_t runway_used_bytes;
    size_t motherline_live_bytes;
    size_t egg_count;
    size_t fledgling_count;
    size_t pitstop_count;
    size_t pitstop_total_morphed;
    size_t falline_emptybox_bytes;
    size_t emptybox_count;

    /* 1.2 On-Heap Generational Rooms & Topological Regions */
    size_t room_young_bytes;
    size_t room_young_objects;
    size_t room_adolescent_bytes;
    size_t room_adolescent_objects;
    size_t room_senior_bytes;
    size_t room_senior_objects;

    size_t region_demand_bytes;
    size_t region_demand_objects;
    size_t region_middle_bytes;
    size_t region_middle_objects;
    size_t tank_regions_bytes;
    size_t tank_regions_count;
    size_t region_hotfield_bytes;
    size_t region_hotfield_objects;

    /* 1.3 Independent Puzzle Arena */
    size_t puzzle_arena_total_bytes;
    size_t puzzle_arena_used_bytes;

    /* 1.4 Landfill Domain (Mature Objects & Evacuation) */
    size_t landfill_total_bytes;
    size_t landfill_used_bytes;
    size_t matured_arena_bytes;
    size_t matured_entry_count;
    size_t rehab_arena_bytes;
    size_t rehab_entry_count;
    uint32_t landfill_pane_count;
    uint64_t matured_flush_count;
    uint64_t rehab_scan_count;
    uint64_t resurrection_count;

    /* 2. Off-Heap Domain (Non-GC Executable & Telemetry) */
    size_t offheap_code_arena_bytes;
    size_t offheap_telemetry_bytes;

    /* 3. Dynamic Slab Expansion & Rebating */
    size_t chained_chunk_count;
    size_t rebated_bytes;

    /* 4. Allocation Pacing & Virtual Memory Pagefile Paging */
    size_t pacing_stall_count;
    size_t paged_out_bytes;
    size_t paged_out_count;
} SF_custominiVm_anatomy_stat_t;

typedef struct SF_custominiVm_anatomy_stat_t sfAnatomyStat;

typedef struct SF_custominiVm_env_change_t {
    int      changed;
    uint32_t allocations_detected;
    uint32_t pacing_stalls_delta;
    uint32_t calls_delta;
    uint32_t resurrections_delta;
    uint32_t paged_out_delta;
} SF_custominiVm_env_change_t;

typedef SF_custominiVm_env_change_t sfEnvChange;

typedef int (*SF_custominiVm_mtp_predicate_fn)(const SF_custominiVm_anatomy_stat_t* stat,
                                               const SF_custominiVm_env_change_t* env,
                                               void* user_data);

struct rehab_restore_info_t {
    uint32_t function_id{ 0 };
    uint64_t type_sig_hash{ 0 };
    size_t   previous_size{ 0 };
    size_t   pit_stored_size{ 0 };
    uint32_t crc32{ 0 };
    size_t   rehab_offset{ 0 };
    void*    rehab_payload_ptr{ nullptr };
    uint8_t  peak_generation{ 0 };
    uint8_t  peak_region{ 0 };
    uint16_t last_core{ 0 };
    uint32_t pane_idx{ 0 };
};

struct SF_custominiVm_hotlog_entry_t;
typedef void (*sf_telemetry_stream_fn_t)(const SF_custominiVm_hotlog_entry_t* entry,
                                        const char* formatted_cli,
                                        void* user_data);

struct sf_vm_instance;
typedef struct sf_vm_instance* sf_vm_handle;

/* ========================================================================= */
/* 3. BIG GC ENGINE (sacculus_gc_t / RedletGC)                               */
/* ========================================================================= */

class RedletGC {
private:
    RedletRunway          _internal_runway;
    RedletMotherline      _internal_motherline;
    RedletFalline         _internal_falline;
    RedletLandfill        _internal_landfill;

    RedletRunway*         _runway{ &_internal_runway };
    RedletMotherline*     _motherline{ &_internal_motherline };
    RedletFalline*        _falline{ &_internal_falline };
    RedletLandfill*       _landfill{ &_internal_landfill };

public:
    sf_pagefile_t         pagefile;

    std::thread           gc_thread;
    std::atomic<bool>     running{ false };
    std::atomic<uint64_t> sweep_count{ 0 };
    std::atomic<size_t>   total_heap_bytes{ 0 };
    std::atomic<size_t>   pressure_threshold{ 64ULL * 1024 * 1024 };

    alignas(64) std::atomic<uint8_t>  current_phase{ SF_GC_PHASE_IDLE };
    alignas(64) std::atomic<uint64_t> matured_flush_count{ 0 };
    alignas(64) std::atomic<uint64_t> rehab_scan_count{ 0 };
    alignas(64) std::atomic<uint64_t> resurrection_count{ 0 };
    alignas(64) std::atomic<uint64_t> falline_compact_count{ 0 };
    std::atomic<uint64_t>             last_sweep_ns{ 0 };
    std::atomic<uint64_t>             last_line_compaction_ts_ns{ 0 };
    std::atomic<uint64_t>             total_calls_tracked{ 0 };

    SF_custominiVm_gc_hook_fn_t       gc_hook{ nullptr };
    void*                             gc_hook_user{ nullptr };
    SF_custominiVm_gc_manifest_t      last_manifest{};
    std::mutex                        manifest_lock;

    float                             heat_decay_factor{ 0.97f };
    float                             landfill_critical_ratio{ SACFOR_LANDFILL_CRIT };
    float                             falline_compact_ratio{ SF_FALLINE_COMPACT_RATIO };
    uint64_t                          line_compaction_freq_ns{ 50000000ULL };

    static constexpr uint32_t         RESURRECT_QUEUE_CAP = 128u;
    std::atomic<uint32_t>             resurrect_queue[RESURRECT_QUEUE_CAP];
    std::atomic<uint32_t>             resurrect_write{ 0 };
    std::atomic<uint32_t>             resurrect_read{ 0 };
    sf_ticket_lock_t                  resurrect_lock;

    MSIMD_API RedletGC();
    MSIMD_API ~RedletGC();

    MSIMD_API void init(RedletRunway* r = nullptr,
                        RedletMotherline* m = nullptr,
                        RedletFalline* f = nullptr,
                        RedletLandfill* l = nullptr,
                        size_t pagefile_bytes = 64ULL * 1024 * 1024);

    MSIMD_API void set_gc_phase(sf_gc_phase_e next_phase, const char* phase_label = "gc_phase");
    MSIMD_API void trigger_hook(SF_custominiVm_gc_hook_event_e ev);

    // 4-Points Cooperative Rendezvous Handshake
    MSIMD_API void wait_for_safepoint_rendezvous(void);

    // Object Admission, Aging, Eviction & Promotion
    MSIMD_API void admit(sfLiveHeader* obj);
    MSIMD_API void admit_egg_object(sfLiveHeader* obj);
    MSIMD_API void mark_garbage(sfLiveHeader* obj);

    // Slice Views Salvage & Resurrection Engine
    MSIMD_API bool salvage_orphaned_slice_views(sfLiveHeader* parent, sf_region_t* already_locked_region = nullptr);
    MSIMD_API bool restore_rehab_payload_and_link(const rehab_restore_info_t& info);
    MSIMD_API size_t resurrect_entries_for_fn(uint32_t fn_id);
    MSIMD_API size_t scan_rehab_arena(void);
    MSIMD_API void request_resurrection(uint32_t fn_id);

    // RMRP (Reduce Memory Resident Propagation)
    MSIMD_API size_t execute_rmrp(sf_snatch_rebate_mode_e rebate_mode, sf_rmrp_method_e method);

    // Falline Emptybox Drain & Landfill Critical Evacuation
    MSIMD_API size_t drain_falline(void);

    // Generational Room Sweeper (Young -> Adolescent -> Senior)
    MSIMD_API size_t sweep_single_room(sf_room_t& room, uint8_t room_id, uint64_t sweep_no,
                                      uint32_t heap_kb, float pressure_ratio,
                                      bool is_e_core, bool use_simd);
    MSIMD_API size_t sweep_motherline_parallel(bool use_simd);
    MSIMD_API size_t sweep_motherline_cycles(void);
    MSIMD_API size_t sweep_motherline_simd(void);
    MSIMD_API size_t sweep_motherline(void);

    // Falline & Motherline Compaction
    MSIMD_API void maybe_compact_falline(void);
    MSIMD_API void compact_single_room(sf_room_t& room, uint8_t room_id, uint64_t sweep_no,
                                      size_t max_compact_fp, bool use_simd,
                                      size_t& out_relocated_count, size_t& out_relocated_bytes);
    MSIMD_API void compact_and_relocate_motherline_adaptive(uint64_t sweep_no, bool use_simd);
    MSIMD_API void compact_and_relocate_motherline_cycles(uint64_t sweep_no);
    MSIMD_API void compact_and_relocate_motherline_simd(uint64_t sweep_no);
    MSIMD_API void compact_and_relocate_motherline(uint64_t sweep_no);
    MSIMD_API bool should_trigger_line_compaction(uint64_t current_sweep, uint64_t fallback_interval = 50);

    // Telemetry & Environment Snapshots
    MSIMD_API sf_env_snapshot_t capture_env(void) const;
    MSIMD_API bool has_env_changed(const sf_env_snapshot_t& prev, const sf_env_snapshot_t& cur) const;
    MSIMD_API sf_mltc_report_t evaluate_mltc_diagnostics(size_t cur_b, size_t last_b, uint64_t diff_time,
                                                        size_t& peak_b, uint32_t& steady_count, uint64_t sweep_no);

    // Main Control Loop & Lifecycle
    MSIMD_API void start(void);
    MSIMD_API void stop(void);
    MSIMD_API sf_step_result_t step(void);
    MSIMD_API SF_custominiVm_gc_manifest_t shake_bed(void);
    MSIMD_API size_t run(SF_custominiVm_gc_run_mode_t mode,
                         bool owp,
                         SF_custominiVm_mtp_predicate_fn mtp_pred = nullptr,
                         void* user_data = nullptr);

    // Accessors
    RedletRunway*     runway()     { return _runway; }
    RedletMotherline* motherline() { return _motherline; }
    RedletFalline*    falline()    { return _falline; }
    RedletLandfill*   landfill()   { return _landfill; }
};

using sacculus_gc_t = RedletGC;
using sfGC = RedletGC;

/* ========================================================================= */
/* 4. CANONICAL C APIS                                                       */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

MSIMD_API RedletGC*   sf_get_default_gc(void);
MSIMD_API void        SF_custominiVm_register_gc_hook(SF_custominiVm_gc_hook_fn_t hook, void* user_data);
MSIMD_API void        SF_custominiVm_host_gc_notify(uint32_t host_event_id);
MSIMD_API size_t      SF_custominiVm_host_loop_tick(uint32_t budget_us);
MSIMD_API void        SF_custominiVm_gc_shake_bed(SF_custominiVm_gc_manifest_t* out_manifest);
MSIMD_API void        SF_custominiVm_gc_step(sf_step_result_t* out_result);
MSIMD_API size_t      SF_custominiVm_gc_run(SF_custominiVm_gc_run_mode_t mode, int owp);
MSIMD_API uint64_t    SF_custominiVm_gc_sweep_count(void);
MSIMD_API size_t      SF_custominiVm_gc_total_heap_bytes(void);

/* RMRP & Resurrection C-APIs */
MSIMD_API void        SF_custominiVm_set_rmrp(int enable, int method);
MSIMD_API int         SF_custominiVm_get_rmrp(void);
MSIMD_API size_t      SF_custominiVm_run_rmrp(void);
MSIMD_API void        SF_custominiVm_resurrect_request(uint32_t fn_id);

/* Anatomy Topography C-APIs */
MSIMD_API void        SF_custominiVm_anatomy_query(SF_custominiVm_anatomy_stat_t* out_stat);
MSIMD_API const char* SF_custominiVm_anatomy_description(void);

/* Telemetry Stream Daemon C-APIs */
MSIMD_API int         SF_custominiVm_telemetry_stream_start(uint32_t flush_interval_us,
                                                           sf_telemetry_stream_fn_t callback,
                                                           void* user_data);
MSIMD_API void        SF_custominiVm_telemetry_stream_stop(void);
MSIMD_API int         SF_custominiVm_telemetry_stream_is_running(void);

/* VM Instance Lifecycle C-APIs */
MSIMD_API sf_vm_handle SF_custominiVm_get_default_instance(void);
MSIMD_API int         SF_custominiVm_instance_is_booted(sf_vm_handle vm);
MSIMD_API void        SF_custominiVm_instance_shutdown(sf_vm_handle vm);
MSIMD_API int         SF_custominiVm_instance_reset(sf_vm_handle vm);
MSIMD_API void        SF_custominiVm_gc_admit(sfLiveHeader* obj);
MSIMD_API void        SF_custominiVm_gc_touch(sfLiveHeader* obj, uint64_t cycles, uint16_t core_id);
MSIMD_API int         SF_custominiVm_sensor_attach_stream(const SF_custominiVm_sensor_config_t* cfg, SF_custominiVm_tensor_t* out_stream_tensor);
MSIMD_API size_t      SF_custominiVm_sensor_poll_stream(SF_custominiVm_tensor_t* stream_tensor, size_t max_samples);
MSIMD_API void        SF_custominiVm_sensor_detach_stream(SF_custominiVm_tensor_t* stream_tensor);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDLET_REDLETGC_HPP
