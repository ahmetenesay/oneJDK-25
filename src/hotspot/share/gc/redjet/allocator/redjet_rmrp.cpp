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

#include "gc/redjet/allocator/redjet_rmrp.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/simd/redjetSIMD.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

// Static member definitions
std::atomic<uint64_t> RedjetRMRP::_total_plump_reclaimed_bytes{ 0 };
std::atomic<uint64_t> RedjetRMRP::_total_sequential_reclaimed_bytes{ 0 };
std::atomic<uint64_t> RedjetRMRP::_total_vfs_reclaimed_bytes{ 0 };
std::atomic<uint32_t> RedjetRMRP::_total_reclamation_cycles{ 0 };
std::atomic<uint32_t> RedjetRMRP::_total_vfs_compaction_triggers{ 0 };

RedjetRMRPBarrier      RedjetRMRP::_barrier;
RedjetRMRPDeduplicator RedjetRMRP::_deduplicator;
RedjetRMRPCompactor    RedjetRMRP::_compactor;

/* ========================================================================= */
/* 1. FREE SPAN INTERVAL TREE (PLUMP COALESCING) IMPLEMENTATION              */
/* ========================================================================= */

RedjetFreeSpanIntervalTree::RedjetFreeSpanIntervalTree()
    : _allocated_nodes(0), _root(nullptr) {
    _tree_lock.next_ticket.store(0, std::memory_order_relaxed);
    _tree_lock.now_serving.store(0, std::memory_order_relaxed);
}

void RedjetFreeSpanIntervalTree::reset() {
    hs_ticket_guard_t guard(_tree_lock);
    _allocated_nodes = 0;
    _root = nullptr;
}

RedjetFreeSpanNode* RedjetFreeSpanIntervalTree::rotate_right(RedjetFreeSpanNode* y) noexcept {
    RedjetFreeSpanNode* x = y->left;
    RedjetFreeSpanNode* T2 = x->right;

    x->right = y;
    y->left = T2;

    update_node(y);
    update_node(x);

    return x;
}

RedjetFreeSpanNode* RedjetFreeSpanIntervalTree::rotate_left(RedjetFreeSpanNode* x) noexcept {
    RedjetFreeSpanNode* y = x->right;
    RedjetFreeSpanNode* T2 = y->left;

    y->left = x;
    x->right = T2;

    update_node(x);
    update_node(y);

    return y;
}

RedjetFreeSpanNode* RedjetFreeSpanIntervalTree::insert_internal(RedjetFreeSpanNode* node, uintptr_t start, uintptr_t end) noexcept {
    if (node == nullptr) {
        if (_allocated_nodes >= MAX_TREE_NODES) return nullptr;
        RedjetFreeSpanNode* n = &_node_pool[_allocated_nodes++];
        n->start_addr = start;
        n->end_addr = end;
        n->size_bytes = static_cast<size_t>(end - start);
        n->max_subtree_end = end;
        n->left = nullptr;
        n->right = nullptr;
        n->height = 1;
        return n;
    }

    if (start < node->start_addr) {
        node->left = insert_internal(node->left, start, end);
    } else {
        node->right = insert_internal(node->right, start, end);
    }

    update_node(node);

    int balance = get_balance(node);

    // Left-Left Case
    if (balance > 1 && start < node->left->start_addr) {
        return rotate_right(node);
    }
    // Right-Right Case
    if (balance < -1 && start >= node->right->start_addr) {
        return rotate_left(node);
    }
    // Left-Right Case
    if (balance > 1 && start >= node->left->start_addr) {
        node->left = rotate_left(node->left);
        return rotate_right(node);
    }
    // Right-Left Case
    if (balance < -1 && start < node->right->start_addr) {
        node->right = rotate_right(node->right);
        return rotate_left(node);
    }

    return node;
}

bool RedjetFreeSpanIntervalTree::insert_span(uintptr_t start, uintptr_t end) {
    if (start >= end) return false;
    hs_ticket_guard_t guard(_tree_lock);
    if (_allocated_nodes >= MAX_TREE_NODES) return false;

    _root = insert_internal(_root, start, end);
    return true;
}

