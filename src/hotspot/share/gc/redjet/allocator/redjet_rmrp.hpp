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

#ifndef SHARE_GC_REDJET_ALLOCATOR_REDJET_RMRP_HPP
#define SHARE_GC_REDJET_ALLOCATOR_REDJET_RMRP_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "utilities/globalDefinitions.hpp"
#include "gc/redjet/allocator/redjet_mltc.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

#ifndef HS_RMRP_METHOD_E_DEFINED
#define HS_RMRP_METHOD_E_DEFINED
enum class hs_rmrp_method_e : uint8_t {
    PLUMP      = 0, // "Prune and coalesce before rebate": merges adjacent free spans into super-blocks before OS decommit
    SEQUENTIAL = 1  // "Installment / stepwise per block": trims trailing cold pages immediately to shrink RSS
};
#endif

// ============================================================================
// RMRP (Reduce Memory Resident Set Reclamation) Subsystem (PLAN.md Section 9.6 & 11.1)
//
// Dual-Method Reclamation:
// 1. PLUMP Mode (Default):
//    - Scans sparse, fragmented slabs and idle Runway/Hotfield memory.
//    - Builds an in-memory Interval-Tree of free holes.
//    - Coalesces adjacent contiguous free holes into large 64KB/2MB chunks.
//    - Performs single-shot bulk OS decommit (VirtualFree / madvise) to prevent RAM hole dispersion.
// 2. SEQUENTIAL Mode (Stepwise installment):
//    - Micro-installment FIFO queue for cloud/container environments requiring low RSS.
//    - Progressive page-by-page span trimming with rate-limiting token bucket.
// 3. Mathematical Foundations:
//    - Shannon Entropy Fragmentation Index:
//        H(X) = - \sum_{i=1}^{k} p_i \log_2(p_i)
//      Measures dispersal of free holes across size classes.
//    - Buddy Allocator Chunk Coalescing:
//        Order-0 (4KB) through Order-9 (2MB large pages).
//    - 512-bit AVX-512 Zero-Page Deduplicator:
//        Scans physical frames using _mm512_test_epi64_mask with 4x unrolling.
// 4. Autonomous VFS Compaction 3-Signal Trigger:
//    - Signal 1: Fragmentation Score (> 35% threshold).
//    - Signal 2: RMRP Cold-Page Resident Bloat Signal.
//    - Signal 3: MLTC Bus Traffic Pacing Lull Window.
// ============================================================================

enum class RedjetPageResidentState : uint8_t {
    RESIDENT_HOT         = 0, // Frequently mutated, keep fully committed in L3/DRAM
    RESIDENT_WARM        = 1, // Mutated within recent sweeps
    RESIDENT_COLD        = 2, // Inactive, eligible for span trimming
    DECOMMITTED_IDLE     = 3, // Decommitted from OS RSS
    DEDUPLICATED_ZERO    = 4  // Detected all-zero, physical page released
};

struct alignas(64) RedjetPagePropagationEntry {
    uintptr_t                   page_address{ 0 };
    size_t                      page_size{ 0 };
    std::atomic<uint8_t>        state{ static_cast<uint8_t>(RedjetPageResidentState::RESIDENT_HOT) };
    std::atomic<uint32_t>       access_heat{ 100 };
    std::atomic<uint32_t>       idle_sweeps{ 0 };
    std::atomic<uint64_t>       last_touch_tsc{ 0 };
};

/* ========================================================================= */
/* 0. RMRP SIGNALS FOR AUTONOMOUS VFS COMPACTION                             */
/* ========================================================================= */

struct RedjetRMRPResidentBloatSignal {
    size_t cold_page_count{ 0 };
    size_t total_monitored_pages{ 0 };
    size_t cold_resident_bytes{ 0 };
    float  cold_page_ratio{ 0.0f };
    bool   bloat_detected{ false };
};

struct RedjetVFSCompactionTriggerResult {
    bool     triggered{ false };
    float    fragmentation_score{ 0.0f };
    bool     bloat_signal{ false };
    bool     lull_signal{ false };
    size_t   reclaimed_bytes{ 0 };
    uint64_t compaction_timestamp_tsc{ 0 };
};

/* ========================================================================= */
/* 1. INTERVAL-TREE FREE SPAN STRUCTURES (PLUMP COALESCING)                  */
/* ========================================================================= */

