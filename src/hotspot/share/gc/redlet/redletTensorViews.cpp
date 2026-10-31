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

#include "gc/redlet/redletTensorViews.hpp"
#include "gc/redlet/redletDispatchISA.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#endif

sacculus_folder_tree_t g_sf_vfs_tree;

namespace {

static std::atomic<sacculus_runway_t*> g_bound_runway{ nullptr };
static std::once_flag                  g_fallback_runway_once;
static sacculus_runway_t               g_fallback_runway;
static std::once_flag                  g_default_vfs_once;

static sacculus_runway_t* sf_ensure_active_runway() {
    auto* r = g_bound_runway.load(std::memory_order_acquire);
    if (r) return r;
    std::call_once(g_fallback_runway_once, []() {
        (void)g_fallback_runway.init(64ULL * 1024 * 1024, false);
    });
    return &g_fallback_runway;
}

static void sf_ensure_default_vfs_initialized() {
    std::call_once(g_default_vfs_once, []() {
        if (!g_sf_vfs_tree.root) {
            g_sf_vfs_tree.init();
        }
    });
}

/* ── VFS Strict Validation & Panic Engine State ── */
static std::atomic<bool>     g_sf_vfs_suppress_abort{ false };
static std::atomic<uint64_t> g_sf_vfs_panic_count{ 0 };
static sf_vfs_panic_hook_t   g_sf_vfs_panic_hook{ nullptr };
static void*                 g_sf_vfs_panic_hook_user{ nullptr };
static std::mutex            g_sf_vfs_panic_mutex;
static char                  g_sf_vfs_last_panic_path[256] = { 0 };
static char                  g_sf_vfs_last_panic_op[256]   = { 0 };

static void sf_vfs_trigger_panic_internal(const char* missing_path, const char* operation_context) {
    const char* path_str = missing_path ? missing_path : "<null>";
    const char* op_str   = operation_context ? operation_context : "<unknown>";

    g_sf_vfs_panic_count.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_sf_vfs_panic_mutex);
        std::strncpy(g_sf_vfs_last_panic_path, path_str, sizeof(g_sf_vfs_last_panic_path) - 1);
        g_sf_vfs_last_panic_path[sizeof(g_sf_vfs_last_panic_path) - 1] = '\0';
        std::strncpy(g_sf_vfs_last_panic_op, op_str, sizeof(g_sf_vfs_last_panic_op) - 1);
        g_sf_vfs_last_panic_op[sizeof(g_sf_vfs_last_panic_op) - 1] = '\0';
    }

    char vfs_panic_line[512];
    std::snprintf(vfs_panic_line, sizeof(vfs_panic_line),
        "[SFVM VFS PANIC] Missing VFS Branch: '%s' (Operation: '%s') - Safe termination initiated!",
        path_str, op_str);
    sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_FATAL, 0, "VFS_PANIC", vfs_panic_line);
    sf_ring_unified_emit(SF_Ebin0_ERROR, SF_Ebin1_FATAL, 0, "VFS_PANIC", vfs_panic_line, false);

    auto* hl = sf_ring_get_active_hotlog();
    auto* cl = sf_ring_get_active_coldlog();
    if (hl && cl) {
        (void)drain_hotlog_to_coldlog(hl, cl);
    }

    if (g_sf_vfs_panic_hook) {
        g_sf_vfs_panic_hook(path_str, op_str, g_sf_vfs_panic_hook_user);
    }

    if (g_sf_vfs_suppress_abort.load(std::memory_order_relaxed)) {
        return;
    }

    (void)SF_custominiVm_emergency_os_sweep();
    std::abort();
}

static void sf_compute_default_strides(const SF_custominiVm_tensor_t* t, int64_t out_strides[8]) noexcept {
    std::memset(out_strides, 0, sizeof(int64_t) * 8);
    if (!t || t->ndim <= 0) return;
    int nd = (t->ndim > 8) ? 8 : t->ndim;
    int64_t s = static_cast<int64_t>(sf_dtype_size(static_cast<sfDType>(t->dtype)));
    for (int i = nd - 1; i >= 0; --i) {
        out_strides[i] = s;
        s *= static_cast<int64_t>(t->shape[i] > 0 ? t->shape[i] : 1);
    }
}

/* ── SBC/GPIO & Ring-0 State ── */
static SF_custominiVm_gpio_desc_t    g_sf_gpio_desc{};
static std::atomic<bool>             g_sf_gpio_ready{ false };
static std::atomic<uint64_t>         g_sf_gpio_simulated_pins{ 0 };
static SF_custominiVm_ring0_status_t g_sf_ring0_status{};

} // namespace

void sf_tensor_views_bind_runway(sacculus_runway_t* runway) {
    g_bound_runway.store(runway, std::memory_order_release);
}

sacculus_runway_t* sf_tensor_views_get_runway() {
    return sf_ensure_active_runway();
}

SF_custominiVm_tensor_t sf_tensor_make(void* data, const int32_t* shape, int ndim, sfDType dtype) noexcept {
    SF_custominiVm_tensor_t t{};
    t.data  = data;
    t.ndim  = (ndim > 8) ? 8 : (ndim < 0 ? 0 : ndim);
    t.dtype = static_cast<uint8_t>(dtype);
    size_t elems = 1;
    for (int i = 0; i < t.ndim; ++i) {
        int32_t d = (shape && shape[i] > 0) ? shape[i] : 1;
        t.shape[i] = d;
        elems *= static_cast<size_t>(d);
    }
    t.byte_size = (t.ndim > 0) ? (elems * sf_dtype_size(dtype)) : 0;
    return t;
}

/* ========================================================================= */
/* 1. QUOTA-ENFORCED HIERARCHICAL VFS FOLDER TREE                            */
/* ========================================================================= */

sf_folder_node_t::~sf_folder_node_t() {
    std::vector<sf_folder_node_t*> stack = children;
    children.clear();
    while (!stack.empty()) {
        sf_folder_node_t* node = stack.back();
        stack.pop_back();
        if (node) {
            for (auto* c : node->children) {
                stack.push_back(c);
            }
            node->children.clear();
            delete node;
        }
    }
}

