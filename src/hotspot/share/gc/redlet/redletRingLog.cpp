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

#include "gc/redlet/redletRingLog.hpp"
#include <cstdio>
#include <chrono>

#if defined(_M_X64) || defined(__x86_64__)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace {

static constexpr uint32_t SF_RING_EXT_STRING_SLOTS = 4096u;
static constexpr uint16_t SF_RING_EXT_SLOT_FLAG    = 0x8000u;
static constexpr uint16_t SF_RING_EXT_SLOT_MASK    = 0x0FFFu;

alignas(64) static char g_ring_ext_strings[SF_RING_EXT_STRING_SLOTS][256] = { {0} };
static std::atomic<uint32_t> g_ring_ext_cursor{ 0 };

static constexpr uint32_t SF_PREBOOT_HOTLOG_CAP = 256u;
alignas(64) static SF_custominiVm_hotlog_entry_t g_preboot_hotlog[SF_PREBOOT_HOTLOG_CAP] = {};
static std::atomic<uint32_t> g_preboot_hotlog_count{ 0 };

static std::atomic<sacculus_hotlog_t*>     g_active_hotlog{ nullptr };
static std::atomic<sacculus_coldlog_t*>    g_active_coldlog{ nullptr };
static std::atomic<uint32_t>               g_telemetry_filter_mask{ 0xFFFFFFFFu };
static std::atomic<sf_raw_log_stream_fn_t> g_raw_log_stream_fn{ nullptr };
static std::atomic<void*>                  g_raw_log_stream_user{ nullptr };
static std::atomic<int>                    g_console_mirroring{ 0 };

static inline uint64_t sf_ring_now_ns() noexcept {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    return ns > 0 ? static_cast<uint64_t>(ns) : 1ULL;
}

static inline uint64_t sf_ring_rdtsc() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return static_cast<uint64_t>(__rdtsc());
#else
    return sf_ring_now_ns();
#endif
}

static void sf_ring_broadcast_raw_line(const char* text, bool is_err = false) {
    if (!text) return;
    size_t len = std::strlen(text);
    auto fn = g_raw_log_stream_fn.load(std::memory_order_acquire);
    if (fn) {
        void* u = g_raw_log_stream_user.load(std::memory_order_acquire);
        fn(text, len, u);
    }
    if (g_console_mirroring.load(std::memory_order_acquire)) {
        FILE* out = is_err ? stderr : stdout;
        std::fputs(text, out);
        if (len == 0 || text[len - 1] != '\n') {
            std::fputc('\n', out);
        }
        std::fflush(out);
    }
}

} // namespace

uint16_t sf_ring_store_ext_string(const char* full_text) noexcept {
    if (!full_text || !full_text[0]) return 0;
    uint32_t idx = g_ring_ext_cursor.fetch_add(1, std::memory_order_relaxed) & SF_RING_EXT_SLOT_MASK;
    size_t len = std::strlen(full_text);
    while (len > 0 && (full_text[len - 1] == '\n' || full_text[len - 1] == '\r')) {
        --len;
    }
    if (len >= 255) len = 255;
    std::memcpy(g_ring_ext_strings[idx], full_text, len);
    g_ring_ext_strings[idx][len] = '\0';
    return static_cast<uint16_t>(SF_RING_EXT_SLOT_FLAG | idx);
}

const char* sf_ring_lookup_ext_string(uint16_t slot) noexcept {
    if ((slot & SF_RING_EXT_SLOT_FLAG) == 0) return nullptr;
    uint32_t idx = slot & SF_RING_EXT_SLOT_MASK;
    return g_ring_ext_strings[idx][0] ? g_ring_ext_strings[idx] : nullptr;
}

void sf_ring_bind_active_logs(sacculus_hotlog_t* hl, sacculus_coldlog_t* cl) {
    g_active_hotlog.store(hl, std::memory_order_release);
    g_active_coldlog.store(cl, std::memory_order_release);
    if (hl) {
        (void)sf_flush_preboot_hotlog(hl, cl);
    }
}

sacculus_hotlog_t* sf_ring_get_active_hotlog() noexcept {
    return g_active_hotlog.load(std::memory_order_acquire);
}

sacculus_coldlog_t* sf_ring_get_active_coldlog() noexcept {
    return g_active_coldlog.load(std::memory_order_acquire);
}