struct RedjetFreeSpanNode {
    uintptr_t           start_addr{ 0 };
    uintptr_t           end_addr{ 0 };
    size_t              size_bytes{ 0 };
    uintptr_t           max_subtree_end{ 0 };
    RedjetFreeSpanNode* left{ nullptr };
    RedjetFreeSpanNode* right{ nullptr };
    int                 height{ 1 };
};

class RedjetFreeSpanIntervalTree {
public:
    static constexpr size_t MAX_TREE_NODES = 2048;

private:
    RedjetFreeSpanNode _node_pool[MAX_TREE_NODES];
    size_t             _allocated_nodes{ 0 };
    RedjetFreeSpanNode* _root{ nullptr };
    hs_ticket_lock_t   _tree_lock;

    int get_height(RedjetFreeSpanNode* n) const noexcept {
        return (n != nullptr) ? n->height : 0;
    }

    uintptr_t get_max(uintptr_t a, uintptr_t b, uintptr_t c) const noexcept {
        uintptr_t m = (a > b) ? a : b;
        return (m > c) ? m : c;
    }

    void update_node(RedjetFreeSpanNode* n) noexcept {
        if (n == nullptr) return;
        int hl = get_height(n->left);
        int hr = get_height(n->right);
        n->height = 1 + ((hl > hr) ? hl : hr);
        n->max_subtree_end = get_max(n->end_addr,
                                     (n->left != nullptr) ? n->left->max_subtree_end : 0,
                                     (n->right != nullptr) ? n->right->max_subtree_end : 0);
    }

    int get_balance(RedjetFreeSpanNode* n) const noexcept {
        return (n != nullptr) ? (get_height(n->left) - get_height(n->right)) : 0;
    }

    RedjetFreeSpanNode* rotate_right(RedjetFreeSpanNode* y) noexcept;
    RedjetFreeSpanNode* rotate_left(RedjetFreeSpanNode* x) noexcept;
    RedjetFreeSpanNode* insert_internal(RedjetFreeSpanNode* node, uintptr_t start, uintptr_t end) noexcept;

public:
    RedjetFreeSpanIntervalTree();

    void reset();
    bool insert_span(uintptr_t start, uintptr_t end);
    size_t coalesce_adjacent_spans(uintptr_t* out_coalesced_starts, size_t* out_coalesced_sizes, size_t max_results);
    size_t total_free_bytes() const noexcept;
};

/* ========================================================================= */
/* 2. BUDDY ALLOCATOR CHUNK COALESCER (Order-0 to Order-9 / 2MB)             */
/* ========================================================================= */

class RedjetBuddyCoalescer {
public:
    static constexpr size_t NUM_BUDDY_ORDERS = 10; // Order 0 = 4KB, Order 9 = 2MB
    static constexpr size_t BASE_CHUNK_SIZE  = 4096;

    struct BuddyBlock {
        uintptr_t   address{ 0 };
        uint8_t     order{ 0 };
        bool        is_free{ true };
        BuddyBlock* next{ nullptr };
        BuddyBlock* prev{ nullptr };
    };

private:
    static constexpr size_t MAX_BUDDY_BLOCKS = 1024;
    BuddyBlock  _block_pool[MAX_BUDDY_BLOCKS];
    size_t      _pool_cursor{ 0 };
    BuddyBlock* _free_lists[NUM_BUDDY_ORDERS]{ nullptr };
    hs_ticket_lock_t _buddy_lock;

public:
    RedjetBuddyCoalescer();

    void reset();
    bool register_free_block(uintptr_t addr, size_t size);
    size_t coalesce_all_orders();
    size_t extract_super_blocks(uint8_t target_order, uintptr_t* out_addresses, size_t max_count);
};

/* ========================================================================= */
/* 3. SHANNON ENTROPY FRAGMENTATION ANALYZER                                  */
/* ========================================================================= */

class RedjetFragmentationAnalyzer {
public:
    static constexpr size_t NUM_SIZE_BINS = 8;
    // Bins: 16B..64B, 64B..256B, 256B..1KB, 1KB..4KB, 4KB..16KB, 16KB..64KB, 64KB..256KB, >256KB

    struct FragmentationReport {
        float  shannon_entropy{ 0.0f };       // H(X) in bits (0.0 = uniform/perfect, > 2.5 = high dispersal)
        float  fragmentation_score{ 0.0f };   // Normalized 0.0 to 1.0 (where > 0.35 triggers compaction)
        size_t total_free_holes{ 0 };
        size_t total_free_bytes{ 0 };
        size_t largest_hole_bytes{ 0 };
        size_t smallest_hole_bytes{ 0 };
    };