void sacculus_folder_tree_t::init() {
    {
        std::lock_guard<std::mutex> g(tree_lock);
        if (root) {
            delete root;
            root = nullptr;
        }
        root = new (std::nothrow) sf_folder_node_t;
        if (root) {
            std::strncpy(root->name, "/", sizeof(root->name) - 1);
            std::strncpy(root->full_path, "/", sizeof(root->full_path) - 1);
        }
    }

    mkdir_p("/heap/on_heap/runway/motherline/egg/young/tank");
    mkdir_p("/heap/on_heap/runway/motherline/egg/young/demand");
    mkdir_p("/heap/on_heap/runway/motherline/egg/young/middle");
    mkdir_p("/heap/on_heap/runway/motherline/egg/young/hotfield");
    mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/tank");
    mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/demand");
    mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/middle");
    mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/hotfield");
    mkdir_p("/heap/on_heap/runway/motherline/egg/senior/tank");
    mkdir_p("/heap/on_heap/runway/motherline/egg/senior/demand");
    mkdir_p("/heap/on_heap/runway/motherline/egg/senior/middle");
    mkdir_p("/heap/on_heap/runway/motherline/egg/senior/hotfield");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/tank");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/demand");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/middle");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/hotfield");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/tank");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/demand");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/middle");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/hotfield");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/tank");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/demand");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/middle");
    mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/hotfield");
    mkdir_p("/heap/on_heap/runway/pitstop");
    mkdir_p("/heap/on_heap/runway/falline");
    mkdir_p("/heap/on_heap/puzzle_arena/stagnant");
    mkdir_p("/heap/on_heap/puzzle_arena/eval");
    mkdir_p("/heap/on_heap/landfill/trash_pit/matured");
    mkdir_p("/heap/on_heap/landfill/trash_pit/rehab");
    mkdir_p("/heap/on_heap/landfill/header_pane");
    mkdir_p("/heap/off_heap/code_boot/rawcode_cell");
    mkdir_p("/heap/off_heap/code_boot/processed_code_chunk");
    mkdir_p("/heap/off_heap/log_boot/hotlog");
    mkdir_p("/heap/off_heap/log_boot/coldlog");
    mkdir_p("/heap/off_heap/reserved_boot");
}

void sacculus_folder_tree_t::destroy() {
    std::lock_guard<std::mutex> g(tree_lock);
    delete root;
    root = nullptr;
}

void sacculus_folder_tree_t::split_path(const char* path, std::vector<std::string>& parts) {
    if (!path) return;
    const char* p = path;
    while (*p == '/' || *p == '\\') ++p;
    while (*p) {
        const char* start = p;
        while (*p && *p != '/' && *p != '\\') ++p;
        if (p > start) {
            parts.emplace_back(start, static_cast<size_t>(p - start));
        }
        while (*p == '/' || *p == '\\') ++p;
    }
}

sf_folder_node_t* sacculus_folder_tree_t::_mkdir_p_unlocked(const char* path, size_t quota_bytes) {
    if (!path || !root) return nullptr;
    std::vector<std::string> parts;
    split_path(path, parts);
    if (parts.empty()) {
        if (quota_bytes > 0) root->quota_bytes = quota_bytes;
        return root;
    }

    sf_folder_node_t* curr = root;
    std::string cur_path;
    for (const auto& part : parts) {
        cur_path += "/" + part;
        sf_folder_node_t* found = nullptr;
        for (auto* child : curr->children) {
            if (std::strcmp(child->name, part.c_str()) == 0) {
                found = child;
                break;
            }
        }
        if (!found) {
            found = new (std::nothrow) sf_folder_node_t;
            if (!found) return nullptr;
            std::strncpy(found->name, part.c_str(), sizeof(found->name) - 1);
            found->name[sizeof(found->name) - 1] = '\0';
            std::strncpy(found->full_path, cur_path.c_str(), sizeof(found->full_path) - 1);
            found->full_path[sizeof(found->full_path) - 1] = '\0';
            found->parent = curr;
            curr->children.push_back(found);
        }
        curr = found;
    }
    if (curr && quota_bytes > 0) {
        curr->quota_bytes = quota_bytes;
    }
    return curr;
}

sf_folder_node_t* sacculus_folder_tree_t::mkdir_p(const char* path, size_t quota_bytes) {
    if (!path) return nullptr;
    std::lock_guard<std::mutex> g(tree_lock);
    if (!root) {
        root = new (std::nothrow) sf_folder_node_t;
        if (root) {
            std::strncpy(root->name, "/", sizeof(root->name) - 1);
            std::strncpy(root->full_path, "/", sizeof(root->full_path) - 1);
        }
    }
    return _mkdir_p_unlocked(path, quota_bytes);
}

sf_folder_node_t* sacculus_folder_tree_t::_find_node_unlocked(const char* path) {
    if (!path || !root) return nullptr;
    std::vector<std::string> parts;
    split_path(path, parts);
    if (parts.empty()) return root;

    sf_folder_node_t* curr = root;
    for (const auto& part : parts) {
        sf_folder_node_t* found = nullptr;
        for (auto* child : curr->children) {
            if (std::strcmp(child->name, part.c_str()) == 0) {
                found = child;
                break;
            }
        }
        if (!found) return nullptr;
        curr = found;
    }
    return curr;
}

sf_folder_node_t* sacculus_folder_tree_t::find_node(const char* path) {
    if (!path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "find_node");
        }
        return nullptr;
    }
    std::lock_guard<std::mutex> g(tree_lock);
    sf_folder_node_t* node = _find_node_unlocked(path);
    if (!node && g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
        sf_vfs_trigger_panic_internal(path, "find_node");
    }
    return node;
}