void sf_ring_hotlog_write(sacculus_hotlog_t* log,
                          SF_custominiVm_event_category_e ev,
                          SF_custominiVm_severity_e sev,
                          uint32_t fn_id, uint16_t slot, uint8_t gen, uint8_t reg,
                          uint16_t core, uint64_t sweep_no, uint32_t heap_kb,
                          const char* msg,
                          bool broadcast_raw) {
    if (!log) return;
    uint32_t cat_bit = 1u << (static_cast<uint8_t>(ev) >> 4);
    if (static_cast<uint8_t>(sev) < static_cast<uint8_t>(SF_Ebin1_ERROR) &&
        (g_telemetry_filter_mask.load(std::memory_order_relaxed) & cat_bit) == 0) {
        return;
    }

    if (msg && std::strlen(msg) >= sizeof(SF_custominiVm_hotlog_entry_t::msg) &&
        (slot & SF_RING_EXT_SLOT_FLAG) == 0 && slot == 0) {
        slot = sf_ring_store_ext_string(msg);
    }

    SF_custominiVm_hotlog_entry_t e{};
    e.ts_ns        = sf_ring_now_ns();
    e.tsc          = sf_ring_rdtsc();
    e.gc_sweep_no  = sweep_no;
    e.fn_id        = fn_id;
    e.heap_used_kb = heap_kb;
    e.slot         = slot;
    e.current_core = core;
    e.generation   = gen;
    e.region       = reg;
    e.event        = static_cast<uint8_t>(ev);
    e.severity     = static_cast<uint8_t>(sev);
    if (msg) {
        size_t len = std::strlen(msg);
        if (len > sizeof(e.msg) - 1) len = sizeof(e.msg) - 1;
        std::memcpy(e.msg, msg, len);
        e.msg[len] = '\0';
    }
    e.ready = 1;
    log->push(e);

    if (broadcast_raw) {
        char log_buf[512];
        const char* col     = sf_severity_ansi_color(e.severity);
        const char* rst     = "\033[0m";
        const char* sev_str = sf_severity_str(e.severity);
        const char* cat_str = sf_event_category_name(e.event);
        const char* ext_str = sf_ring_lookup_ext_string(e.slot);
        const char* display = ext_str ? ext_str : (msg ? msg : e.msg);
        std::snprintf(log_buf, sizeof(log_buf),
            "\033[1;36m[0x%04X]\033[0m %s[%-5s]%s [%-18s] core=%-2u fn=%-4u sweep=%-4llu heap=%-6uKB msg=\"%s\"",
            static_cast<unsigned int>(e.event),
            col, sev_str, rst, cat_str,
            static_cast<unsigned int>(e.current_core),
            static_cast<unsigned int>(e.fn_id),
            static_cast<unsigned long long>(e.gc_sweep_no),
            static_cast<unsigned int>(e.heap_used_kb),
            display);
        sf_ring_broadcast_raw_line(log_buf, e.severity >= 3);
    }
}

void sf_ring_coldlog_write(sacculus_coldlog_t* log,
                           SF_custominiVm_event_category_e ev,
                           SF_custominiVm_severity_e sev,
                           uint32_t fn_id,
                           const char* msg,
                           bool broadcast_raw) {
    if (!log) return;
    uint32_t cat_bit = 1u << (static_cast<uint8_t>(ev) >> 4);
    if (static_cast<uint8_t>(sev) < static_cast<uint8_t>(SF_Ebin1_ERROR) &&
        (g_telemetry_filter_mask.load(std::memory_order_relaxed) & cat_bit) == 0) {
        return;
    }

    SF_custominiVm_coldlog_entry_t e{};
    e.ts_ns    = sf_ring_now_ns();
    e.fn_id    = fn_id;
    e.event    = static_cast<uint8_t>(ev);
    e.severity = static_cast<uint8_t>(sev);
    if (msg) {
        size_t len = std::strlen(msg);
        if (len > sizeof(e.msg) - 1) len = sizeof(e.msg) - 1;
        std::memcpy(e.msg, msg, len);
        e.msg[len] = '\0';
    }
    log->push(e);

    if (broadcast_raw) {
        char log_buf[320];
        const char* col     = sf_severity_ansi_color(e.severity);
        const char* rst     = "\033[0m";
        const char* sev_str = sf_severity_str(e.severity);
        const char* cat_str = sf_event_category_name(e.event);
        std::snprintf(log_buf, sizeof(log_buf),
            "\033[1;35m[0x%04X]\033[0m %s[%-5s]%s [%-18s] fn=%-4u msg=\"%s\"",
            static_cast<unsigned int>(e.event),
            col, sev_str, rst, cat_str,
            static_cast<unsigned int>(e.fn_id),
            msg ? msg : e.msg);
        sf_ring_broadcast_raw_line(log_buf, e.severity >= 3);
    }
}