    static FragmentationReport analyze_fragmentation(const uintptr_t* hole_starts,
                                                     const size_t* hole_sizes,
                                                     size_t hole_count,
                                                     size_t total_arena_capacity) noexcept;
};

/* ========================================================================= */
/* 4. PAGE RESIDENT PROPAGATION BARRIER                                      */
/* ========================================================================= */

class RedjetRMRPBarrier {
public:
    static constexpr size_t MAX_TRACKED_PAGES = 4096;

private:
    RedjetPagePropagationEntry _entries[MAX_TRACKED_PAGES];
    std::atomic<size_t>        _entry_count{ 0 };
    hs_ticket_lock_t           _barrier_lock;

    std::atomic<uint64_t>      _barrier_hits{ 0 };
    std::atomic<uint64_t>      _cold_transitions{ 0 };

public:
    RedjetRMRPBarrier();

    // Registers a memory page under resident propagation monitoring
    void register_page(uintptr_t page_addr, size_t page_sz);

    // Barrier touch: marks page hot when mutator touches cross-page reference
    void on_page_touch(uintptr_t addr) noexcept;

    // Periodic decay sweep: identifies cold pages for resident trimming
    size_t decay_and_classify_cold_pages(uint32_t decay_amount);

    // Decommits cold pages from OS RSS
    size_t decommit_cold_pages(size_t target_reclaim_bytes);

    // Evaluates resident bloat based on cold/idle page ratio and total cold bytes
    RedjetRMRPResidentBloatSignal evaluate_bloat_signal(float threshold_ratio = 0.25f,
                                                        size_t min_cold_bytes = 16 * 1024 * 1024) const noexcept;

    // Checks if address is cold
    bool is_page_cold(uintptr_t addr) const noexcept;