static void inorder_collect(RedjetFreeSpanNode* n, RedjetFreeSpanNode** out_arr, size_t* count, size_t max_count) {
    if (n == nullptr || *count >= max_count) return;
    inorder_collect(n->left, out_arr, count, max_count);
    if (*count < max_count) {
        out_arr[(*count)++] = n;
    }
    inorder_collect(n->right, out_arr, count, max_count);
}

size_t RedjetFreeSpanIntervalTree::coalesce_adjacent_spans(uintptr_t* out_coalesced_starts, size_t* out_coalesced_sizes, size_t max_results) {
    if (out_coalesced_starts == nullptr || out_coalesced_sizes == nullptr || max_results == 0) return 0;
    hs_ticket_guard_t guard(_tree_lock);
    if (_root == nullptr || _allocated_nodes == 0) return 0;

    RedjetFreeSpanNode* sorted_nodes[MAX_TREE_NODES];
    size_t node_count = 0;
    inorder_collect(_root, sorted_nodes, &node_count, MAX_TREE_NODES);

    if (node_count == 0) return 0;

    size_t out_idx = 0;
    uintptr_t cur_start = sorted_nodes[0]->start_addr;
    uintptr_t cur_end = sorted_nodes[0]->end_addr;

    for (size_t i = 1; i < node_count; ++i) {
        if (sorted_nodes[i]->start_addr == cur_end) {
            // Contiguous adjacent hole: merge!
            cur_end = sorted_nodes[i]->end_addr;
        } else {
            out_coalesced_starts[out_idx] = cur_start;
            out_coalesced_sizes[out_idx] = static_cast<size_t>(cur_end - cur_start);
            out_idx++;
            if (out_idx >= max_results) break;

            cur_start = sorted_nodes[i]->start_addr;
            cur_end = sorted_nodes[i]->end_addr;
        }
    }

    if (out_idx < max_results) {
        out_coalesced_starts[out_idx] = cur_start;
        out_coalesced_sizes[out_idx] = static_cast<size_t>(cur_end - cur_start);
        out_idx++;
    }

    return out_idx;
}

size_t RedjetFreeSpanIntervalTree::total_free_bytes() const noexcept {
    size_t sum = 0;
    for (size_t i = 0; i < _allocated_nodes; ++i) {
        sum += _node_pool[i].size_bytes;
    }
    return sum;
}

/* ========================================================================= */
/* 2. BUDDY ALLOCATOR CHUNK COALESCER IMPLEMENTATION                         */
/* ========================================================================= */

RedjetBuddyCoalescer::RedjetBuddyCoalescer() : _pool_cursor(0) {
    _buddy_lock.next_ticket.store(0, std::memory_order_relaxed);
    _buddy_lock.now_serving.store(0, std::memory_order_relaxed);
    for (size_t o = 0; o < NUM_BUDDY_ORDERS; ++o) {
        _free_lists[o] = nullptr;
    }
}

void RedjetBuddyCoalescer::reset() {
    hs_ticket_guard_t guard(_buddy_lock);
    _pool_cursor = 0;
    for (size_t o = 0; o < NUM_BUDDY_ORDERS; ++o) {
        _free_lists[o] = nullptr;
    }
}

bool RedjetBuddyCoalescer::register_free_block(uintptr_t addr, size_t size) {
    if (addr == 0 || size < BASE_CHUNK_SIZE) return false;
    hs_ticket_guard_t guard(_buddy_lock);
    if (_pool_cursor >= MAX_BUDDY_BLOCKS) return false;

    // Determine initial order
    uint8_t order = 0;
    size_t chunk_sz = BASE_CHUNK_SIZE;
    while (order + 1 < NUM_BUDDY_ORDERS && (chunk_sz << 1) <= size) {
        chunk_sz <<= 1;
        order++;
    }

    BuddyBlock* blk = &_block_pool[_pool_cursor++];
    blk->address = addr;
    blk->order = order;
    blk->is_free = true;
    blk->prev = nullptr;
    blk->next = _free_lists[order];
    if (_free_lists[order] != nullptr) {
        _free_lists[order]->prev = blk;
    }
    _free_lists[order] = blk;

    return true;
}