void sf_ring_unified_emit(SF_custominiVm_event_category_e ev,
                          SF_custominiVm_severity_e sev,
                          uint32_t fn_id,
                          const char* short_tag,
                          const char* full_text,
                          bool broadcast_raw) {
    uint16_t ext_slot = sf_ring_store_ext_string(full_text);
    const char* tag = (short_tag && short_tag[0]) ? short_tag : (full_text ? full_text : "sfvm_log");
    auto* hl = g_active_hotlog.load(std::memory_order_acquire);
    auto* cl = g_active_coldlog.load(std::memory_order_acquire);

    if (hl) {
        sf_ring_hotlog_write(hl, ev, sev, fn_id, ext_slot, 0, 0, 0, 0, 0, tag, broadcast_raw);
    } else {
        uint32_t pb_idx = g_preboot_hotlog_count.fetch_add(1, std::memory_order_relaxed);
        if (pb_idx < SF_PREBOOT_HOTLOG_CAP) {
            SF_custominiVm_hotlog_entry_t& e = g_preboot_hotlog[pb_idx];
            e.ts_ns        = sf_ring_now_ns();
            e.tsc          = sf_ring_rdtsc();
            e.gc_sweep_no  = 0;
            e.fn_id        = fn_id;
            e.heap_used_kb = 0;
            e.slot         = ext_slot;
            e.current_core = 0;
            e.generation   = 0;
            e.region       = 0;
            e.event        = static_cast<uint8_t>(ev);
            e.severity     = static_cast<uint8_t>(sev);
            size_t len = std::strlen(tag);
            if (len > sizeof(e.msg) - 1) len = sizeof(e.msg) - 1;
            std::memcpy(e.msg, tag, len);
            e.msg[len] = '\0';
            e.ready = 1;
        }
        if (broadcast_raw && full_text) {
            sf_ring_broadcast_raw_line(full_text, static_cast<uint8_t>(sev) >= 2);
        }
    }

    if (cl && static_cast<uint8_t>(sev) >= 1) {
        sf_ring_coldlog_write(cl, ev, sev, fn_id, full_text ? full_text : tag, false);
    }
}

size_t sf_flush_preboot_hotlog(sacculus_hotlog_t* hl, sacculus_coldlog_t* cl) {
    if (!hl) return 0;
    uint32_t cnt = g_preboot_hotlog_count.exchange(0, std::memory_order_acq_rel);
    if (cnt > SF_PREBOOT_HOTLOG_CAP) cnt = SF_PREBOOT_HOTLOG_CAP;
    size_t flushed = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        auto& e = g_preboot_hotlog[i];
        if (e.ready) {
            hl->push(e);
            ++flushed;
            if (cl && e.severity >= 1) {
                const char* ext_s = sf_ring_lookup_ext_string(e.slot);
                SF_custominiVm_coldlog_entry_t ce{};
                ce.ts_ns    = e.ts_ns;
                ce.fn_id    = e.fn_id;
                ce.event    = e.event;
                ce.severity = e.severity;
                const char* src_m = ext_s ? ext_s : e.msg;
                size_t len = std::strlen(src_m);
                if (len > sizeof(ce.msg) - 1) len = sizeof(ce.msg) - 1;
                std::memcpy(ce.msg, src_m, len);
                ce.msg[len] = '\0';
                cl->push(ce);
            }
            e.ready = 0;
        }
    }
    return flushed;
}

namespace {
struct sf_hot_to_cold_ctx_t {
    sacculus_coldlog_t* cl{ nullptr };
    size_t              count{ 0 };
};

static void sf_hot_to_cold_cb(const SF_custominiVm_hotlog_entry_t* e, void* user) {
    auto* ctx = static_cast<sf_hot_to_cold_ctx_t*>(user);
    if (!ctx || !ctx->cl || !e) return;
    const char* ext_s = sf_ring_lookup_ext_string(e->slot);
    sf_ring_coldlog_write(ctx->cl,
                          static_cast<SF_custominiVm_event_category_e>(e->event),
                          static_cast<SF_custominiVm_severity_e>(e->severity),
                          e->fn_id,
                          ext_s ? ext_s : e->msg,
                          false);
    ctx->count++;
}
} // namespace

