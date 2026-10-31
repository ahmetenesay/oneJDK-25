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

#include "gc/redlet/redletPagefile.hpp"
#include <cstring>
#include <cstdio>
#include <memory>
#include <unordered_map>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#endif

sf_os_allocation_registry_t g_sf_os_registry;
std::atomic<bool>           g_sf_enable_huge_pages{ false };
sf_pagefile_t               g_sf_pagefile;

/* ========================================================================= */
/* 1. OS VIRTUAL MEMORY ALLOCATION REGISTRY                                  */
/* ========================================================================= */

void sf_os_allocation_registry_t::register_block(void* addr, size_t bytes, uint32_t tag) {
    if (!addr || bytes == 0) return;
    std::lock_guard<std::recursive_mutex> g(lock);
    for (size_t i = 0; i < MAX_TRACKED_OS_BLOCKS; ++i) {
        if (entries[i].addr == nullptr) {
            entries[i].addr = addr;
            entries[i].bytes = bytes;
            entries[i].tag = tag;
            active_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    overflow_entries.push_back({ addr, bytes, tag });
    active_count.fetch_add(1, std::memory_order_relaxed);
}

void sf_os_allocation_registry_t::unregister_block(void* addr) {
    if (!addr) return;
    std::lock_guard<std::recursive_mutex> g(lock);
    for (size_t i = 0; i < MAX_TRACKED_OS_BLOCKS; ++i) {
        if (entries[i].addr == addr) {
            entries[i].addr = nullptr;
            entries[i].bytes = 0;
            entries[i].tag = 0;
            size_t c = active_count.load(std::memory_order_relaxed);
            if (c > 0) active_count.store(c - 1, std::memory_order_relaxed);
            return;
        }
    }
    for (size_t i = 0; i < overflow_entries.size(); ++i) {
        if (overflow_entries[i].addr == addr) {
            overflow_entries.erase(overflow_entries.begin() + i);
            size_t c = active_count.load(std::memory_order_relaxed);
            if (c > 0) active_count.store(c - 1, std::memory_order_relaxed);
            return;
        }
    }
}

size_t sf_os_allocation_registry_t::sweep_all() {
    std::lock_guard<std::recursive_mutex> g(lock);
    size_t swept_bytes = 0;
    auto sweep_entry = [&](void* a, size_t b, uint32_t t) {
        if (!a) return;
        if (t == SF_OS_TAG_MMIO) {
#if !defined(_WIN32)
            munmap(a, b);
#endif
        } else if (t == SF_OS_TAG_FILEMAP) {
#ifdef _WIN32
            UnmapViewOfFile(a);
#else
            munmap(a, b);
#endif
        } else {
#ifdef _WIN32
            VirtualFree(a, 0, MEM_RELEASE);
#else
            munmap(a, b);
#endif
        }
        swept_bytes += b;
    };

    for (size_t i = 0; i < MAX_TRACKED_OS_BLOCKS; ++i) {
        if (entries[i].addr != nullptr) {
            void* a = entries[i].addr;
            size_t b = entries[i].bytes;
            uint32_t t = entries[i].tag;
            entries[i].addr = nullptr;
            entries[i].bytes = 0;
            entries[i].tag = 0;
            sweep_entry(a, b, t);
        }
    }
    for (auto& oe : overflow_entries) {
        if (oe.addr != nullptr) {
            sweep_entry(oe.addr, oe.bytes, oe.tag);
        }
    }
    overflow_entries.clear();
    active_count.store(0, std::memory_order_relaxed);
    return swept_bytes;
}

void* sf_os_reserve_memory(size_t bytes) {
    if (bytes == 0) return nullptr;
#ifdef _WIN32
    void* p = nullptr;
    if (g_sf_enable_huge_pages.load(std::memory_order_relaxed) && bytes >= 2 * 1024 * 1024) {
        SIZE_T min_large = GetLargePageMinimum();
        if (min_large > 0) {
            SIZE_T aligned_bytes = (bytes + min_large - 1) & ~(min_large - 1);
            p = VirtualAlloc(nullptr, aligned_bytes, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
            if (p) {
                bytes = aligned_bytes;
            }
        }
    }
    if (!p) {
        p = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    }
#else
    int map_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_HUGETLB)
    if (g_sf_enable_huge_pages.load(std::memory_order_relaxed) && bytes >= 2 * 1024 * 1024) {
        map_flags |= MAP_HUGETLB;
    }
#endif
    void* p = mmap(nullptr, bytes, PROT_NONE, map_flags, -1, 0);
#if defined(MAP_HUGETLB)
    if (p == MAP_FAILED && (map_flags & MAP_HUGETLB)) {
        p = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
#endif
    if (p == MAP_FAILED) p = nullptr;
#endif
    if (p) {
        g_sf_os_registry.register_block(p, bytes, SF_OS_TAG_VIRTUAL);
    }
    return p;
}

bool sf_os_commit_memory(void* addr, size_t bytes) {
    if (!addr || bytes == 0) return false;
#ifdef _WIN32
    return VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#elif defined(__linux__)
    bool ok = (mprotect(addr, bytes, PROT_READ | PROT_WRITE) == 0);
    if (ok && g_sf_enable_huge_pages.load(std::memory_order_relaxed)) {
#if defined(MADV_HUGEPAGE)
        madvise(addr, bytes, MADV_HUGEPAGE);
#endif
    }
    return ok;
#else
    return mprotect(addr, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

void sf_os_decommit_memory(void* addr, size_t bytes) {
    if (!addr || bytes == 0) return;
#ifdef _WIN32
    VirtualFree(addr, bytes, MEM_DECOMMIT);
#elif defined(__linux__)
    madvise(addr, bytes, MADV_DONTNEED);
#else
    madvise(addr, bytes, MADV_FREE);
#endif
}

void sf_os_release_memory(void* addr, size_t bytes) {
    if (!addr) return;
    g_sf_os_registry.unregister_block(addr);
#ifdef _WIN32
    (void)bytes;
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, bytes);
#endif
}

/* ========================================================================= */
/* 2. ANONYMOUS PAGEFILE MAPPING                                             */
/* ========================================================================= */

bool sf_anonymous_pagefile_t::init(size_t capacity_bytes) {
    if (capacity_bytes == 0) return false;
    mapped_capacity = (capacity_bytes + 4095ULL) & ~4095ULL;
#ifdef _WIN32
    hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)((uint64_t)mapped_capacity >> 32),
        (DWORD)(mapped_capacity & 0xFFFFFFFFULL), NULL);
    if (!hMap) return false;
    mapped_base = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, mapped_capacity);
    if (!mapped_base) {
        CloseHandle(hMap);
        hMap = NULL;
    } else {
        g_sf_os_registry.register_block(mapped_base, mapped_capacity, SF_OS_TAG_FILEMAP);
    }
    return mapped_base != nullptr;
#else
    mapped_base = mmap(nullptr, mapped_capacity, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool ok = (mapped_base != MAP_FAILED) && (mapped_base != nullptr);
    if (ok) {
        g_sf_os_registry.register_block(mapped_base, mapped_capacity, SF_OS_TAG_FILEMAP);
    } else {
        mapped_base = nullptr;
    }
    return ok;
#endif
}

void sf_anonymous_pagefile_t::destroy() {
    if (!mapped_base) return;
    g_sf_os_registry.unregister_block(mapped_base);
#ifdef _WIN32
    UnmapViewOfFile(mapped_base);
    if (hMap) CloseHandle(hMap);
    hMap = NULL;
#else
    munmap(mapped_base, mapped_capacity);
#endif
    mapped_base = nullptr;
    mapped_capacity = 0;
}

/* ========================================================================= */
/* 3. SHARDED COLD OBJECT METADATA SIDE-TABLE                                */
/* ========================================================================= */

namespace {
struct sf_cold_meta_shard_t {
    std::mutex lock;
    std::unordered_map<const SF_custominiVm_live_header_t*, std::unique_ptr<sf_cold_object_metadata_t>> map;
};

static constexpr size_t SF_COLD_META_SHARDS = 32;
static sf_cold_meta_shard_t g_cold_meta_shards[SF_COLD_META_SHARDS];

static inline size_t sf_cold_shard_idx(const SF_custominiVm_live_header_t* obj) noexcept {
    uintptr_t v = reinterpret_cast<uintptr_t>(obj) >> 6;
    v ^= (v >> 16);
    return static_cast<size_t>(v & (SF_COLD_META_SHARDS - 1));
}
} // namespace

sf_cold_object_metadata_t* sf_get_cold_metadata(const SF_custominiVm_live_header_t* obj, bool create_if_missing) {
    if (!obj) return nullptr;
    auto& shard = g_cold_meta_shards[sf_cold_shard_idx(obj)];
    std::lock_guard<std::mutex> g(shard.lock);
    auto it = shard.map.find(obj);
    if (it != shard.map.end()) {
        return it->second.get();
    }
    if (!create_if_missing) return nullptr;
    auto entry = std::make_unique<sf_cold_object_metadata_t>();
    entry->owner = obj;
    sf_cold_object_metadata_t* raw = entry.get();
    shard.map.emplace(obj, std::move(entry));
    return raw;
}

void sf_remove_cold_metadata(const SF_custominiVm_live_header_t* obj) {
    if (!obj) return;
    auto& shard = g_cold_meta_shards[sf_cold_shard_idx(obj)];
    std::lock_guard<std::mutex> g(shard.lock);
    shard.map.erase(obj);
}

const char* sf_get_object_debug_name(const SF_custominiVm_live_header_t* obj) {
    auto* cold = sf_get_cold_metadata(obj, false);
    if (cold && cold->debug_name[0] != '\0') {
        return cold->debug_name;
    }
    return "unnamed_cell";
}

void sf_set_object_debug_name(const SF_custominiVm_live_header_t* obj, const char* name) {
    if (!obj || !name) return;
    auto* cold = sf_get_cold_metadata(obj, true);
    if (cold) {
        std::strncpy(cold->debug_name, name, sizeof(cold->debug_name) - 1);
        cold->debug_name[sizeof(cold->debug_name) - 1] = '\0';
    }
}

/* ========================================================================= */
/* 4. VIRTUAL PAGEFILE SWAP ENGINE                                           */
/* ========================================================================= */

bool sf_pagefile_t::allocate_slot_locked(size_t fp, uint64_t& out_offset) {
    size_t needed_blocks = (fp + PAGEFILE_BLOCK_BYTES - 1) / PAGEFILE_BLOCK_BYTES;
    if (needed_blocks == 0) needed_blocks = 1;

    if (total_blocks > 0 && needed_blocks <= total_blocks && !free_bitmap.empty()) {
        size_t run_start = 0;
        size_t run_len = 0;
        for (size_t b = 0; b < total_blocks; ++b) {
            size_t word_idx = b >> 6;
            uint64_t bit_mask = 1ULL << (b & 63);
            if (run_len == 0 && free_bitmap[word_idx] == ~0ULL) {
                b = ((word_idx + 1) << 6) - 1;
                continue;
            }
            if ((free_bitmap[word_idx] & bit_mask) == 0) {
                if (run_len == 0) run_start = b;
                if (++run_len == needed_blocks) {
                    for (size_t k = run_start; k < run_start + needed_blocks; ++k) {
                        free_bitmap[k >> 6] |= (1ULL << (k & 63));
                    }
                    out_offset = static_cast<uint64_t>(run_start) * PAGEFILE_BLOCK_BYTES;
                    uint64_t end_off = out_offset + needed_blocks * PAGEFILE_BLOCK_BYTES;
                    uint64_t cur_w = write_offset.load(std::memory_order_relaxed);
                    if (end_off > cur_w) write_offset.store(end_off, std::memory_order_relaxed);
                    return true;
                }
            } else {
                run_len = 0;
            }
        }
    }

    uint64_t cur = write_offset.load(std::memory_order_relaxed);
    if (cur + fp > capacity_bytes) return false;
    write_offset.store(cur + fp, std::memory_order_relaxed);
    out_offset = cur;
    return true;
}

void sf_pagefile_t::release_slot_locked(uint64_t slot_off, size_t fp) {
    size_t start_block = static_cast<size_t>(slot_off / PAGEFILE_BLOCK_BYTES);
    size_t needed_blocks = (fp + PAGEFILE_BLOCK_BYTES - 1) / PAGEFILE_BLOCK_BYTES;
    for (size_t k = start_block; k < start_block + needed_blocks && k < total_blocks; ++k) {
        free_bitmap[k >> 6] &= ~(1ULL << (k & 63));
    }
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
    if (file_fd >= 0) {
        (void)::fallocate(file_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                          static_cast<off_t>(slot_off), static_cast<off_t>(fp));
    }
#endif
}

bool sf_pagefile_t::init(size_t max_bytes, SF_custominiVm_paging_policy_t pol) {
    std::lock_guard<std::mutex> g(lock);
    capacity_bytes = max_bytes > 0 ? max_bytes : (256ULL * 1024 * 1024);
    policy = pol;
    write_offset.store(0, std::memory_order_relaxed);
    paged_out_bytes.store(0, std::memory_order_relaxed);
    paged_out_count.store(0, std::memory_order_relaxed);
    total_blocks = (capacity_bytes + PAGEFILE_BLOCK_BYTES - 1) / PAGEFILE_BLOCK_BYTES;
    free_bitmap.assign((total_blocks + 63) / 64, 0ULL);

#ifdef _WIN32
    if (file_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle);
        file_handle = INVALID_HANDLE_VALUE;
    }
    char temp_path[MAX_PATH];
    DWORD tp_len = GetTempPathA(MAX_PATH, temp_path);
    if (tp_len > 0 && tp_len < MAX_PATH) {
        char filename[MAX_PATH];
        static std::atomic<uint32_t> s_pf_seq{ 0 };
        uint32_t seq = s_pf_seq.fetch_add(1, std::memory_order_relaxed);
        std::snprintf(filename, sizeof(filename), "%ssfvm_hidden_pagefile_%lu_%u.tmp",
                      temp_path, static_cast<unsigned long>(GetCurrentProcessId()), seq);
        file_handle = CreateFileA(filename,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            NULL);
    }
    if (file_handle == INVALID_HANDLE_VALUE && !memory_fallback) {
        memory_fallback = VirtualAlloc(NULL, capacity_bytes, MEM_RESERVE, PAGE_READWRITE);
        if (memory_fallback) {
            g_sf_os_registry.register_block(memory_fallback, capacity_bytes, SF_OS_TAG_VIRTUAL);
        }
    }
    return (file_handle != INVALID_HANDLE_VALUE) || (memory_fallback != nullptr);
#else
    if (file_fd >= 0) {
        close(file_fd);
        file_fd = -1;
    }
#if defined(__linux__) && defined(MFD_CLOEXEC)
    file_fd = memfd_create("sfvm_anonymous_pagefile", MFD_CLOEXEC);
#endif
    if (file_fd < 0) {
#if defined(O_TMPFILE)
        file_fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
#endif
    }
    if (file_fd < 0 && !memory_fallback) {
        memory_fallback = mmap(NULL, capacity_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory_fallback && memory_fallback != MAP_FAILED) {
            g_sf_os_registry.register_block(memory_fallback, capacity_bytes, SF_OS_TAG_VIRTUAL);
        } else {
            memory_fallback = nullptr;
        }
    }
    return (file_fd >= 0) || (memory_fallback != nullptr);
#endif
}

void sf_pagefile_t::destroy() {
    std::lock_guard<std::mutex> g(lock);
#ifdef _WIN32
    if (file_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle);
        file_handle = INVALID_HANDLE_VALUE;
    }
    if (memory_fallback) {
        g_sf_os_registry.unregister_block(memory_fallback);
        VirtualFree(memory_fallback, 0, MEM_RELEASE);
        memory_fallback = nullptr;
    }
#else
    if (file_fd >= 0) {
        close(file_fd);
        file_fd = -1;
    }
    if (memory_fallback) {
        g_sf_os_registry.unregister_block(memory_fallback);
        munmap(memory_fallback, capacity_bytes);
        memory_fallback = nullptr;
    }
#endif
    paged_out_bytes.store(0, std::memory_order_relaxed);
    paged_out_count.store(0, std::memory_order_relaxed);
    write_offset.store(0, std::memory_order_relaxed);
    free_bitmap.clear();
    total_blocks = 0;
}