size_t RedjetBuddyCoalescer::coalesce_all_orders() {
    hs_ticket_guard_t guard(_buddy_lock);
    size_t coalesced_merges = 0;

    for (uint8_t o = 0; o + 1 < NUM_BUDDY_ORDERS; ++o) {
        BuddyBlock* cur = _free_lists[o];
        while (cur != nullptr) {
            size_t block_sz = BASE_CHUNK_SIZE << o;
            uintptr_t buddy_addr = cur->address ^ block_sz;

            // Search for buddy block in current order list
            BuddyBlock* candidate = _free_lists[o];
            BuddyBlock* found_buddy = nullptr;
            while (candidate != nullptr) {
                if (candidate != cur && candidate->address == buddy_addr && candidate->is_free) {
                    found_buddy = candidate;
                    break;
                }
                candidate = candidate->next;
            }

            if (found_buddy != nullptr) {
                // Remove cur from list
                if (cur->prev) cur->prev->next = cur->next;
                else _free_lists[o] = cur->next;
                if (cur->next) cur->next->prev = cur->prev;

                // Remove found_buddy from list
                if (found_buddy->prev) found_buddy->prev->next = found_buddy->next;
                else _free_lists[o] = found_buddy->next;
                if (found_buddy->next) found_buddy->next->prev = found_buddy->prev;

                // Create promoted super block of order + 1
                uintptr_t super_addr = (cur->address < found_buddy->address) ? cur->address : found_buddy->address;
                cur->address = super_addr;
                cur->order = o + 1;
                cur->is_free = true;
                cur->prev = nullptr;
                cur->next = _free_lists[o + 1];
                if (_free_lists[o + 1] != nullptr) {
                    _free_lists[o + 1]->prev = cur;
                }
                _free_lists[o + 1] = cur;

                found_buddy->is_free = false; // Mark inactive
                coalesced_merges++;
                cur = _free_lists[o]; // Restart search in this order
            } else {
                cur = cur->next;
            }
        }
    }

    return coalesced_merges;
}

size_t RedjetBuddyCoalescer::extract_super_blocks(uint8_t target_order, uintptr_t* out_addresses, size_t max_count) {
    if (out_addresses == nullptr || max_count == 0 || target_order >= NUM_BUDDY_ORDERS) return 0;
    hs_ticket_guard_t guard(_buddy_lock);

    size_t count = 0;
    BuddyBlock* cur = _free_lists[target_order];
    while (cur != nullptr && count < max_count) {
        if (cur->is_free) {
            out_addresses[count++] = cur->address;
        }
        cur = cur->next;
    }
    return count;
}

/* ========================================================================= */
/* 3. SHANNON ENTROPY FRAGMENTATION ANALYZER IMPLEMENTATION                  */
/* ========================================================================= */