size_t drain_hotlog_to_coldlog(sacculus_hotlog_t* hl, sacculus_coldlog_t* cl) {
    if (!hl || !cl) return 0;
    sf_hot_to_cold_ctx_t ctx{ cl, 0 };
    hl->drain(sf_hot_to_cold_cb, &ctx);
    return ctx.count;
}

/* ========================================================================= */
/* 4. C ABI EXPORTS                                                          */
/* ========================================================================= */

extern "C" {

MSIMD_API void SF_custominiVm_set_raw_log_stream(sf_raw_log_stream_fn_t fn, void* user_data) {
    g_raw_log_stream_user.store(user_data, std::memory_order_release);
    g_raw_log_stream_fn.store(fn, std::memory_order_release);
}

MSIMD_API void SF_custominiVm_set_console_mirroring(int enable) {
    g_console_mirroring.store(enable != 0 ? 1 : 0, std::memory_order_release);
}

MSIMD_API int SF_custominiVm_get_console_mirroring(void) {
    return g_console_mirroring.load(std::memory_order_acquire);
}

MSIMD_API void SF_custominiVm_set_telemetry_filter_mask(uint32_t mask) {
    g_telemetry_filter_mask.store(mask, std::memory_order_release);
}

MSIMD_API uint32_t SF_custominiVm_get_telemetry_filter_mask(void) {
    return g_telemetry_filter_mask.load(std::memory_order_acquire);
}

MSIMD_API size_t SF_custominiVm_hotlog_drain(sf_hotlog_drain_cb_t cb, void* user_data) {
    auto* hl = g_active_hotlog.load(std::memory_order_acquire);
    if (!hl || !cb) return 0;
    return hl->drain(cb, user_data);
}

MSIMD_API size_t SF_custominiVm_coldlog_drain(sf_coldlog_drain_cb_t cb, void* user_data) {
    auto* cl = g_active_coldlog.load(std::memory_order_acquire);
    if (!cl || !cb) return 0;
    return cl->drain(cb, user_data);
}

MSIMD_API size_t SF_custominiVm_drain_hot_to_cold(void) {
    auto* hl = g_active_hotlog.load(std::memory_order_acquire);
    auto* cl = g_active_coldlog.load(std::memory_order_acquire);
    return drain_hotlog_to_coldlog(hl, cl);
}

MSIMD_API size_t SF_custominiVm_export_chrome_trace_json(char* out_buf, size_t buf_cap, size_t max_entries) {
    if (!out_buf || buf_cap < 32) return 0;
    auto* hl = g_active_hotlog.load(std::memory_order_acquire);
    std::vector<SF_custominiVm_hotlog_entry_t> entries;
    if (hl) {
        hl->snapshot_recent(entries, max_entries > 0 ? max_entries : 1024);
    }

    size_t pos = 0;
    int n = std::snprintf(out_buf + pos, buf_cap - pos, "{\"traceEvents\":[");
    if (n <= 0 || static_cast<size_t>(n) >= buf_cap - pos) return 0;
    pos += static_cast<size_t>(n);

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const char* cat = sf_event_category_name(e.event);
        const char* ext = sf_ring_lookup_ext_string(e.slot);
        const char* msg = ext ? ext : e.msg;
        uint64_t ts_us  = e.ts_ns / 1000ULL;

        char item_buf[384];
        int item_len = std::snprintf(item_buf, sizeof(item_buf),
            "%s{\"name\":\"%s\",\"cat\":\"RedletGC\",\"ph\":\"i\",\"ts\":%llu,\"pid\":1,\"tid\":%u,\"args\":{\"fn\":%u,\"sweep\":%llu,\"msg\":\"%s\"}}",
            (i > 0) ? "," : "",
            cat,
            static_cast<unsigned long long>(ts_us),
            static_cast<unsigned int>(e.current_core),
            static_cast<unsigned int>(e.fn_id),
            static_cast<unsigned long long>(e.gc_sweep_no),
            msg ? msg : "");
        if (item_len <= 0 || pos + static_cast<size_t>(item_len) + 4 >= buf_cap) {
            break;
        }
        std::memcpy(out_buf + pos, item_buf, static_cast<size_t>(item_len));
        pos += static_cast<size_t>(item_len);
    }

    if (pos + 3 < buf_cap) {
        out_buf[pos++] = ']';
        out_buf[pos++] = '}';
        out_buf[pos]   = '\0';
    }
    return pos;
}

} // extern "C"