bool sf_pagefile_t::page_out(SF_custominiVm_live_header_t* obj) {
    if (!obj || obj->sfMN != SF_LIVE_MAGIC) return false;
    // Slice views do not own physical payload pages in obj->body()
    if (obj->parent_header.load(std::memory_order_acquire) != nullptr) return false;
    if (obj->is_pinned()) return false;

    auto* cold = sf_get_cold_metadata(obj, true);
    if (!cold) return false;
    // High priority (>= 8) operations stay pinned in SRAM/resident DRAM
    if (cold->group_priority >= 8 || cold->priority >= 8) return false;
    if (cold->child_view_count.load(std::memory_order_acquire) > 0) return false;
    if (cold->is_paged_out.load(std::memory_order_acquire)) return true;

    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    if (fp == 0) return false;

    std::lock_guard<std::mutex> g(lock);
    if (cold->is_paged_out.load(std::memory_order_acquire)) return true;
    if (obj->is_pinned() || cold->child_view_count.load(std::memory_order_acquire) > 0) return false;

    uint64_t slot_off = 0;
    if (!allocate_slot_locked(fp, slot_off)) {
        return false;
    }

    bool written = false;
#ifdef _WIN32
    if (file_handle != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(slot_off);
        if (SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN)) {
            uint8_t* src_ptr = obj->body();
            size_t remaining = fp;
            bool io_ok = true;
            const DWORD MAX_CHUNK = 0x40000000; // 1 GB chunks to prevent 32-bit DWORD truncation
            while (remaining > 0) {
                DWORD chunk_size = (remaining > static_cast<size_t>(MAX_CHUNK)) ? MAX_CHUNK : static_cast<DWORD>(remaining);
                DWORD chunk_written = 0;
                if (!WriteFile(file_handle, src_ptr, chunk_size, &chunk_written, NULL) || chunk_written != chunk_size) {
                    io_ok = false;
                    break;
                }
                src_ptr += chunk_written;
                remaining -= chunk_written;
            }
            if (io_ok) written = true;
        }
    }