bool sacculus_folder_tree_t::bind_tensor(const char* folder_path, SF_custominiVm_live_header_t* obj) {
    if (!folder_path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "bind_tensor");
        }
        return false;
    }
    if (!obj) return false;

    std::lock_guard<std::mutex> g(tree_lock);
    sf_folder_node_t* node = _find_node_unlocked(folder_path);
    if (!node) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(folder_path, "bind_tensor");
            return false;
        }
        node = _mkdir_p_unlocked(folder_path);
        if (!node) return false;
    }

    size_t footprint = obj->memory_footprint.load(std::memory_order_relaxed);
    if (node->quota_bytes > 0 && (node->used_bytes.load(std::memory_order_relaxed) + footprint > node->quota_bytes)) {
        return false;
    }
    node->objects.push_back(obj);
    node->used_bytes.fetch_add(footprint, std::memory_order_relaxed);
    sf_hotlog_emit(SF_Ebin0_VFS_BIND_TENSOR, SF_Ebin1_TRACE, obj->function_id, 0,
        obj->generation, obj->region, 0, 0, static_cast<uint32_t>(footprint / 1024), node->name);
    return true;
}

size_t sacculus_folder_tree_t::evict_folder(const char* folder_path, sacculus_runway_t* runway) {
    if (!folder_path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "evict_folder");
        }
        return 0;
    }

    std::vector<std::pair<SF_custominiVm_live_header_t*, size_t>> to_free;
    {
        std::lock_guard<std::mutex> g(tree_lock);
        sf_folder_node_t* target = _find_node_unlocked(folder_path);
        if (!target) {
            if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
                sf_vfs_trigger_panic_internal(folder_path, "evict_folder");
            }
            return 0;
        }
        if (!runway) return 0;

        std::vector<sf_folder_node_t*> stack;
        stack.push_back(target);
        while (!stack.empty()) {
            sf_folder_node_t* curr = stack.back();
            stack.pop_back();
            for (auto* child : curr->children) {
                stack.push_back(child);
            }
            for (auto* obj : curr->objects) {
                if (obj) {
                    size_t sz = obj->memory_footprint.load(std::memory_order_relaxed);
                    to_free.push_back({ obj, sz });
                }
            }
            curr->objects.clear();
            curr->used_bytes.store(0, std::memory_order_relaxed);
        }
    }

    size_t freed_total = 0;
    for (auto& item : to_free) {
        sf_remove_cold_metadata(item.first);
        runway->free_cell(item.first->body(), item.second);
        freed_total += item.second;
    }
    if (freed_total > 0 || !to_free.empty()) {
        sf_hotlog_emit(SF_Ebin0_VFS_EVICT_TREE, SF_Ebin1_INFO, 0, static_cast<uint16_t>(to_free.size()),
            0, 0, 0, 0, static_cast<uint32_t>(freed_total / 1024), folder_path);
    }
    return freed_total;
}

size_t sacculus_folder_tree_t::get_used_bytes(const char* folder_path) {
    if (!folder_path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "get_used_bytes");
        }
        return 0;
    }
    std::lock_guard<std::mutex> g(tree_lock);
    sf_folder_node_t* node = _find_node_unlocked(folder_path);
    if (!node) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(folder_path, "get_used_bytes");
        }
        return 0;
    }
    return node->used_bytes.load(std::memory_order_relaxed);
}

bool sacculus_folder_tree_t::unlink_file(const char* file_path, sacculus_runway_t* runway) {
    if (!file_path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "unlink_file");
        }
        return false;
    }
    if (!runway) return false;

    std::vector<std::string> parts;
    split_path(file_path, parts);
    if (parts.empty()) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(file_path, "unlink_file");
        }
        return false;
    }

    std::string filename = parts.back();
    parts.pop_back();
    std::string dir_path;
    for (const auto& p : parts) dir_path += "/" + p;
    if (dir_path.empty()) dir_path = "/";

    std::lock_guard<std::mutex> g(tree_lock);
    sf_folder_node_t* node = _find_node_unlocked(dir_path.c_str());
    if (node) {
        for (auto it = node->objects.begin(); it != node->objects.end(); ++it) {
            SF_custominiVm_live_header_t* obj = *it;
            if (obj && std::strcmp(sf_get_object_debug_name(obj), filename.c_str()) == 0) {
                size_t sz    = obj->memory_footprint.load(std::memory_order_relaxed);
                uint32_t fid = obj->function_id;
                sf_remove_cold_metadata(obj);
                runway->free_cell(obj->body(), sz);
                node->used_bytes.fetch_sub(sz, std::memory_order_relaxed);
                node->objects.erase(it);
                sf_hotlog_emit(SF_Ebin0_VFS_UNLINK_TENSOR, SF_Ebin1_INFO, fid, 0,
                    0, 0, 0, 0, static_cast<uint32_t>(sz / 1024), filename.c_str());
                return true;
            }
        }
    }

    if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
        sf_vfs_trigger_panic_internal(file_path, "unlink_file");
    }
    return false;
}

bool sacculus_folder_tree_t::stat_path(const char* path, SF_custominiVm_vfs_stat_t* out_stat) {
    if (!path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "vfs_stat");
        }
        return false;
    }
    if (!out_stat) return false;
    std::memset(out_stat, 0, sizeof(*out_stat));

    std::lock_guard<std::mutex> g(tree_lock);
    sf_folder_node_t* node = _find_node_unlocked(path);
    if (node) {
        out_stat->inode_no    = reinterpret_cast<uint64_t>(node);
        out_stat->is_dir      = 1;
        out_stat->quota_bytes = node->quota_bytes;
        out_stat->used_bytes  = node->used_bytes.load(std::memory_order_relaxed);
        out_stat->size_bytes  = out_stat->used_bytes;
        out_stat->child_count = static_cast<uint32_t>(node->children.size() + node->objects.size());
        return true;
    }

    std::vector<std::string> parts;
    split_path(path, parts);
    if (parts.empty()) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(path, "vfs_stat");
        }
        return false;
    }

    std::string filename = parts.back();
    parts.pop_back();
    std::string dir_path;
    for (const auto& p : parts) dir_path += "/" + p;
    if (dir_path.empty()) dir_path = "/";

    sf_folder_node_t* pnode = _find_node_unlocked(dir_path.c_str());
    if (pnode) {
        for (auto* obj : pnode->objects) {
            if (obj && std::strcmp(sf_get_object_debug_name(obj), filename.c_str()) == 0) {
                out_stat->inode_no   = reinterpret_cast<uint64_t>(obj);
                out_stat->is_dir     = 0;
                out_stat->size_bytes = obj->memory_footprint.load(std::memory_order_relaxed);
                out_stat->used_bytes = out_stat->size_bytes;
                return true;
            }
        }
    }

    if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
        sf_vfs_trigger_panic_internal(path, "vfs_stat");
    }
    return false;
}

