/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp core architecture.
 */

#include "gc/redlet/redletGearkit.hpp"
#ifndef REDLET_STANDALONE_BUILD
#include "gc/redlet/redlet_globals.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_trace(...) if (false) printf
#define log_warning(...) if (false) printf
#define log_error(...) if (false) printf
#define FLAG_SET_ERGO(flag, value) ((void)0)
#endif
#include <chrono>
#include <mutex>

static std::atomic<msimd_profile_mode_t> g_gearkit_mode{ MSIMD_PROFILE_STANDARD };
static std::atomic<SF_custominiVm_scgc_type_t> g_scgc_type{ SF_SCGC_TYPE_DEFAULT };
static sf_point_coordinator_t g_point_coordinator;
static thread_local sf_thread_point_ctx_t tl_point_ctx;
static std::atomic<bool> g_stream_pause_gate{ false };

/* ── HotLog Extended Strings Buffer ── */
static constexpr uint32_t SF_HOTLOG_EXT_STRING_SLOTS = 4096u;
static constexpr uint16_t SF_HOTLOG_EXT_SLOT_FLAG    = 0x8000u;
static constexpr uint16_t SF_HOTLOG_EXT_SLOT_MASK    = 0x0FFFu;
alignas(64) static char g_sf_hotlog_ext_strings[SF_HOTLOG_EXT_STRING_SLOTS][256] = { {0} };
static std::atomic<uint32_t> g_sf_hotlog_ext_cursor{ 0 };

static inline uint16_t sf_store_hotlog_ext_string(const char* full_text) noexcept {
    if (!full_text || !full_text[0]) return 0;
    uint32_t idx = g_sf_hotlog_ext_cursor.fetch_add(1, std::memory_order_relaxed) & SF_HOTLOG_EXT_SLOT_MASK;
    size_t len = strlen(full_text);
    while (len > 0 && (full_text[len - 1] == '\n' || full_text[len - 1] == '\r')) {
        --len;
    }
    if (len >= 255) len = 255;
    memcpy(g_sf_hotlog_ext_strings[idx], full_text, len);
    g_sf_hotlog_ext_strings[idx][len] = '\0';
    return static_cast<uint16_t>(SF_HOTLOG_EXT_SLOT_FLAG | idx);
}

static inline const char* sf_lookup_hotlog_ext_string(uint16_t slot) noexcept {
    if ((slot & SF_HOTLOG_EXT_SLOT_FLAG) == 0) return nullptr;
    uint32_t idx = slot & SF_HOTLOG_EXT_SLOT_MASK;
    return g_sf_hotlog_ext_strings[idx][0] ? g_sf_hotlog_ext_strings[idx] : nullptr;
}