#else
    if (file_fd >= 0) {
        uint8_t* src_ptr = obj->body();
        size_t remaining = fp;
        uint64_t cur_off = slot_off;
        bool io_ok = true;
        while (remaining > 0) {
            ssize_t ret = pwrite(file_fd, src_ptr, remaining, static_cast<off_t>(cur_off));
            if (ret <= 0) {
                io_ok = false;
                break;
            }
            src_ptr += ret;
            cur_off += static_cast<uint64_t>(ret);
            remaining -= static_cast<size_t>(ret);
        }
        if (io_ok) written = true;
    }
#endif

    if (!written && memory_fallback) {
        uint8_t* dst = static_cast<uint8_t*>(memory_fallback) + slot_off;
        if (sf_os_commit_memory(dst, fp)) {
            std::memcpy(dst, obj->body(), fp);
            written = true;
        }
    }

    if (written) {
        cold->pagefile_slot_offset = slot_off;
        cold->is_paged_out.store(1, std::memory_order_release);
        paged_out_bytes.fetch_add(fp, std::memory_order_relaxed);
        paged_out_count.fetch_add(1, std::memory_order_relaxed);

        // Decommit interior full 4KB pages of payload without disturbing header or tail canary page
        uintptr_t b_start = reinterpret_cast<uintptr_t>(obj->body());
        uintptr_t p_start = (b_start + 4095ULL) & ~4095ULL;
        uintptr_t p_end   = (b_start + fp) & ~4095ULL;
        if (p_end > p_start) {
            sf_os_decommit_memory(reinterpret_cast<void*>(p_start), p_end - p_start);
        }

        sf_hotlog_emit(SF_Ebin0_PAGEFILE_PAGEOUT, SF_Ebin1_INFO, obj->function_id,
            static_cast<uint16_t>(slot_off / PAGEFILE_BLOCK_BYTES), obj->generation, obj->region,
            obj->current_core.load(std::memory_order_relaxed), 0,
            static_cast<uint32_t>(fp / 1024), "tensor_paged_out");
        return true;
    }

    release_slot_locked(slot_off, fp);
    return false;
}