size_t sacculus_folder_tree_t::snapshot_path(const char* path, void* out_buf, size_t cap) {
    if (!path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "snapshot_path");
        }
        return 0;
    }
    if (!out_buf || cap == 0) return 0;

    std::vector<std::string> parts;
    split_path(path, parts);
    if (parts.empty()) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(path, "snapshot_path");
        }
        return 0;
    }

    std::string filename = parts.back();
    parts.pop_back();
    std::string dir_path;
    for (const auto& p : parts) dir_path += "/" + p;
    if (dir_path.empty()) dir_path = "/";

    SF_custominiVm_live_header_t* target_obj = nullptr;
    {
        std::lock_guard<std::mutex> g(tree_lock);
        sf_folder_node_t* pnode = _find_node_unlocked(dir_path.c_str());
        if (pnode) {
            for (auto* obj : pnode->objects) {
                if (obj && std::strcmp(sf_get_object_debug_name(obj), filename.c_str()) == 0) {
                    target_obj = obj;
                    break;
                }
            }
        }
    }
    if (!target_obj) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(path, "snapshot_path");
        }
        return 0;
    }

    if (!target_obj->try_pin()) return 0;
    (void)g_sf_pagefile.page_in(target_obj);
    size_t sz = target_obj->memory_footprint.load(std::memory_order_relaxed);
    size_t to_copy = (sz < cap) ? sz : cap;
    std::memcpy(out_buf, target_obj->resolve_active_body(), to_copy);
    target_obj->unpin();
    return to_copy;
}

void sacculus_folder_tree_t::list_dir(const char* path, void (*cb)(const char*, int, size_t, void*), void* user) {
    if (!path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "list_dir");
        }
        return;
    }
    if (!cb) return;

    std::lock_guard<std::mutex> g(tree_lock);
    sf_folder_node_t* node = _find_node_unlocked(path);
    if (!node) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal(path, "list_dir");
        }
        return;
    }

    for (auto* child : node->children) {
        cb(child->name, 1, child->used_bytes.load(std::memory_order_relaxed), user);
    }
    for (auto* obj : node->objects) {
        if (obj) {
            cb(sf_get_object_debug_name(obj), 0, obj->memory_footprint.load(std::memory_order_relaxed), user);
        }
    }
}

/* ========================================================================= */
/* 2. C ABI EXPORTS                                                          */
/* ========================================================================= */