static inline uint64_t sacculus_now_ns() noexcept {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

/* ========================================================================= */
/* GEARKIT CONTROL APIS                                                      */
/* ========================================================================= */

MSIMD_API int SacFor_Gearkit_NORMAX(void) {
    g_gearkit_mode.store(MSIMD_PROFILE_STANDARD, std::memory_order_release);
    FLAG_SET_ERGO(RedletProfile, "NORMAX");
    FLAG_SET_ERGO(RedletSCMode, false);
    FLAG_SET_ERGO(RedletForbidDynamicGrowth, false);

    sf_unified_log_emit(SF_Ebin0_GC_SWEEP, SF_Ebin1_INFO, 0,
        "Gearkit:NORMAX",
        "Gearkit: Switched to NORMAX profile (Dynamic SIMD throughput with oneTBB heterogeneous work-stealing)");
    log_info(gc)("RedletGearkit: NORMAX profile engaged (High-throughput Dynamic SIMD)");
    return 0;
}

MSIMD_API int SacFor_Gearkit_SSIOLSC(void) {
    g_gearkit_mode.store(MSIMD_PROFILE_SAFETY_CRITICAL, std::memory_order_release);
    FLAG_SET_ERGO(RedletProfile, "SSIOLSC");
    FLAG_SET_ERGO(RedletSCMode, true);
    FLAG_SET_ERGO(RedletForbidDynamicGrowth, true);

    sf_unified_log_emit(SF_Ebin0_GC_SWEEP, SF_Ebin1_INFO, 0,
        "Gearkit:SSIOLSC",
        "Gearkit: Switched to SSIOLSC profile (Safety-Critical, deterministic real-time, zero-jitter envelope)");
    log_info(gc)("RedletGearkit: SSIOLSC profile engaged (Zero-jitter deterministic execution envelope)");
    return 0;
}

MSIMD_API int SacFor_Gearkit_SSIOLSC_Ex(SF_custominiVm_scgc_type_t scgc_type) {
    int ret = SacFor_Gearkit_SSIOLSC();
    if (ret != 0) return ret;

    g_scgc_type.store(scgc_type, std::memory_order_release);
    if (scgc_type == SF_SCGC_TYPE_PRIMITIVE_RW) {
        FLAG_SET_ERGO(RedletSCGCType, "PrimitiveRW");
        sf_unified_log_emit(SF_Ebin0_GC_SWEEP, SF_Ebin1_INFO, 0,
            "Gearkit:SSIOLSC[PRIMITIVE_RW]",
            "Gearkit: SSIOLSC[PRIMITIVE_RW] - GC disabled, header bypass, raw sector R/W");
        log_info(gc)("RedletGearkit: SSIOLSC[PRIMITIVE_RW] mode engaged (GC collection bypassed)");
    } else {
        FLAG_SET_ERGO(RedletSCGCType, "Default");
        sf_unified_log_emit(SF_Ebin0_GC_SWEEP, SF_Ebin1_INFO, 0,
            "Gearkit:SSIOLSC[DEFAULT]",
            "Gearkit: SSIOLSC[DEFAULT] - Phase-STW Bureaucratic, header chain alive");
        log_info(gc)("RedletGearkit: SSIOLSC[DEFAULT] mode engaged (Phase-STW Bureaucratic collection)");
    }
    return 0;
}

MSIMD_API msimd_profile_mode_t SacFor_Gearkit_GetMode(void) {
    return g_gearkit_mode.load(std::memory_order_acquire);
}

MSIMD_API int SF_custominiVm_set_mode_normax(void) {
    return SacFor_Gearkit_NORMAX();
}

MSIMD_API int SF_custominiVm_set_mode_ssiolsc(void) {
    return SacFor_Gearkit_SSIOLSC();
}

MSIMD_API void SF_custominiVm_set_scgc_type(SF_custominiVm_scgc_type_t type) {
    SacFor_Gearkit_SSIOLSC_Ex(type);
}

MSIMD_API SF_custominiVm_scgc_type_t SF_custominiVm_get_scgc_type(void) {
    return g_scgc_type.load(std::memory_order_acquire);
}

/* ========================================================================= */
/* EBIN LOGGING & STRING FORMATTING IMPLEMENTATION                           */
/* ========================================================================= */

MSIMD_API const char* sf_event_category_name(uint8_t ev) {
    switch (ev) {
    case 0x01: return "ALLOC";
    case 0x02: return "FREE";
    case 0x03: return "PROMOTE";
    case 0x04: return "DEMOTE";
    case 0x05: return "TO_FALLINE";
    case 0x06: return "TO_LANDFILL";
    case 0x07: return "EMPTYBOX";
    case 0x08: return "LANDFILL_EVAC";
    case 0x09: return "HOTSWAP";
    case 0x0A: return "HOTSWAP_FAIL";
    case 0x0B: return "GC_SWEEP";
    case 0x0C: return "GC_COMPACT";
    case 0x0D: return "GC_ALIGN";
    case 0x0E: return "SAFEPOINT";
    case 0x0F: return "HOOK";
    case 0x10: return "ERROR";
    case 0x20: return "RUNWAY_BUMP";
    case 0x21: return "ALLOC_FREELIST";
    case 0x22: return "CHUNK_CHAIN";
    case 0x23: return "ALLOC_REBATE";
    case 0x24: return "PACE_STALL";
    case 0x25: return "EGG_ADMIT";
    case 0x26: return "FLEDGLING_PROMOTE";
    case 0x27: return "PITSTOP_EXCISE";
    case 0x28: return "PITSTOP_SHADOW";
    case 0x29: return "PITSTOP_RETURN";
    case 0x2A: return "PITSTOP_STALL";
    case 0x2B: return "PITSTOP_CALLBACK";
    case 0x2C: return "FALLINE_SNATCH_HIT";
    case 0x2D: return "FALLINE_SNATCH_MISS";
    case 0x2E: return "FALLINE_SPLIT";
    case 0x2F: return "FALLINE_COALESCE";
    case 0x30: return "SAFEPOINT_PARK";
    case 0x31: return "SAFEPOINT_RESUME";
    case 0x32: return "HOOKPOINT_ENTER";
    case 0x33: return "HOOKPOINT_EXIT";
    case 0x34: return "CONTROLPOINT_AUDIT";
    case 0x35: return "CONTROLPOINT_PAGEIN";
    case 0x36: return "PAUSEPOINT_HALT";
    case 0x37: return "INTERRUPT_GATE";
    case 0x40: return "GC_PHASE_CHANGE";
    case 0x41: return "PROMOTE_YOUNG_ADOL";
    case 0x42: return "PROMOTE_ADOL_SENIOR";
    case 0x43: return "REGION_MIGRATE";
    case 0x44: return "RMRP_SPAN_TRIM";
    case 0x45: return "RMRP_DECOMMIT";
    case 0x46: return "RESURRECT_REQUEST";
    case 0x47: return "RESURRECT_RESTORED";
    case 0x48: return "RESURRECT_DROP";
    case 0x49: return "LANDFILL_CRIT_WARN";
    case 0x4A: return "SLICEVIEW_SALVAGED";
    case 0x50: return "ALU_DISPATCH";
    case 0x51: return "ALU_SATURATE_STEAL";
    case 0x52: return "GROUP_COMPUTE_BEGIN";
    case 0x53: return "GROUP_COMPUTE_RESET";
    case 0x54: return "CORE_PIN_PERF";
    case 0x55: return "CORE_UNPIN";
    case 0x56: return "UPCLOCKER_BOOST_ON";
    case 0x57: return "UPCLOCKER_BOOST_OFF";
    case 0x58: return "FOCUS_MODE_SWITCH";
    case 0x60: return "SRAM_PREWARM";
    case 0x61: return "STORE_FENCE_BCAST";
    case 0x62: return "STREAM_STORE_ENGAGE";
    case 0x63: return "STREAM_STORE_FLUSH";
    case 0x64: return "SCRATCHPAD_ALLOC";
    case 0x65: return "SRAM_COMMIT_DRAM";
    case 0x70: return "BITCODE_DECOMP_OK";
    case 0x71: return "ORCJIT_MODULE_ADDED";
    case 0x72: return "ORCJIT_RESOLVE_OK";
    case 0x73: return "ENTANGLE_BEGIN";
    case 0x74: return "ENTANGLE_COMMIT";
    case 0x75: return "LLVM_PASS_INJECTED";
    case 0x80: return "HEAL_HEADER_REPAIRED";
    case 0x81: return "HEAL_BROOKS_SEVERED";
    case 0x82: return "HEAL_CANARY_QUARANTINE";
    case 0x83: return "HEAL_NUMERIC_DIVERGE";
    case 0x84: return "HEAL_BITCODE_EXCEPTION";
    case 0x90: return "VFS_MKDIR_SUCCESS";
    case 0x91: return "VFS_BIND_TENSOR";
    case 0x92: return "VFS_UNLINK_TENSOR";
    case 0x93: return "VFS_EVICT_TREE";
    case 0x94: return "PAGEFILE_PAGEOUT";
    case 0x95: return "PAGEFILE_PAGEIN";
    case 0x96: return "HOST_GC_INTERCEPT";
    default:   return "UNKNOWN_EVENT";
    }
}

MSIMD_API const char* sf_severity_str(uint8_t sev) {
    switch (sev) {
    case 0: return "TRACE";
    case 1: return "INFO";
    case 2: return "WARN";
    case 3: return "ERROR";
    case 4: return "FATAL";
    default: return "UNK";
    }
}

MSIMD_API const char* sf_severity_ansi_color(uint8_t sev) {
    switch (sev) {
    case 0: return "\033[90m";       // Muted dark gray
    case 1: return "\033[32m";       // Vibrant green
    case 2: return "\033[33m";       // Amber yellow
    case 3: return "\033[31m";       // Crimson red
    case 4: return "\033[97;41;1m";  // Bold white on red background
    default: return "\033[0m";
    }
}

MSIMD_API void sf_hotlog_emit(SF_custominiVm_event_category_e ev,
                              SF_custominiVm_severity_e sev,
                              uint32_t fn_id, uint16_t slot, uint8_t gen, uint8_t reg,
                              uint16_t core, uint64_t sweep_no, uint32_t heap_kb,
                              const char* msg) {
    const char* ext = sf_lookup_hotlog_ext_string(slot);
    const char* display_msg = ext ? ext : (msg ? msg : "");

    switch (sev) {
    case SF_Ebin1_TRACE:
        log_trace(gc, telemetry)("[%s] fn=%u slot=%u gen=%u reg=%u core=%u sweep=%llu heap=%uKB: %s",
                                 sf_event_category_name((uint8_t)ev), fn_id, slot, gen, reg, core,
                                 (unsigned long long)sweep_no, heap_kb, display_msg);
        break;
    case SF_Ebin1_INFO:
        log_info(gc, telemetry)("[%s] fn=%u gen=%u reg=%u sweep=%llu heap=%uKB: %s",
                                sf_event_category_name((uint8_t)ev), fn_id, gen, reg,
                                (unsigned long long)sweep_no, heap_kb, display_msg);
        break;
    case SF_Ebin1_WARN:
        log_warning(gc, telemetry)("[%s] fn=%u sweep=%llu: %s",
                                   sf_event_category_name((uint8_t)ev), fn_id,
                                   (unsigned long long)sweep_no, display_msg);
        break;
    case SF_Ebin1_ERROR:
    case SF_Ebin1_FATAL:
        log_error(gc, telemetry)("[%s] fn=%u: %s",
                                 sf_event_category_name((uint8_t)ev), fn_id, display_msg);
        break;
    }
}

MSIMD_API void sf_unified_log_emit(SF_custominiVm_event_category_e ev,
                                   SF_custominiVm_severity_e sev,
                                   uint32_t fn_id,
                                   const char* short_tag,
                                   const char* full_text) {
    uint16_t ext_slot = sf_store_hotlog_ext_string(full_text);
    const char* tag = (short_tag && short_tag[0]) ? short_tag : (full_text ? full_text : "redlet_log");
    sf_hotlog_emit(ev, sev, fn_id, ext_slot, 0, 0, 0, 0, 0, tag);
}

MSIMD_API void sacculus_coldlog_write(void* log,
                                      SF_custominiVm_event_category_e ev,
                                      SF_custominiVm_severity_e sev,
                                      uint32_t fn_id,
                                      const char* msg,
                                      bool broadcast_raw) {
    (void)log;
    (void)broadcast_raw;
    sf_unified_log_emit(ev, sev, fn_id, nullptr, msg);
}

MSIMD_API size_t SF_custominiVm_hotlog_format_cli(const SF_custominiVm_hotlog_entry_t* entry,
                                                  char* buffer, size_t buffer_size) {
    if (!entry || !buffer || buffer_size == 0) return 0;
    const char* col = sf_severity_ansi_color(entry->severity);
    const char* rst = "\033[0m";
    const char* sev = sf_severity_str(entry->severity);
    const char* cat = sf_event_category_name(entry->event);
    const char* ext = sf_lookup_hotlog_ext_string(entry->slot);
    const char* msg_text = ext ? ext : entry->msg;

    int written = snprintf(buffer, buffer_size,
        "[%llu] %s[%-5s]%s [%-20s] core=%-2u gen=%u reg=%u sweep=%-4llu heap=%-6uKB msg=\"%s\"",
        (unsigned long long)entry->ts_ns,
        col, sev, rst, cat,
        (unsigned int)entry->current_core, (unsigned int)entry->generation, (unsigned int)entry->region,
        (unsigned long long)entry->gc_sweep_no, (unsigned int)entry->heap_used_kb,
        msg_text ? msg_text : "");
    if (written < 0) return 0;
    return (size_t)written >= buffer_size ? buffer_size - 1 : (size_t)written;
}

MSIMD_API size_t SF_custominiVm_hotlog_format_json(const SF_custominiVm_hotlog_entry_t* entry,
                                                   char* buffer, size_t buffer_size) {
    if (!entry || !buffer || buffer_size == 0) return 0;
    const char* sev = sf_severity_str(entry->severity);
    const char* cat = sf_event_category_name(entry->event);
    const char* ext = sf_lookup_hotlog_ext_string(entry->slot);
    const char* msg_text = ext ? ext : entry->msg;

    int written = snprintf(buffer, buffer_size,
        "{\"ts_ns\":%llu,\"tsc\":%llu,\"sweep\":%llu,\"fn_id\":%u,\"heap_kb\":%u,\"slot\":%u,\"core\":%u,\"gen\":%u,\"reg\":%u,\"event\":\"%s\",\"event_id\":%u,\"severity\":\"%s\",\"msg\":\"%s\"}",
        (unsigned long long)entry->ts_ns, (unsigned long long)entry->tsc, (unsigned long long)entry->gc_sweep_no,
        (unsigned int)entry->fn_id, (unsigned int)entry->heap_used_kb, (unsigned int)entry->slot,
        (unsigned int)entry->current_core, (unsigned int)entry->generation, (unsigned int)entry->region,
        cat, (unsigned int)entry->event, sev, msg_text ? msg_text : "");
    if (written < 0) return 0;
    return (size_t)written >= buffer_size ? buffer_size - 1 : (size_t)written;
}

MSIMD_API size_t SF_custominiVm_coldlog_format_cli(const SF_custominiVm_coldlog_entry_t* entry,
                                                   char* buffer, size_t buffer_size) {
    if (!entry || !buffer || buffer_size == 0) return 0;
    const char* col = sf_severity_ansi_color(entry->severity);
    const char* rst = "\033[0m";
    const char* sev = sf_severity_str(entry->severity);
    const char* cat = sf_event_category_name(entry->event);

    int written = snprintf(buffer, buffer_size,
        "[%llu] %s[%-5s]%s [%-20s] fn_id=%-4u msg=\"%s\"",
        (unsigned long long)entry->ts_ns,
        col, sev, rst, cat,
        (unsigned int)entry->fn_id,
        entry->msg[0] ? entry->msg : "");
    if (written < 0) return 0;
    return (size_t)written >= buffer_size ? buffer_size - 1 : (size_t)written;
}

MSIMD_API size_t SF_custominiVm_coldlog_format_json(const SF_custominiVm_coldlog_entry_t* entry,
                                                    char* buffer, size_t buffer_size) {
    if (!entry || !buffer || buffer_size == 0) return 0;
    const char* sev = sf_severity_str(entry->severity);
    const char* cat = sf_event_category_name(entry->event);

    int written = snprintf(buffer, buffer_size,
        "{\"ts_ns\":%llu,\"fn_id\":%u,\"event\":\"%s\",\"event_id\":%u,\"severity\":\"%s\",\"msg\":\"%s\"}",
        (unsigned long long)entry->ts_ns, (unsigned int)entry->fn_id,
        cat, (unsigned int)entry->event, sev, entry->msg[0] ? entry->msg : "");
    if (written < 0) return 0;
    return (size_t)written >= buffer_size ? buffer_size - 1 : (size_t)written;
}

/* ========================================================================= */
/* 4-POINTS EXECUTION ENGINE IMPLEMENTATION                                  */
/* ========================================================================= */

MSIMD_API void sf_gc_safepoint_poll(void) {
    ++tl_point_ctx.tl_safepoint_polls;
    g_point_coordinator.safepoint_polls.fetch_add(1, std::memory_order_relaxed);

    if (g_gearkit_mode.load(std::memory_order_relaxed) == MSIMD_PROFILE_SAFETY_CRITICAL) {
        return; // In SC mode, dynamic asynchronous rendezvous stalls are disabled
    }

    if (g_stream_pause_gate.load(std::memory_order_relaxed)) {
        ++tl_point_ctx.tl_pausepoint_polls;
        g_point_coordinator.pausepoint_preemptions.fetch_add(1, std::memory_order_relaxed);
    }
}

MSIMD_API void sf_gc_controlpoint_check(void* ptr, size_t bytes) {
    (void)ptr;
    ++tl_point_ctx.tl_controlpoint_checks;
    tl_point_ctx.tl_controlpoint_bytes += bytes;
    g_point_coordinator.controlpoint_checks.fetch_add(1, std::memory_order_relaxed);
    g_point_coordinator.controlpoint_bytes_validated.fetch_add(bytes, std::memory_order_relaxed);
}

MSIMD_API void sf_gc_hookpoint_enter(uint32_t fn_id, void* tensor_hdr) {
    tl_point_ctx.active_fn_id = fn_id;
    tl_point_ctx.nesting_depth++;
    g_point_coordinator.hookpoint_enter_count.fetch_add(1, std::memory_order_relaxed);
    if (tensor_hdr && tl_point_ctx.pinned_hdr_count < sf_thread_point_ctx_t::MAX_PINNED_HDRS) {
        tl_point_ctx.pinned_hdrs[tl_point_ctx.pinned_hdr_count++] = tensor_hdr;
        g_point_coordinator.hookpoint_auto_pinned_tensors.fetch_add(1, std::memory_order_relaxed);
        g_point_coordinator.hookpoint_active_pinned.fetch_add(1, std::memory_order_relaxed);
    }
}

MSIMD_API void sf_gc_hookpoint_exit(void* tensor_hdr) {
    if (tl_point_ctx.nesting_depth > 0) {
        tl_point_ctx.nesting_depth--;
    }
    g_point_coordinator.hookpoint_exit_count.fetch_add(1, std::memory_order_relaxed);
    if (tensor_hdr) {
        for (uint32_t i = 0; i < tl_point_ctx.pinned_hdr_count; ++i) {
            if (tl_point_ctx.pinned_hdrs[i] == tensor_hdr) {
                tl_point_ctx.pinned_hdrs[i] = tl_point_ctx.pinned_hdrs[--tl_point_ctx.pinned_hdr_count];
                g_point_coordinator.hookpoint_active_pinned.fetch_sub(1, std::memory_order_relaxed);
                break;
            }
        }
    }
}

MSIMD_API void sf_gc_pausepoint_poll(void) {
    ++tl_point_ctx.tl_pausepoint_polls;
    g_point_coordinator.pausepoint_polls.fetch_add(1, std::memory_order_relaxed);
    if (g_point_coordinator.pausepoint_gate.load(std::memory_order_relaxed)) {
        g_point_coordinator.pausepoint_preemptions.fetch_add(1, std::memory_order_relaxed);
    }
}

MSIMD_API void sf_gc_pausepoint_set_gate(bool armed) {
    g_point_coordinator.pausepoint_gate.store(armed, std::memory_order_release);
    g_stream_pause_gate.store(armed, std::memory_order_release);
}

MSIMD_API sf_point_coordinator_t* sf_get_point_coordinator(void) {
    return &g_point_coordinator;
}