RedjetFragmentationAnalyzer::FragmentationReport
RedjetFragmentationAnalyzer::analyze_fragmentation(const uintptr_t* hole_starts,
                                                   const size_t* hole_sizes,
                                                   size_t hole_count,
                                                   size_t total_arena_capacity) noexcept {
    FragmentationReport r{};
    if (hole_sizes == nullptr || hole_count == 0 || total_arena_capacity == 0) {
        return r;
    }

    r.total_free_holes = hole_count;
    r.smallest_hole_bytes = SIZE_MAX;
    r.largest_hole_bytes = 0;

    size_t bin_counts[NUM_SIZE_BINS] = { 0 };
    size_t bin_bytes[NUM_SIZE_BINS] = { 0 };

    for (size_t i = 0; i < hole_count; ++i) {
        size_t sz = hole_sizes[i];
        r.total_free_bytes += sz;
        if (sz > r.largest_hole_bytes) r.largest_hole_bytes = sz;
        if (sz < r.smallest_hole_bytes) r.smallest_hole_bytes = sz;

        // Classify into size bin
        if (sz <= 64)                { bin_counts[0]++; bin_bytes[0] += sz; }
        else if (sz <= 256)           { bin_counts[1]++; bin_bytes[1] += sz; }
        else if (sz <= 1024)          { bin_counts[2]++; bin_bytes[2] += sz; }
        else if (sz <= 4096)          { bin_counts[3]++; bin_bytes[3] += sz; }
        else if (sz <= 16 * 1024)     { bin_counts[4]++; bin_bytes[4] += sz; }
        else if (sz <= 64 * 1024)     { bin_counts[5]++; bin_bytes[5] += sz; }
        else if (sz <= 256 * 1024)    { bin_counts[6]++; bin_bytes[6] += sz; }
        else                          { bin_counts[7]++; bin_bytes[7] += sz; }
    }

    if (r.smallest_hole_bytes == SIZE_MAX) r.smallest_hole_bytes = 0;

    // Calculate Shannon Entropy: H = - \sum p_i * log2(p_i)
    float entropy = 0.0f;
    float total_holes_f = static_cast<float>(hole_count);
    for (size_t b = 0; b < NUM_SIZE_BINS; ++b) {
        if (bin_counts[b] > 0) {
            float p_i = static_cast<float>(bin_counts[b]) / total_holes_f;
            entropy -= p_i * (std::log(p_i) / 0.69314718056f); // log2(x) = ln(x) / ln(2)
        }
    }
    r.shannon_entropy = entropy;

    // Normalized Fragmentation Score:
    // Combines entropy, small-hole ratio, and largest-hole ratio
    float small_holes = static_cast<float>(bin_counts[0] + bin_counts[1] + bin_counts[2]);
    float small_ratio = small_holes / total_holes_f;
    float free_ratio = static_cast<float>(r.total_free_bytes) / static_cast<float>(total_arena_capacity);
    float largest_ratio = (r.total_free_bytes > 0) ? (static_cast<float>(r.largest_hole_bytes) / static_cast<float>(r.total_free_bytes)) : 1.0f;

    float score = 0.4f * (entropy / 3.0f) + 0.4f * small_ratio + 0.2f * (1.0f - largest_ratio);
    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    r.fragmentation_score = score;

    return r;
}

/* ========================================================================= */
/* 4. REDJET RMRP BARRIER IMPLEMENTATION                                     */
/* ========================================================================= */

RedjetRMRPBarrier::RedjetRMRPBarrier() {
    _entry_count.store(0, std::memory_order_relaxed);
    _barrier_hits.store(0, std::memory_order_relaxed);
    _cold_transitions.store(0, std::memory_order_relaxed);
}

void RedjetRMRPBarrier::register_page(uintptr_t page_addr, size_t page_sz) {
    hs_ticket_guard_t guard(_barrier_lock);
    size_t count = _entry_count.load(std::memory_order_relaxed);
    if (count >= MAX_TRACKED_PAGES) return;

    for (size_t i = 0; i < count; ++i) {
        if (_entries[i].page_address == page_addr) {
            _entries[i].page_size = page_sz;
            _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT), std::memory_order_relaxed);
            _entries[i].access_heat.store(100, std::memory_order_relaxed);
            _entries[i].idle_sweeps.store(0, std::memory_order_relaxed);
            return;
        }
    }

    _entries[count].page_address = page_addr;
    _entries[count].page_size = page_sz;
    _entries[count].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT), std::memory_order_relaxed);
    _entries[count].access_heat.store(100, std::memory_order_relaxed);
    _entries[count].idle_sweeps.store(0, std::memory_order_relaxed);
    _entries[count].last_touch_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
    _entry_count.store(count + 1, std::memory_order_release);
}