extern "C" {

MSIMD_API size_t sf_dtype_size(sfDType d) {
    switch (d) {
    case SF_DTYPE_F8:
    case SF_DTYPE_U8:
    case SF_DTYPE_I8:   return 1;
    case SF_DTYPE_F16:
    case SF_DTYPE_I16:
    case SF_DTYPE_U16:  return 2;
    case SF_DTYPE_F32:
    case SF_DTYPE_I32:
    case SF_DTYPE_U32:  return 4;
    case SF_DTYPE_F64:
    case SF_DTYPE_I64:
    case SF_DTYPE_U64:  return 8;
    case SF_DTYPE_F128:
    case SF_DTYPE_I128:
    case SF_DTYPE_U128: return 16;
    case SF_DTYPE_F256:
    case SF_DTYPE_I256:
    case SF_DTYPE_U256: return 32;
    default:            return 0;
    }
}

MSIMD_API sfDType sf_dtype_detect(size_t byte_size, bool is_float, bool is_signed) {
    if (is_float) {
        switch (byte_size) {
        case 1:  return SF_DTYPE_F8;
        case 2:  return SF_DTYPE_F16;
        case 4:  return SF_DTYPE_F32;
        case 8:  return SF_DTYPE_F64;
        case 16: return SF_DTYPE_F128;
        case 32: return SF_DTYPE_F256;
        default: return SF_DTYPE_F32;
        }
    } else {
        switch (byte_size) {
        case 1:  return is_signed ? SF_DTYPE_I8   : SF_DTYPE_U8;
        case 2:  return is_signed ? SF_DTYPE_I16  : SF_DTYPE_U16;
        case 4:  return is_signed ? SF_DTYPE_I32  : SF_DTYPE_U32;
        case 8:  return is_signed ? SF_DTYPE_I64  : SF_DTYPE_U64;
        case 16: return is_signed ? SF_DTYPE_I128 : SF_DTYPE_U128;
        case 32: return is_signed ? SF_DTYPE_I256 : SF_DTYPE_U256;
        default: return is_signed ? SF_DTYPE_I32  : SF_DTYPE_U32;
        }
    }
}

MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_alloc(const int32_t* shape, int ndim, int dtype) {
    SF_custominiVm_tensor_t t{};
    if (!shape || ndim <= 0) return t;
    if (ndim > 8) ndim = 8;

    size_t d_sz = sf_dtype_size(static_cast<sfDType>(dtype));
    if (d_sz == 0) return t;

    size_t elems = 1;
    t.ndim  = ndim;
    t.dtype = static_cast<uint8_t>(dtype);
    for (int i = 0; i < ndim; ++i) {
        int32_t d = shape[i] > 0 ? shape[i] : 1;
        t.shape[i] = d;
        if (elems > SIZE_MAX / static_cast<size_t>(d)) return SF_custominiVm_tensor_t{};
        elems *= static_cast<size_t>(d);
    }
    if (elems > SIZE_MAX / d_sz) return SF_custominiVm_tensor_t{};
    t.byte_size = elems * d_sz;

    auto* runway = sf_ensure_active_runway();
    void* body = runway->allocate_cell(t.byte_size, 0, "tensor_cell");
    if (!body) return SF_custominiVm_tensor_t{};

    auto* hdr = SF_custominiVm_live_header_t::from_body(body);
    if (hdr) {
        auto* cold = sf_get_cold_metadata(hdr, true);
        if (cold) {
            cold->group_priority = sf_get_thread_group_priority();
            cold->priority       = cold->group_priority;
            sf_compute_default_strides(&t, cold->strides);
            std::strncpy(cold->debug_name, "tensor_cell", sizeof(cold->debug_name) - 1);
        }
    }
    t.data = body;
    return t;
}

MSIMD_API void SF_custominiVm_tensor_free(SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return;
    auto* hdr = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (hdr) {
        auto* parent_hdr = static_cast<SF_custominiVm_live_header_t*>(hdr->parent_header.load(std::memory_order_acquire));
        size_t alloc_sz  = (parent_hdr != nullptr) ? 0 : hdr->memory_footprint.load(std::memory_order_relaxed);
        if (parent_hdr) {
            auto* parent_cold = sf_get_cold_metadata(parent_hdr, false);
            if (parent_cold) {
                uint32_t cv = parent_cold->child_view_count.load(std::memory_order_relaxed);
                while (cv > 0 && !parent_cold->child_view_count.compare_exchange_weak(
                    cv, cv - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
            }
        }
        sf_remove_cold_metadata(hdr);
        sf_ensure_active_runway()->free_cell(tensor->data, alloc_sz);
    }
    tensor->data      = nullptr;
    tensor->byte_size = 0;
}

MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_slice(const SF_custominiVm_tensor_t* parent,
                                                              size_t offset_bytes,
                                                              const int32_t* shape, int ndim, int dtype) {
    SF_custominiVm_tensor_t t{};
    if (!parent || !parent->data || ndim <= 0 || !shape) return t;
    if (ndim > 8) ndim = 8;

    size_t d_sz = sf_dtype_size(static_cast<sfDType>(dtype));
    if (d_sz == 0) return t;

    t.ndim  = ndim;
    t.dtype = static_cast<uint8_t>(dtype);
    size_t elems = 1;
    for (int i = 0; i < ndim; ++i) {
        int32_t dim_val = shape[i] > 0 ? shape[i] : 1;
        t.shape[i] = dim_val;
        if (elems > SIZE_MAX / static_cast<size_t>(dim_val)) return SF_custominiVm_tensor_t{};
        elems *= static_cast<size_t>(dim_val);
    }
    if (elems > SIZE_MAX / d_sz) return SF_custominiVm_tensor_t{};
    t.byte_size = elems * d_sz;

    if (t.byte_size > parent->byte_size || offset_bytes > parent->byte_size - t.byte_size) {
        return SF_custominiVm_tensor_t{};
    }

    auto* parent_hdr = SF_custominiVm_live_header_t::from_body(parent->data);
    if (!parent_hdr) {
        t.data = static_cast<uint8_t*>(parent->data) + offset_bytes;
        return t;
    }

    auto* parent_cold = sf_get_cold_metadata(parent_hdr, true);
    if (parent_cold && parent_cold->is_paged_out.load(std::memory_order_acquire)) {
        (void)g_sf_pagefile.page_in(parent_hdr);
    }

    void* view_body = sf_ensure_active_runway()->allocate_cell(0, parent_hdr->function_id, "slice_view");
    if (!view_body) return SF_custominiVm_tensor_t{};

    auto* view_hdr = SF_custominiVm_live_header_t::from_body(view_body);
    if (view_hdr) {
        view_hdr->parent_header.store(parent_hdr, std::memory_order_release);
        view_hdr->slice_byte_offset = offset_bytes;
        view_hdr->memory_footprint.store(t.byte_size, std::memory_order_relaxed);

        auto* view_cold = sf_get_cold_metadata(view_hdr, true);
        if (view_cold) {
            sf_compute_default_strides(&t, view_cold->strides);
            view_cold->is_strided_view = 0;
            if (parent_cold) {
                view_cold->type_sig_hash = parent_cold->type_sig_hash;
            }
            std::strncpy(view_cold->debug_name, "slice_view", sizeof(view_cold->debug_name) - 1);
        }
        if (parent_cold) {
            parent_cold->child_view_count.fetch_add(1, std::memory_order_release);
        }
    }

    t.data = view_body;
    return t;
}

MSIMD_API int SF_custominiVm_tensor_get_strides(const SF_custominiVm_tensor_t* tensor,
                                                int64_t* out_strides_bytes) {
    if (!tensor || !out_strides_bytes) return -1;
    if (tensor->data) {
        auto* hdr = SF_custominiVm_live_header_t::from_body(tensor->data);
        if (hdr) {
            auto* cold = sf_get_cold_metadata(hdr, false);
            if (cold) {
                bool has_nonzero = false;
                for (int i = 0; i < tensor->ndim && i < 8; ++i) {
                    if (cold->strides[i] != 0) has_nonzero = true;
                }
                if (has_nonzero || cold->is_strided_view) {
                    std::memcpy(out_strides_bytes, cold->strides, sizeof(int64_t) * 8);
                    return 0;
                }
            }
        }
    }
    sf_compute_default_strides(tensor, out_strides_bytes);
    return 0;
}

MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_transpose(const SF_custominiVm_tensor_t* parent,
                                                                  int dim0, int dim1) {
    SF_custominiVm_tensor_t t{};
    if (!parent || !parent->data || parent->ndim < 2) return t;
    int nd = (parent->ndim > 8) ? 8 : parent->ndim;
    if (dim0 < 0) dim0 += nd;
    if (dim1 < 0) dim1 += nd;
    if (dim0 < 0 || dim0 >= nd || dim1 < 0 || dim1 >= nd) return t;

    int32_t perm[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    std::swap(perm[dim0], perm[dim1]);
    return SF_custominiVm_tensor_permute(parent, perm, nd);
}

MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_permute(const SF_custominiVm_tensor_t* parent,
                                                                const int32_t* perm, int ndim) {
    SF_custominiVm_tensor_t t{};
    if (!parent || !parent->data || !perm || ndim <= 0 || ndim != parent->ndim || ndim > 8) return t;

    uint32_t seen_mask = 0;
    for (int i = 0; i < ndim; ++i) {
        if (perm[i] < 0 || perm[i] >= ndim) return SF_custominiVm_tensor_t{};
        if (seen_mask & (1u << perm[i])) return SF_custominiVm_tensor_t{};
        seen_mask |= (1u << perm[i]);
    }

    int64_t parent_strides[8] = { 0 };
    (void)SF_custominiVm_tensor_get_strides(parent, parent_strides);

    int32_t new_shape[8]   = { 0 };
    int64_t new_strides[8] = { 0 };
    for (int i = 0; i < ndim; ++i) {
        new_shape[i]   = parent->shape[perm[i]];
        new_strides[i] = parent_strides[perm[i]];
    }

    t = SF_custominiVm_tensor_slice(parent, 0, new_shape, ndim, parent->dtype);
    if (t.data) {
        auto* view_hdr = SF_custominiVm_live_header_t::from_body(t.data);
        if (view_hdr) {
            auto* view_cold = sf_get_cold_metadata(view_hdr, true);
            if (view_cold) {
                std::memcpy(view_cold->strides, new_strides, sizeof(new_strides));
                view_cold->is_strided_view = 1;
            }
        }
    }
    return t;
}

MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_tensor_broadcast(const SF_custominiVm_tensor_t* parent,
                                                                  const int32_t* target_shape, int target_ndim) {
    SF_custominiVm_tensor_t t{};
    if (!parent || !parent->data || !target_shape || target_ndim <= 0 || target_ndim > 8 || target_ndim < parent->ndim) {
        return t;
    }

    int64_t parent_strides[8] = { 0 };
    (void)SF_custominiVm_tensor_get_strides(parent, parent_strides);

    int64_t bcast_strides[8] = { 0 };
    int offset_dim = target_ndim - parent->ndim;
    for (int i = target_ndim - 1; i >= 0; --i) {
        int32_t tgt_d = target_shape[i];
        if (tgt_d <= 0) return SF_custominiVm_tensor_t{};
        int p_idx = i - offset_dim;
        if (p_idx >= 0) {
            int32_t src_d = parent->shape[p_idx];
            if (src_d == tgt_d) {
                bcast_strides[i] = parent_strides[p_idx];
            } else if (src_d == 1) {
                bcast_strides[i] = 0; // Zero-copy broadcast along singleton dimension
            } else {
                return SF_custominiVm_tensor_t{};
            }
        } else {
            bcast_strides[i] = 0; // Leading broadcast dimension
        }
    }

    auto* parent_hdr = SF_custominiVm_live_header_t::from_body(parent->data);
    if (!parent_hdr) return t;

    void* view_body = sf_ensure_active_runway()->allocate_cell(0, parent_hdr->function_id, "broadcast_view");
    if (!view_body) return t;

    auto* parent_cold = sf_get_cold_metadata(parent_hdr, true);
    auto* view_hdr    = SF_custominiVm_live_header_t::from_body(view_body);
    if (view_hdr) {
        view_hdr->parent_header.store(parent_hdr, std::memory_order_release);
        view_hdr->slice_byte_offset = 0;
        view_hdr->memory_footprint.store(parent->byte_size, std::memory_order_relaxed);

        auto* view_cold = sf_get_cold_metadata(view_hdr, true);
        if (view_cold) {
            std::memcpy(view_cold->strides, bcast_strides, sizeof(bcast_strides));
            view_cold->is_strided_view = 1;
            std::strncpy(view_cold->debug_name, "broadcast_view", sizeof(view_cold->debug_name) - 1);
        }
        if (parent_cold) {
            parent_cold->child_view_count.fetch_add(1, std::memory_order_release);
        }
    }

    t.data      = view_body;
    t.ndim      = target_ndim;
    t.dtype     = parent->dtype;
    t.byte_size = parent->byte_size;
    for (int i = 0; i < target_ndim; ++i) t.shape[i] = target_shape[i];
    return t;
}

MSIMD_API int SF_custominiVm_tensor_pin(SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return -1;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return -1;
    return obj->try_pin() ? 0 : -1;
}

MSIMD_API int SF_custominiVm_tensor_unpin(SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return -1;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return -1;
    obj->unpin();
    return 0;
}

MSIMD_API int SF_custominiVm_tensor_is_view(const SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return 0;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return 0;
    return (obj->parent_header.load(std::memory_order_acquire) != nullptr) ? 1 : 0;
}

MSIMD_API void* SF_custominiVm_tensor_resolve_ptr(const SF_custominiVm_tensor_t* tensor) {
    if (!tensor || !tensor->data) return nullptr;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return tensor->data;
    return obj->resolve_active_body();
}

/* ── VFS Hierarchy & Panic Hooks ── */

MSIMD_API int SF_custominiVm_folder_mkdir(const char* path, size_t quota_bytes) {
    sf_ensure_default_vfs_initialized();
    auto* node = g_sf_vfs_tree.mkdir_p(path, quota_bytes);
    if (node) {
        sf_hotlog_emit(SF_Ebin0_VFS_MKDIR_SUCCESS, SF_Ebin1_TRACE, 0, 0, 0, 0, 0, 0,
            static_cast<uint32_t>(quota_bytes / 1024), path ? path : "vfs_mkdir");
        return 0;
    }
    return -1;
}

MSIMD_API int SF_custominiVm_folder_bind(const char* path, SF_custominiVm_tensor_t* tensor) {
    sf_ensure_default_vfs_initialized();
    if (!tensor || !tensor->data) return -1;
    auto* obj = SF_custominiVm_live_header_t::from_body(tensor->data);
    if (!obj) return -1;
    return g_sf_vfs_tree.bind_tensor(path, obj) ? 0 : -1;
}

MSIMD_API size_t SF_custominiVm_folder_evict(const char* path) {
    sf_ensure_default_vfs_initialized();
    return g_sf_vfs_tree.evict_folder(path, sf_ensure_active_runway());
}

MSIMD_API size_t SF_custominiVm_folder_used_bytes(const char* path) {
    sf_ensure_default_vfs_initialized();
    return g_sf_vfs_tree.get_used_bytes(path);
}

MSIMD_API int SF_custominiVm_vfs_mkdir(const char* path, size_t quota_bytes) {
    return SF_custominiVm_folder_mkdir(path, quota_bytes);
}

MSIMD_API SF_custominiVm_tensor_t SF_custominiVm_vfs_alloc_tensor(const char* path,
                                                                  const int32_t* shape, int ndim, int dtype) {
    sf_ensure_default_vfs_initialized();
    if (!path) {
        if (g_sf_strict_vfs_validation.load(std::memory_order_relaxed)) {
            sf_vfs_trigger_panic_internal("<null>", "vfs_alloc_tensor");
        }
        return SF_custominiVm_tensor_t{};
    }

    SF_custominiVm_tensor_t t = SF_custominiVm_tensor_alloc(shape, ndim, dtype);
    if (t.data) {
        auto* obj = SF_custominiVm_live_header_t::from_body(t.data);
        if (obj) {
            std::vector<std::string> parts;
            sfFolderTree::split_path(path, parts);
            bool bound_ok = false;
            if (!parts.empty()) {
                sf_set_object_debug_name(obj, parts.back().c_str());
                std::string folder = "/";
                if (parts.size() > 1) {
                    folder.clear();
                    for (size_t i = 0; i < parts.size() - 1; ++i) {
                        folder += "/" + parts[i];
                    }
                }
                bound_ok = g_sf_vfs_tree.bind_tensor(folder.c_str(), obj);
            } else {
                bound_ok = g_sf_vfs_tree.bind_tensor("/", obj);
            }
            if (!bound_ok) {
                SF_custominiVm_tensor_free(&t);
                return SF_custominiVm_tensor_t{};
            }
        }
    }
    return t;
}

MSIMD_API int SF_custominiVm_vfs_unlink(const char* path) {
    sf_ensure_default_vfs_initialized();
    return g_sf_vfs_tree.unlink_file(path, sf_ensure_active_runway()) ? 0 : -1;
}

MSIMD_API size_t SF_custominiVm_vfs_rmdir_rf(const char* path) {
    return SF_custominiVm_folder_evict(path);
}

MSIMD_API int SF_custominiVm_vfs_stat(const char* path, SF_custominiVm_vfs_stat_t* out_stat) {
    sf_ensure_default_vfs_initialized();
    return g_sf_vfs_tree.stat_path(path, out_stat) ? 0 : -1;
}

MSIMD_API size_t SF_custominiVm_vfs_snapshot(const char* path, void* out_buf, size_t buf_cap) {
    sf_ensure_default_vfs_initialized();
    return g_sf_vfs_tree.snapshot_path(path, out_buf, buf_cap);
}

MSIMD_API void SF_custominiVm_vfs_ls(const char* path,
                                     void (*cb)(const char* name, int is_dir, size_t bytes, void* user),
                                     void* user) {
    sf_ensure_default_vfs_initialized();
    g_sf_vfs_tree.list_dir(path, cb, user);
}

MSIMD_API void SF_custominiVm_set_strict_vfs_branch_validation(int enable) {
    g_sf_strict_vfs_validation.store(enable != 0, std::memory_order_relaxed);
}

MSIMD_API int SF_custominiVm_get_strict_vfs_branch_validation(void) {
    return g_sf_strict_vfs_validation.load(std::memory_order_relaxed) ? 1 : 0;
}

MSIMD_API int SF_custominiVm_is_strict_vfs_branch_validation(void) {
    return SF_custominiVm_get_strict_vfs_branch_validation();
}

MSIMD_API void SF_custominiVm_vfs_set_panic_dialog_enabled(int enable) {
    g_sf_vfs_panic_dialog_enabled.store(enable != 0, std::memory_order_relaxed);
}

MSIMD_API int SF_custominiVm_vfs_get_panic_dialog_enabled(void) {
    return g_sf_vfs_panic_dialog_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

MSIMD_API void SF_custominiVm_vfs_set_panic_behavior(int suppress_abort, int show_dialog) {
    g_sf_vfs_suppress_abort.store(suppress_abort != 0, std::memory_order_relaxed);
    g_sf_vfs_panic_dialog_enabled.store(show_dialog != 0, std::memory_order_relaxed);
}

MSIMD_API void SF_custominiVm_vfs_set_panic_hook(sf_vfs_panic_hook_t hook, void* user_data) {
    g_sf_vfs_panic_hook      = hook;
    g_sf_vfs_panic_hook_user = user_data;
}

MSIMD_API void SF_custominiVm_vfs_trigger_panic(const char* missing_path, const char* operation_context) {
    sf_vfs_trigger_panic_internal(missing_path, operation_context);
}

MSIMD_API uint64_t SF_custominiVm_vfs_get_panic_count(void) {
    return g_sf_vfs_panic_count.load(std::memory_order_relaxed);
}

MSIMD_API const char* SF_custominiVm_vfs_get_last_panic_path(void) {
    std::lock_guard<std::mutex> lock(g_sf_vfs_panic_mutex);
    return g_sf_vfs_last_panic_path;
}

MSIMD_API const char* SF_custominiVm_vfs_get_last_panic_op(void) {
    std::lock_guard<std::mutex> lock(g_sf_vfs_panic_mutex);
    return g_sf_vfs_last_panic_op;
}

/* ── SBC / GPIO Fast MMIO & Ring-0 HWP Bridges ── */

MSIMD_API int SF_custominiVm_sbc_detect_board(SF_custominiVm_gpio_desc_t* out_desc) {
    if (!out_desc) return -1;
    std::memset(out_desc, 0, sizeof(*out_desc));
    out_desc->board_type = SF_SBC_NONE;
    out_desc->total_pins = 0;
    std::strncpy(out_desc->board_name, "Standard PC/Server", sizeof(out_desc->board_name) - 1);

#if defined(__linux__)
    FILE* fp = std::fopen("/proc/device-tree/model", "r");
    char model_buf[128] = { 0 };
    if (fp) {
        if (std::fgets(model_buf, sizeof(model_buf) - 1, fp)) {
            size_t l = std::strlen(model_buf);
            while (l > 0 && (model_buf[l - 1] == '\n' || model_buf[l - 1] == '\r')) model_buf[--l] = '\0';
        }
        std::fclose(fp);
    }
    if (model_buf[0] != '\0') {
        std::strncpy(out_desc->board_name, model_buf, sizeof(out_desc->board_name) - 1);
        if (std::strstr(model_buf, "Raspberry Pi 5")) {
            out_desc->board_type     = SF_SBC_RASPBERRY_PI_5;
            out_desc->total_pins     = 40;
            out_desc->mmio_phys_base = 0x1F00000000ULL;
        } else if (std::strstr(model_buf, "Raspberry Pi 4")) {
            out_desc->board_type     = SF_SBC_RASPBERRY_PI_4;
            out_desc->total_pins     = 40;
            out_desc->mmio_phys_base = 0xFE200000ULL;
        } else if (std::strstr(model_buf, "Raspberry Pi 3") || std::strstr(model_buf, "Raspberry Pi 2")) {
            out_desc->board_type     = SF_SBC_RASPBERRY_PI_2_3;
            out_desc->total_pins     = 40;
            out_desc->mmio_phys_base = 0x3F200000ULL;
        } else if (std::strstr(model_buf, "Raspberry Pi") || std::strstr(model_buf, "Pi Zero")) {
            out_desc->board_type     = SF_SBC_RASPBERRY_PI_1_ZERO;
            out_desc->total_pins     = 26;
            out_desc->mmio_phys_base = 0x20200000ULL;
        } else if (std::strstr(model_buf, "Orange Pi")) {
            out_desc->board_type     = SF_SBC_ORANGE_PI;
            out_desc->total_pins     = 40;
            out_desc->mmio_phys_base = 0x0300B000ULL;
        } else if (std::strstr(model_buf, "Jetson") || std::strstr(model_buf, "Tegra")) {
            out_desc->board_type     = SF_SBC_NVIDIA_JETSON;
            out_desc->total_pins     = 40;
            out_desc->mmio_phys_base = 0x02200000ULL;
        }
    }
#endif
    g_sf_gpio_desc = *out_desc;
    return 0;
}

MSIMD_API int SF_custominiVm_gpio_init(SF_custominiVm_gpio_desc_t* desc) {
    if (!desc) return -1;
    if (desc->board_type == SF_SBC_NONE) {
        g_sf_gpio_ready.store(true, std::memory_order_release);
        return 0;
    }
#if defined(__linux__)
    int mem_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (mem_fd < 0) mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd >= 0) {
        void* map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, static_cast<off_t>(desc->mmio_phys_base));
        close(mem_fd);
        if (map != MAP_FAILED) {
            desc->mmio_virtual_base  = map;
            desc->is_fast_mmio_active = 1;
            g_sf_os_registry.register_block(map, 4096, SF_OS_TAG_MMIO);
        }
    }
#endif
    g_sf_gpio_desc = *desc;
    g_sf_gpio_ready.store(true, std::memory_order_release);
    return 0;
}

MSIMD_API int SF_custominiVm_gpio_set_mode(uint32_t pin, SF_custominiVm_gpio_pin_mode_e mode) {
    (void)pin;
    (void)mode;
    return 0;
}

MSIMD_API void SF_custominiVm_gpio_fast_write(uint32_t pin, uint8_t level) {
    if (pin >= 64) return;
    uint32_t bank = pin / 32;
    uint32_t bit  = 1u << (pin % 32);
    if (g_sf_gpio_desc.is_fast_mmio_active && g_sf_gpio_desc.mmio_virtual_base) {
        volatile uint32_t* gpio = static_cast<volatile uint32_t*>(g_sf_gpio_desc.mmio_virtual_base);
        if (level) gpio[7 + bank] = bit;
        else       gpio[10 + bank] = bit;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return;
    }
    uint64_t sim_bit = 1ULL << pin;
    if (level) g_sf_gpio_simulated_pins.fetch_or(sim_bit, std::memory_order_relaxed);
    else       g_sf_gpio_simulated_pins.fetch_and(~sim_bit, std::memory_order_relaxed);
}

MSIMD_API uint8_t SF_custominiVm_gpio_fast_read(uint32_t pin) {
    if (pin >= 64) return 0;
    uint32_t bank = pin / 32;
    uint32_t bit  = 1u << (pin % 32);
    if (g_sf_gpio_desc.is_fast_mmio_active && g_sf_gpio_desc.mmio_virtual_base) {
        volatile uint32_t* gpio = static_cast<volatile uint32_t*>(g_sf_gpio_desc.mmio_virtual_base);
        uint32_t reg_val = gpio[13 + bank];
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return (reg_val & bit) ? 1 : 0;
    }
    uint64_t sim_bit = 1ULL << pin;
    return (g_sf_gpio_simulated_pins.load(std::memory_order_relaxed) & sim_bit) ? 1 : 0;
}

MSIMD_API int SF_custominiVm_ring0_connect(SF_custominiVm_ring0_status_t* out_status) {
    std::memset(&g_sf_ring0_status, 0, sizeof(g_sf_ring0_status));
#ifdef _WIN32
    HANDLE hDevice = CreateFileA("\\\\.\\SFVM_K",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDevice != INVALID_HANDLE_VALUE) {
        g_sf_ring0_status.is_driver_loaded = 1;
        g_sf_ring0_status.driver_version   = 0x0100;
        CloseHandle(hDevice);
    }
#elif defined(__linux__)
    int fd = open("/dev/sfvm_k", O_RDWR);
    if (fd >= 0) {
        g_sf_ring0_status.is_driver_loaded = 1;
        g_sf_ring0_status.driver_version   = 0x0100;
        close(fd);
    }
#endif
    if (!g_sf_ring0_status.is_driver_loaded) {
        g_sf_ring0_status.driver_version = 0x0001;
    }
    if (out_status) *out_status = g_sf_ring0_status;
    return g_sf_ring0_status.is_driver_loaded ? 0 : 1;
}

MSIMD_API int SF_custominiVm_ring0_apply_hwp_policy(const SF_custominiVm_ring0_hwp_policy_t* policy) {
    if (!policy) return -1;
    if (g_sf_ring0_status.is_driver_loaded) {
        return 0;
    }
    if (policy->anti_downclock_boost) {
        sf_upclocker_set_boost(1);
        sf_upclocker_t::pin_max_frequency_warmup(25);
    }
    return 0;
}

MSIMD_API void SF_custominiVm_ring0_disconnect(void) {
    g_sf_ring0_status.is_driver_loaded = 0;
}

} // extern "C"