bool sf_pagefile_t::page_in(SF_custominiVm_live_header_t* obj) {
    if (!obj || obj->sfMN != SF_LIVE_MAGIC) return false;
    auto* cold = sf_get_cold_metadata(obj, false);
    if (!cold) return true;
    if (!cold->is_paged_out.load(std::memory_order_acquire)) return true;

    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    if (fp == 0) return false;

    std::lock_guard<std::mutex> g(lock);
    if (!cold->is_paged_out.load(std::memory_order_acquire)) return true;

    // Re-commit interior 4KB pages if they were decommitted during page_out
    uintptr_t b_start = reinterpret_cast<uintptr_t>(obj->body());
    uintptr_t p_start = (b_start + 4095ULL) & ~4095ULL;
    uintptr_t p_end   = (b_start + fp) & ~4095ULL;
    if (p_end > p_start) {
        (void)sf_os_commit_memory(reinterpret_cast<void*>(p_start), p_end - p_start);
    }

    bool read_ok = false;
    uint64_t slot_off = cold->pagefile_slot_offset;

#ifdef _WIN32
    if (file_handle != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(slot_off);
        if (SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN)) {
            uint8_t* dst_ptr = obj->body();
            size_t remaining = fp;
            bool io_ok = true;
            const DWORD MAX_CHUNK = 0x40000000;
            while (remaining > 0) {
                DWORD chunk_size = (remaining > static_cast<size_t>(MAX_CHUNK)) ? MAX_CHUNK : static_cast<DWORD>(remaining);
                DWORD chunk_read = 0;
                if (!ReadFile(file_handle, dst_ptr, chunk_size, &chunk_read, NULL) || chunk_read != chunk_size) {
                    io_ok = false;
                    break;
                }
                dst_ptr += chunk_read;
                remaining -= chunk_read;
            }
            if (io_ok) read_ok = true;
        }
    }