void RedjetRMRPBarrier::on_page_touch(uintptr_t addr) noexcept {
    _barrier_hits.fetch_add(1, std::memory_order_relaxed);
    uintptr_t page_base = addr & ~static_cast<uintptr_t>(4095);

    size_t count = _entry_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (_entries[i].page_address == page_base) {
            _entries[i].access_heat.store(100, std::memory_order_relaxed);
            _entries[i].idle_sweeps.store(0, std::memory_order_relaxed);
            _entries[i].last_touch_tsc.store(sacculus_rdtsc(), std::memory_order_relaxed);
            _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT), std::memory_order_relaxed);
            return;
        }
    }
}

size_t RedjetRMRPBarrier::decay_and_classify_cold_pages(uint32_t decay_amount) {
    size_t cold_count = 0;
    size_t count = _entry_count.load(std::memory_order_relaxed);

    for (size_t i = 0; i < count; ++i) {
        uint32_t heat = _entries[i].access_heat.load(std::memory_order_relaxed);
        if (heat > decay_amount) {
            _entries[i].access_heat.store(heat - decay_amount, std::memory_order_relaxed);
        } else {
            _entries[i].access_heat.store(0, std::memory_order_relaxed);
            uint32_t sweeps = _entries[i].idle_sweeps.fetch_add(1, std::memory_order_relaxed);
            if (sweeps >= 3) {
                uint8_t cur_state = _entries[i].state.load(std::memory_order_relaxed);
                if (cur_state != static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD) &&
                    cur_state != static_cast<uint8_t>(RedjetPageResidentState::DECOMMITTED_IDLE)) {
                    _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD), std::memory_order_release);
                    _cold_transitions.fetch_add(1, std::memory_order_relaxed);
                    cold_count++;
                }
            }
        }
    }
    return cold_count;
}

size_t RedjetRMRPBarrier::decommit_cold_pages(size_t target_reclaim_bytes) {
    size_t reclaimed = 0;
    size_t count = _entry_count.load(std::memory_order_relaxed);

    for (size_t i = 0; i < count && reclaimed < target_reclaim_bytes; ++i) {
        if (_entries[i].state.load(std::memory_order_relaxed) == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD)) {
            uintptr_t addr = _entries[i].page_address;
            size_t sz = _entries[i].page_size;
            if (addr != 0 && sz != 0) {
#if defined(_WIN32)
                VirtualFree(reinterpret_cast<LPVOID>(addr), sz, MEM_DECOMMIT);
#else
                madvise(reinterpret_cast<void*>(addr), sz, MADV_DONTNEED);
#endif
                _entries[i].state.store(static_cast<uint8_t>(RedjetPageResidentState::DECOMMITTED_IDLE), std::memory_order_release);
                reclaimed += sz;
            }
        }
    }
    return reclaimed;
}

RedjetRMRPResidentBloatSignal RedjetRMRPBarrier::evaluate_bloat_signal(float threshold_ratio, size_t min_cold_bytes) const noexcept {
    RedjetRMRPResidentBloatSignal sig{};
    size_t total = _entry_count.load(std::memory_order_relaxed);
    if (total == 0) return sig;

    sig.total_monitored_pages = total;
    for (size_t i = 0; i < total; ++i) {
        uint8_t st = _entries[i].state.load(std::memory_order_relaxed);
        if (st == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD)) {
            sig.cold_page_count++;
            sig.cold_resident_bytes += _entries[i].page_size;
        }
    }

    sig.cold_page_ratio = static_cast<float>(sig.cold_page_count) / static_cast<float>(total);
    sig.bloat_detected = (sig.cold_page_ratio >= threshold_ratio) && (sig.cold_resident_bytes >= min_cold_bytes);
    return sig;
}

bool RedjetRMRPBarrier::is_page_cold(uintptr_t addr) const noexcept {
    uintptr_t page_base = addr & ~static_cast<uintptr_t>(4095);
    size_t total = _entry_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < total; ++i) {
        if (_entries[i].page_address == page_base) {
            uint8_t st = _entries[i].state.load(std::memory_order_relaxed);
            return (st == static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_COLD) ||
                    st == static_cast<uint8_t>(RedjetPageResidentState::DECOMMITTED_IDLE));
        }
    }
    return false;
}