    uint64_t barrier_hits() const noexcept { return _barrier_hits.load(std::memory_order_relaxed); }
    uint64_t cold_transitions() const noexcept { return _cold_transitions.load(std::memory_order_relaxed); }
    size_t tracked_page_count() const noexcept { return _entry_count.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 5. ZERO-PAGE DEDUPLICATOR (AVX-512 / AVX2 / Scalar)                       */
/* ========================================================================= */

class RedjetRMRPDeduplicator {
private:
    std::atomic<uint64_t> _total_zero_pages_detected{ 0 };
    std::atomic<uint64_t> _total_zero_bytes_reclaimed{ 0 };
    std::atomic<uint64_t> _scan_operations{ 0 };
    std::atomic<uint64_t> _simd512_scans{ 0 };

public:
    RedjetRMRPDeduplicator();

    // Inspects page memory for all-zero content using 512-bit AVX-512 vector comparison
    // (_mm512_test_epi64_mask), AVX2, and 64-bit scalar loop fallback.
    static bool is_page_all_zero(const void* page_addr, size_t page_sz) noexcept;

    // Scans range, decommits any all-zero pages, returns reclaimed byte count
    size_t deduplicate_zero_pages_in_range(void* base_addr, size_t range_bytes, size_t page_sz = 4096);

    uint64_t zero_pages_detected() const noexcept { return _total_zero_pages_detected.load(std::memory_order_relaxed); }
    uint64_t zero_bytes_reclaimed() const noexcept { return _total_zero_bytes_reclaimed.load(std::memory_order_relaxed); }
    uint64_t simd512_scans() const noexcept { return _simd512_scans.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 6. PLUMP, SEQUENTIAL & VFS COMPACTOR                                      */
/* ========================================================================= */

class RedjetRMRPCompactor {
private:
    std::atomic<uint64_t>      _plump_cycles{ 0 };
    std::atomic<uint64_t>      _sequential_cycles{ 0 };
    std::atomic<uint64_t>      _vfs_compaction_cycles{ 0 };
    std::atomic<uint64_t>      _coalesced_hole_bytes{ 0 };

    RedjetFreeSpanIntervalTree _interval_tree;
    RedjetBuddyCoalescer       _buddy_coalescer;

public:
    RedjetRMRPCompactor();

    // Plump Compaction: bulk coalescing of sparse/idle slabs and physical OS decommit
    size_t execute_plump_compaction(void* arena_base, size_t arena_capacity, size_t target_reclaim_bytes);

    // Sequential Compaction: linear span trimming on trailing cold pages
    size_t execute_sequential_compaction(void* arena_base, size_t arena_capacity, size_t target_reclaim_bytes);

    // Autonomous VFS Compaction: coalesces holes across VFS nodes and decommits cold frames
    size_t execute_vfs_compaction(void* arena_base, size_t target_reclaim_bytes);

    uint64_t plump_cycles() const noexcept { return _plump_cycles.load(std::memory_order_relaxed); }
    uint64_t sequential_cycles() const noexcept { return _sequential_cycles.load(std::memory_order_relaxed); }
    uint64_t vfs_compaction_cycles() const noexcept { return _vfs_compaction_cycles.load(std::memory_order_relaxed); }
    uint64_t coalesced_hole_bytes() const noexcept { return _coalesced_hole_bytes.load(std::memory_order_relaxed); }
};

/* ========================================================================= */
/* 7. REDJET RMRP SUBSYSTEM NEXUS                                            */
/* ========================================================================= */

class RedjetRMRP {
private:
  static std::atomic<uint64_t> _total_plump_reclaimed_bytes;
  static std::atomic<uint64_t> _total_sequential_reclaimed_bytes;
  static std::atomic<uint64_t> _total_vfs_reclaimed_bytes;
  static std::atomic<uint32_t> _total_reclamation_cycles;
  static std::atomic<uint32_t> _total_vfs_compaction_triggers;

  static RedjetRMRPBarrier      _barrier;
  static RedjetRMRPDeduplicator _deduplicator;
  static RedjetRMRPCompactor    _compactor;

public:
  // Executes resident set reclamation under specified mode
  RJ_API static size_t execute_reclamation(hs_rmrp_method_e method, size_t target_bytes);

  // Plump: Bulk coalesce of idle slabs and physical memory decommit
  RJ_API static size_t execute_plump_reclamation(size_t target_bytes);

  // Sequential: Span trimming on cold, aged runway and landfill pages
  RJ_API static size_t execute_sequential_trimming(size_t target_bytes);

  // Autonomous VFS Compaction Trigger:
  // Evaluates 3 signals: Fragmentation Score (>35% or threshold), RMRP cold-page resident bloat signal,
  // and MLTC bus traffic pacing lull window.
  RJ_API static RedjetRMRPResidentBloatSignal check_resident_bloat(float threshold_ratio = 0.25f);
  RJ_API static RedjetVFSCompactionTriggerResult evaluate_and_trigger_vfs_compaction(
      float fragmentation_score, bool mltc_lull_active, size_t target_reclaim_bytes = 64 * 1024 * 1024);

  // Zero-Page Deduplication: Scans memory range and decommits zero pages
  RJ_API static size_t deduplicate_zero_pages(void* base_addr, size_t bytes, size_t page_sz = 4096);

  // Barrier Accessors
  RJ_API static void register_monitored_page(uintptr_t page_addr, size_t page_sz);
  RJ_API static void on_barrier_touch(uintptr_t addr) noexcept;
  RJ_API static size_t sweep_cold_pages(uint32_t decay_amount);

  // Statistics
  RJ_API static uint64_t total_plump_reclaimed() { return _total_plump_reclaimed_bytes.load(std::memory_order_relaxed); }
  RJ_API static uint64_t total_sequential_reclaimed() { return _total_sequential_reclaimed_bytes.load(std::memory_order_relaxed); }
  RJ_API static uint64_t total_vfs_reclaimed() { return _total_vfs_reclaimed_bytes.load(std::memory_order_relaxed); }
  RJ_API static uint32_t total_cycles() { return _total_reclamation_cycles.load(std::memory_order_relaxed); }
  RJ_API static uint32_t total_vfs_compactions() { return _total_vfs_compaction_triggers.load(std::memory_order_relaxed); }
  RJ_API static uint64_t total_zero_bytes_deduplicated() { return _deduplicator.zero_bytes_reclaimed(); }
  RJ_API static RedjetRMRPBarrier& barrier() noexcept { return _barrier; }
  RJ_API static RedjetRMRPDeduplicator& deduplicator() noexcept { return _deduplicator; }
  RJ_API static RedjetRMRPCompactor& compactor() noexcept { return _compactor; }
};

using RedjetRMRPSubsystem = RedjetRMRP;

#endif // SHARE_GC_REDJET_ALLOCATOR_REDJET_RMRP_HPP