#else
    if (file_fd >= 0) {
        uint8_t* dst_ptr = obj->body();
        size_t remaining = fp;
        uint64_t cur_off = slot_off;
        bool io_ok = true;
        while (remaining > 0) {
            ssize_t ret = pread(file_fd, dst_ptr, remaining, static_cast<off_t>(cur_off));
            if (ret <= 0) {
                io_ok = false;
                break;
            }
            dst_ptr += ret;
            cur_off += static_cast<uint64_t>(ret);
            remaining -= static_cast<size_t>(ret);
        }
        if (io_ok) read_ok = true;
    }
#endif

    if (!read_ok && memory_fallback) {
        uint8_t* src = static_cast<uint8_t*>(memory_fallback) + slot_off;
        std::memcpy(obj->body(), src, fp);
        read_ok = true;
    }

    if (read_ok) {
        obj->writeTailCanary();
        cold->is_paged_out.store(0, std::memory_order_release);
        size_t pob = paged_out_bytes.load(std::memory_order_relaxed);
        if (pob >= fp) paged_out_bytes.fetch_sub(fp, std::memory_order_relaxed);
        size_t poc = paged_out_count.load(std::memory_order_relaxed);
        if (poc > 0) paged_out_count.fetch_sub(1, std::memory_order_relaxed);
        release_slot_locked(slot_off, fp);
        if (paged_out_count.load(std::memory_order_relaxed) == 0) {
            write_offset.store(0, std::memory_order_relaxed);
        }
        sf_hotlog_emit(SF_Ebin0_PAGEFILE_PAGEIN, SF_Ebin1_INFO, obj->function_id,
            static_cast<uint16_t>(slot_off / PAGEFILE_BLOCK_BYTES), obj->generation, obj->region,
            obj->current_core.load(std::memory_order_relaxed), 0,
            static_cast<uint32_t>(fp / 1024), "tensor_paged_in");
        return true;
    }
    return false;
}