/* ========================================================================= */
/* 5. ZERO-PAGE DEDUPLICATOR (AVX-512 / AVX2 / SCALAR) IMPLEMENTATION        */
/* ========================================================================= */

RedjetRMRPDeduplicator::RedjetRMRPDeduplicator() {
    _total_zero_pages_detected.store(0, std::memory_order_relaxed);
    _total_zero_bytes_reclaimed.store(0, std::memory_order_relaxed);
    _scan_operations.store(0, std::memory_order_relaxed);
    _simd512_scans.store(0, std::memory_order_relaxed);
}

bool RedjetRMRPDeduplicator::is_page_all_zero(const void* page_addr, size_t page_sz) noexcept {
    if (page_addr == nullptr || page_sz == 0) return false;

    const uint8_t* ptr = static_cast<const uint8_t*>(page_addr);
    size_t offset = 0;

#if defined(__AVX512F__)
    // 512-bit vector zero checking: 64 bytes per cycle with 4x unrolling (256 bytes per loop)
    __m512i zero_vec = _mm512_setzero_si512();
    while (offset + 256 <= page_sz) {
        __m512i v0 = _mm512_loadu_si512(reinterpret_cast<const void*>(ptr + offset));
        __m512i v1 = _mm512_loadu_si512(reinterpret_cast<const void*>(ptr + offset + 64));
        __m512i v2 = _mm512_loadu_si512(reinterpret_cast<const void*>(ptr + offset + 128));
        __m512i v3 = _mm512_loadu_si512(reinterpret_cast<const void*>(ptr + offset + 192));

        __mmask8 m0 = _mm512_test_epi64_mask(v0, v0);
        __mmask8 m1 = _mm512_test_epi64_mask(v1, v1);
        __mmask8 m2 = _mm512_test_epi64_mask(v2, v2);
        __mmask8 m3 = _mm512_test_epi64_mask(v3, v3);

        if ((m0 | m1 | m2 | m3) != 0) {
            return false;
        }
        offset += 256;
    }
#elif defined(__AVX2__)
    // 256-bit AVX2 fallback: 32 bytes per cycle
    __m256i zero_vec = _mm256_setzero_si256();
    while (offset + 128 <= page_sz) {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr + offset));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr + offset + 32));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr + offset + 64));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr + offset + 96));

        if (!_mm256_testz_si256(v0, v0) || !_mm256_testz_si256(v1, v1) ||
            !_mm256_testz_si256(v2, v2) || !_mm256_testz_si256(v3, v3)) {
            return false;
        }
        offset += 128;
    }
#endif

    // Scalar 64-bit unrolled fallback
    const uint64_t* u64 = reinterpret_cast<const uint64_t*>(ptr + offset);
    size_t u64_words = (page_sz - offset) / sizeof(uint64_t);

    for (size_t i = 0; i < u64_words; ++i) {
        if (u64[i] != 0) {
            return false;
        }
    }

    size_t rem = (page_sz - offset) % sizeof(uint64_t);
    const uint8_t* rem_bytes = ptr + page_sz - rem;
    for (size_t i = 0; i < rem; ++i) {
        if (rem_bytes[i] != 0) {
            return false;
        }
    }

    return true;
}

size_t RedjetRMRPDeduplicator::deduplicate_zero_pages_in_range(void* base_addr, size_t range_bytes, size_t page_sz) {
    if (base_addr == nullptr || range_bytes < page_sz) return 0;
    _scan_operations.fetch_add(1, std::memory_order_relaxed);

    size_t total_pages = range_bytes / page_sz;
    size_t reclaimed_bytes = 0;
    uint8_t* cursor = static_cast<uint8_t*>(base_addr);

    for (size_t p = 0; p < total_pages; ++p) {
        uint8_t* page = cursor + (p * page_sz);
        if (is_page_all_zero(page, page_sz)) {
#if defined(_WIN32)
            VirtualFree(reinterpret_cast<LPVOID>(page), page_sz, MEM_DECOMMIT);
#else
            madvise(reinterpret_cast<void*>(page), page_sz, MADV_DONTNEED);
#endif
            _total_zero_pages_detected.fetch_add(1, std::memory_order_relaxed);
            reclaimed_bytes += page_sz;
        }
    }

    _total_zero_bytes_reclaimed.fetch_add(reclaimed_bytes, std::memory_order_relaxed);
    return reclaimed_bytes;
}

/* ========================================================================= */
/* 6. PLUMP, SEQUENTIAL & VFS COMPACTOR IMPLEMENTATION                       */
/* ========================================================================= */

RedjetRMRPCompactor::RedjetRMRPCompactor() {
    _plump_cycles.store(0, std::memory_order_relaxed);
    _sequential_cycles.store(0, std::memory_order_relaxed);
    _vfs_compaction_cycles.store(0, std::memory_order_relaxed);
    _coalesced_hole_bytes.store(0, std::memory_order_relaxed);
}

size_t RedjetRMRPCompactor::execute_plump_compaction(void* arena_base, size_t arena_capacity, size_t target_reclaim_bytes) {
    if (arena_base == nullptr || arena_capacity == 0 || target_reclaim_bytes == 0) return 0;
    _plump_cycles.fetch_add(1, std::memory_order_relaxed);

    _interval_tree.reset();
    _buddy_coalescer.reset();

    // Divide arena into 64KB slabs and detect contiguous idle spans
    constexpr size_t SLAB_SZ = 64 * 1024;
    size_t num_slabs = arena_capacity / SLAB_SZ;
    uint8_t* base = static_cast<uint8_t*>(arena_base);

    for (size_t s = 0; s < num_slabs; ++s) {
        uint8_t* slab_addr = base + (s * SLAB_SZ);
        if (RedjetRMRPDeduplicator::is_page_all_zero(slab_addr, SLAB_SZ)) {
            _interval_tree.insert_span(reinterpret_cast<uintptr_t>(slab_addr),
                                       reinterpret_cast<uintptr_t>(slab_addr + SLAB_SZ));
            _buddy_coalescer.register_free_block(reinterpret_cast<uintptr_t>(slab_addr), SLAB_SZ);
        }
    }

    // Coalesce buddy orders to form 2MB large pages
    _buddy_coalescer.coalesce_all_orders();

    uintptr_t coalesced_starts[256];
    size_t    coalesced_sizes[256];
    size_t    coalesced_count = _interval_tree.coalesce_adjacent_spans(coalesced_starts, coalesced_sizes, 256);

    size_t total_reclaimed = 0;
    for (size_t i = 0; i < coalesced_count && total_reclaimed < target_reclaim_bytes; ++i) {
        uintptr_t start = coalesced_starts[i];
        size_t sz = coalesced_sizes[i];

#if defined(_WIN32)
        VirtualFree(reinterpret_cast<LPVOID>(start), sz, MEM_DECOMMIT);
#else
        madvise(reinterpret_cast<void*>(start), sz, MADV_DONTNEED);
#endif
        total_reclaimed += sz;
    }

    _coalesced_hole_bytes.fetch_add(total_reclaimed, std::memory_order_relaxed);
    return total_reclaimed;
}

size_t RedjetRMRPCompactor::execute_sequential_compaction(void* arena_base, size_t arena_capacity, size_t target_reclaim_bytes) {
    if (arena_base == nullptr || arena_capacity == 0 || target_reclaim_bytes == 0) return 0;
    _sequential_cycles.fetch_add(1, std::memory_order_relaxed);

    constexpr size_t PAGE_SZ = 4096;
    size_t num_pages = arena_capacity / PAGE_SZ;
    uint8_t* base = static_cast<uint8_t*>(arena_base);
    size_t total_reclaimed = 0;

    // Linear sequential sweep from the high address boundary downwards
    for (size_t p = num_pages; p > 0 && total_reclaimed < target_reclaim_bytes; --p) {
        uint8_t* page_addr = base + ((p - 1) * PAGE_SZ);
        if (RedjetRMRPDeduplicator::is_page_all_zero(page_addr, PAGE_SZ)) {
#if defined(_WIN32)
            VirtualFree(reinterpret_cast<LPVOID>(page_addr), PAGE_SZ, MEM_DECOMMIT);
#else
            madvise(reinterpret_cast<void*>(page_addr), PAGE_SZ, MADV_DONTNEED);
#endif
            total_reclaimed += PAGE_SZ;
        }
    }

    return total_reclaimed;
}