bool sf_pagefile_t::prefetch_async(SF_custominiVm_live_header_t* obj) {
    if (!obj || obj->sfMN != SF_LIVE_MAGIC) return false;
    auto* cold = sf_get_cold_metadata(obj, false);
    if (!cold || !cold->is_paged_out.load(std::memory_order_acquire)) return true;
#if defined(__linux__) && defined(POSIX_FADV_WILLNEED)
    size_t fp = obj->memory_footprint.load(std::memory_order_relaxed);
    uint64_t slot_off = cold->pagefile_slot_offset;
    if (file_fd >= 0 && fp > 0) {
        (void)posix_fadvise(file_fd, static_cast<off_t>(slot_off), static_cast<off_t>(fp), POSIX_FADV_WILLNEED);
    }
#endif
    return page_in(obj);
}

/* ========================================================================= */
/* 5. C ABI EXPORTS                                                          */
/* ========================================================================= */

extern "C" {

MSIMD_API int SF_custominiVm_pagefile_init(size_t max_bytes, SF_custominiVm_paging_policy_t policy) {
    return g_sf_pagefile.init(max_bytes, policy) ? 0 : -1;
}

MSIMD_API int SF_custominiVm_page_out(SF_custominiVm_tensor_t* tensor) {
    if (SacFor_Gearkit_GetMode() == MSIMD_PROFILE_SAFETY_CRITICAL) return -1;
    if (!tensor || !tensor->data) return -1;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return -1;
    return g_sf_pagefile.page_out(obj) ? 0 : -1;
}

MSIMD_API int SF_custominiVm_page_in(SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return -1;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return -1;
    auto* p = static_cast<SF_custominiVm_live_header_t*>(obj->parent_header.load(std::memory_order_acquire));
    if (p) {
        (void)g_sf_pagefile.page_in(p);
    }
    return g_sf_pagefile.page_in(obj) ? 0 : -1;
}

MSIMD_API int SF_custominiVm_page_prefetch(SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return -1;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return -1;
    auto* p = static_cast<SF_custominiVm_live_header_t*>(obj->parent_header.load(std::memory_order_acquire));
    if (p) {
        (void)g_sf_pagefile.prefetch_async(p);
    }
    return g_sf_pagefile.prefetch_async(obj) ? 0 : -1;
}

MSIMD_API size_t SF_custominiVm_pagefile_used_bytes(void) {
    return g_sf_pagefile.paged_out_bytes.load(std::memory_order_relaxed);
}

MSIMD_API size_t SF_custominiVm_emergency_os_sweep(void) {
    return g_sf_os_registry.sweep_all();
}

MSIMD_API size_t SF_custominiVm_os_active_allocations(void) {
    return g_sf_os_registry.active_count.load(std::memory_order_relaxed);
}

} // extern "C"