size_t RedjetRMRPCompactor::execute_vfs_compaction(void* arena_base, size_t target_reclaim_bytes) {
    _vfs_compaction_cycles.fetch_add(1, std::memory_order_relaxed);
    return execute_plump_compaction(arena_base, target_reclaim_bytes * 2, target_reclaim_bytes);
}

/* ========================================================================= */
/* 7. REDJET RMRP SUBSYSTEM NEXUS IMPLEMENTATION                             */
/* ========================================================================= */

size_t RedjetRMRP::execute_reclamation(hs_rmrp_method_e method, size_t target_bytes) {
    _total_reclamation_cycles.fetch_add(1, std::memory_order_relaxed);
    if (method == hs_rmrp_method_e::PLUMP) {
        return execute_plump_reclamation(target_bytes);
    } else {
        return execute_sequential_trimming(target_bytes);
    }
}

size_t RedjetRMRP::execute_plump_reclamation(size_t target_bytes) {
    size_t reclaimed = _barrier.decommit_cold_pages(target_bytes);
    _total_plump_reclaimed_bytes.fetch_add(reclaimed, std::memory_order_relaxed);
    return reclaimed;
}

size_t RedjetRMRP::execute_sequential_trimming(size_t target_bytes) {
    size_t reclaimed = _barrier.decommit_cold_pages(target_bytes);
    _total_sequential_reclaimed_bytes.fetch_add(reclaimed, std::memory_order_relaxed);
    return reclaimed;
}

RedjetRMRPResidentBloatSignal RedjetRMRP::check_resident_bloat(float threshold_ratio) {
    return _barrier.evaluate_bloat_signal(threshold_ratio);
}

RedjetVFSCompactionTriggerResult RedjetRMRP::evaluate_and_trigger_vfs_compaction(
    float fragmentation_score, bool mltc_lull_active, size_t target_reclaim_bytes) {
    RedjetVFSCompactionTriggerResult res{};
    res.fragmentation_score = fragmentation_score;
    res.lull_signal = mltc_lull_active;

    RedjetRMRPResidentBloatSignal bloat = check_resident_bloat(0.25f);
    res.bloat_signal = bloat.bloat_detected;

    // Trigger Autonomous VFS Compaction when:
    // (Fragmentation > 0.35 OR Bloat Detected) AND MLTC Bus Traffic is in Lull Window
    if ((fragmentation_score >= 0.35f || bloat.bloat_detected) && mltc_lull_active) {
        res.triggered = true;
        res.compaction_timestamp_tsc = sacculus_rdtsc();
        res.reclaimed_bytes = execute_plump_reclamation(target_reclaim_bytes);
        _total_vfs_compaction_triggers.fetch_add(1, std::memory_order_relaxed);
        _total_vfs_reclaimed_bytes.fetch_add(res.reclaimed_bytes, std::memory_order_relaxed);
    }

    return res;
}

size_t RedjetRMRP::deduplicate_zero_pages(void* base_addr, size_t bytes, size_t page_sz) {
    return _deduplicator.deduplicate_zero_pages_in_range(base_addr, bytes, page_sz);
}

void RedjetRMRP::register_monitored_page(uintptr_t page_addr, size_t page_sz) {
    _barrier.register_page(page_addr, page_sz);
}

void RedjetRMRP::on_barrier_touch(uintptr_t addr) noexcept {
    _barrier.on_page_touch(addr);
}

size_t RedjetRMRP::sweep_cold_pages(uint32_t decay_amount) {
    return _barrier.decay_and_classify_cold_pages(decay_amount);
}
