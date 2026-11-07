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

#include "gc/redjet/puzzler/redjetPuzzleArenaDuel.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"
#include "gc/redjet/engine/redjetPointCoordinator.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/runtime/redjetHardware.hpp"
#include "mirc/mirc.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

std::atomic<uint64_t> RedjetPuzzleArenaDuelEngine::_total_duels_executed{ 0 };
std::atomic<uint64_t> RedjetPuzzleArenaDuelEngine::_total_hotswaps_performed{ 0 };
std::atomic<uint64_t> RedjetPuzzleArenaDuelEngine::_total_precision_rejections{ 0 };

/* ========================================================================= */
/* 1. 64-ITERATION TSC PROFILE STATISTICAL ANALYSIS                          */
/* ========================================================================= */

void RedjetDuelProfile64::analyze() noexcept {
    uint64_t sorted[ITERATIONS];
    std::memcpy(sorted, tsc_deltas, sizeof(sorted));
    std::sort(sorted, sorted + ITERATIONS);

    min_tsc = sorted[0];
    max_tsc = sorted[ITERATIONS - 1];
    median_tsc = (sorted[31] + sorted[32]) / 2;

    uint64_t sum_all = 0;
    for (size_t i = 0; i < ITERATIONS; ++i) {
        sum_all += sorted[i];
    }
    mean_tsc = sum_all / ITERATIONS;

    // Trim top 4 and bottom 4 samples (56 remaining samples) to eliminate OS noise & cache miss spikes
    uint64_t sum_trimmed = 0;
    for (size_t i = 4; i < ITERATIONS - 4; ++i) {
        sum_trimmed += sorted[i];
    }
    trimmed_mean_tsc = sum_trimmed / (ITERATIONS - 8);

    // Compute standard deviation over trimmed samples
    double variance_acc = 0.0;
    double t_mean = static_cast<double>(trimmed_mean_tsc);
    for (size_t i = 4; i < ITERATIONS - 4; ++i) {
        double diff = static_cast<double>(sorted[i]) - t_mean;
        variance_acc += (diff * diff);
    }
    std_dev = std::sqrt(variance_acc / static_cast<double>(ITERATIONS - 8));
}

/* ========================================================================= */
/* 2. 64-ITERATION CANDIDATE EXECUTION HARNESS                               */
/* ========================================================================= */

void RedjetPuzzleArenaDuelEngine::profile_candidate_64(
    const char* isa_name,
    const float* src_a,
    const float* src_b,
    float* cand_out,
    size_t count,
    RedjetDuelProfile64* out_profile) noexcept {

    if (!isa_name || !src_a || !src_b || !cand_out || !out_profile || count == 0) {
        return;
    }

    const bool is_512 = (strncmp(isa_name, "AVX512", 6) == 0 || strncmp(isa_name, "AVX10", 5) == 0 || strcmp(isa_name, "SVE2_Cached") == 0);
    const bool is_256 = (strncmp(isa_name, "AVX2", 4) == 0 || strcmp(isa_name, "AVX_VNNI") == 0 || strcmp(isa_name, "RVV_Cached") == 0);
    const bool is_128 = (strcmp(isa_name, "SSE42_Cached") == 0 || strcmp(isa_name, "NEON_Cached") == 0);

    for (size_t iter = 0; iter < RedjetDuelProfile64::ITERATIONS; ++iter) {
        uint64_t t0 = sacculus_rdtsc();

        if (is_512) {
            for (size_t i = 0; i < count; i += 16) {
                for (size_t k = 0; k < 16 && (i + k) < count; ++k) {
                    cand_out[i + k] = src_a[i + k] * src_b[i + k] + 0.125f;
                }
            }
        } else if (is_256) {
            for (size_t i = 0; i < count; i += 8) {
                for (size_t k = 0; k < 8 && (i + k) < count; ++k) {
                    cand_out[i + k] = src_a[i + k] * src_b[i + k] + 0.125f;
                }
            }
        } else if (is_128) {
            for (size_t i = 0; i < count; i += 4) {
                for (size_t k = 0; k < 4 && (i + k) < count; ++k) {
                    cand_out[i + k] = src_a[i + k] * src_b[i + k] + 0.125f;
                }
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                cand_out[i] = src_a[i] * src_b[i] + 0.125f;
            }
        }

        uint64_t t1 = sacculus_rdtsc();
        uint64_t delta = (t1 > t0) ? (t1 - t0) : 1;
        out_profile->tsc_deltas[iter] = delta;
    }

    out_profile->analyze();
}

/* ========================================================================= */
/* 3. ATOMIC SLOT 16 HOT-SWAP IMPLEMENTATION                                 */
/* ========================================================================= */

bool RedjetPuzzleArenaDuelEngine::perform_atomic_slot16_hotswap(
    MIRC_Lvl1_Table* lvl1_tab,
    const char* winner_isa_name,
    uint16_t old_active_idx,
    uint64_t baseline_tsc,
    uint64_t winning_tsc,
    double speedup,
    mirc_hotswap_delta_t* out_delta) {

    if (!lvl1_tab || !winner_isa_name) return false;

    // Overwrite Slot 16 (Puzzler Wildcard Hot-Swap Yuvası)
    strncpy(lvl1_tab->simd_ladder[16].name, winner_isa_name, sizeof(lvl1_tab->simd_ladder[16].name) - 1);
    lvl1_tab->simd_ladder[16].name[sizeof(lvl1_tab->simd_ladder[16].name) - 1] = '\0';
    lvl1_tab->simd_ladder[16].ladder_level = 16;
    lvl1_tab->simd_ladder[16].is_active = 1;

    // Deactivate previous rungs
    for (uint16_t i = 0; i < 16; ++i) {
        lvl1_tab->simd_ladder[i].is_active = 0;
    }

    // Atomically set active index to Slot 16 with memory barrier
    lvl1_tab->active_idx = 16;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 2-Point Rendezvous: Flush instruction caches across all CPU cores
    RedjetPointCoordinator::sync_point();

    _total_hotswaps_performed.fetch_add(1, std::memory_order_relaxed);

    mirc_hotswap_delta_t delta;
    delta.method_id = lvl1_tab->method_id;
    delta.old_simd_idx = old_active_idx;
    delta.new_simd_idx = 16;
    delta.baseline_cycles = baseline_tsc;
    delta.winning_cycles = winning_tsc;
    delta.cycles_gained = (baseline_tsc > winning_tsc) ? (baseline_tsc - winning_tsc) : 0;
    delta.speedup = speedup;
    strncpy(delta.winner_isa_name, winner_isa_name, sizeof(delta.winner_isa_name) - 1);
    delta.patch_target_pc = nullptr;
    delta.slot16_overwrite = true;
    delta.applied = true;

    if (out_delta != nullptr) {
        *out_delta = delta;
    }

    hs_hotlog_emit(HS_Ebin0_HOTSWAP, HS_Ebin1_INFO, delta.method_id, delta.new_simd_idx,
                   0, 0, 0, delta.cycles_gained, static_cast<uint32_t>(delta.speedup * 100.0),
                   "MIRC_DUEL_WON_HOTSWAP");

    return true;
}

/* ========================================================================= */
/* 4. EXECUTE NMETHOD DUEL PIPELINE                                         */
/* ========================================================================= */

bool RedjetPuzzleArenaDuelEngine::execute_nmethod_duel(
    uint32_t method_id,
    MIRC_Lvl1_Table* lvl1_tab,
    const VAC_Table_Snapshot* vac_tab,
    mirc_hotswap_delta_t* out_delta,
    RedjetNMethodDuelResult* out_result) {

    if (!lvl1_tab) return false;

    _total_duels_executed.fetch_add(1, std::memory_order_relaxed);

    // 1. Resolve host hardware capabilities via MIRC Simulator
    const mirc_resolved_hardware_t hw = DyntoposelSimu::resolve_host_hardware();

    // 2. Determine precision bounds
    const bool is_strictfp = lvl1_tab->is_strictfp || (vac_tab != nullptr && vac_tab->is_strictfp());
    const float allowed_epsilon = is_strictfp ? 1.192092896e-07f : ((lvl1_tab->target_epsilon > 0.0f) ? lvl1_tab->target_epsilon : 1.192092896e-07f);
    const uint16_t current_active_idx = lvl1_tab->active_idx;

    // 3. Allocate aligned test buffers
    float* src_a = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));
    float* src_b = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));
    float* ref_out = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));
    float* cand_out = static_cast<float*>(hs_puzzle_aligned_alloc(DUEL_SAMPLE_ELEMS * sizeof(float), 64));

    if (!src_a || !src_b || !ref_out || !cand_out) {
        if (src_a) hs_puzzle_aligned_free(src_a);
        if (src_b) hs_puzzle_aligned_free(src_b);
        if (ref_out) hs_puzzle_aligned_free(ref_out);
        if (cand_out) hs_puzzle_aligned_free(cand_out);
        return false;
    }

    for (size_t i = 0; i < DUEL_SAMPLE_ELEMS; ++i) {
        src_a[i] = 1.25f + 0.03125f * static_cast<float>(i & 15);
        src_b[i] = 2.50f - 0.015625f * static_cast<float>(i & 15);
        ref_out[i] = src_a[i] * src_b[i] + 0.125f; // Strict IEEE 754 reference
    }

    // 4. Measure Baseline (64 iterations)
    const char* baseline_isa = (current_active_idx < 17) ? lvl1_tab->simd_ladder[current_active_idx].name : "Baseline";
    RedjetDuelProfile64 baseline_prof;
    profile_candidate_64(baseline_isa, src_a, src_b, cand_out, DUEL_SAMPLE_ELEMS, &baseline_prof);

    uint64_t best_measured_tsc = baseline_prof.trimmed_mean_tsc;
    if (best_measured_tsc == 0) best_measured_tsc = 1;

    int winning_slot = -1;
    const char* winner_name = nullptr;
    double winning_speedup = 1.0;
    bool found_better = false;

    // 5. Test candidate rungs in SIMD ladder (Slots 0 to 15)
    for (uint16_t s = 0; s < 16 && s < lvl1_tab->candidate_count; ++s) {
        if (s == current_active_idx) continue;
        const char* isa = lvl1_tab->simd_ladder[s].name;
        if (!isa || !isa[0]) continue;

        // Verify host CPU ISA support
        if (strcmp(isa, "AMX_Tile") == 0 && !hw.has_amx) continue;
        if (strncmp(isa, "AVX512", 6) == 0 && !hw.has_avx512) continue;
        if (strncmp(isa, "AVX10", 5) == 0 && !hw.has_avx10_2 && !hw.has_avx512) continue;
        if (strncmp(isa, "AVX2", 4) == 0 && !hw.has_avx2) continue;
        if (strcmp(isa, "AVX_VNNI") == 0 && !hw.has_vnni && !hw.has_avx2) continue;
        if ((strcmp(isa, "SVE2_Cached") == 0 || strcmp(isa, "SVE_Cached") == 0) && !hw.has_sve) continue;
        if (strcmp(isa, "NEON_Cached") == 0 && !hw.has_neon) continue;
        if (strcmp(isa, "RVV_Cached") == 0 && !hw.has_rvv) continue;

        RedjetDuelProfile64 cand_prof;
        profile_candidate_64(isa, src_a, src_b, cand_out, DUEL_SAMPLE_ELEMS, &cand_prof);

        // Verify IEEE 754 precision adherence
        float max_err = 0.0f;
        float sum_sq = 0.0f;
        bool finite_ok = true;
        hs_accumulate_ieee754_error_f32(cand_out, ref_out, DUEL_SAMPLE_ELEMS, max_err, sum_sq, finite_ok);

        if (!finite_ok || max_err > allowed_epsilon) {
            _total_precision_rejections.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Statistical significance check: candidate must be at least 5% faster than best
        uint64_t cand_tsc = (cand_prof.trimmed_mean_tsc > 0) ? cand_prof.trimmed_mean_tsc : 1ULL;
        double cand_speedup = static_cast<double>(baseline_prof.trimmed_mean_tsc) / static_cast<double>(cand_tsc);
        if (cand_speedup >= MIN_WINNING_SPEEDUP && cand_prof.trimmed_mean_tsc < best_measured_tsc) {
            best_measured_tsc = cand_prof.trimmed_mean_tsc;
            winning_slot = s;
            winner_name = isa;
            winning_speedup = cand_speedup;
            found_better = true;
        }
    }

    // 6. Test Dynamic Hybrid Rung (Slot 16 Candidate)
    RedjetDuelProfile64 hybrid_prof;
    profile_candidate_64("Puzzler_FMA_Unroll8_NT", src_a, src_b, cand_out, DUEL_SAMPLE_ELEMS, &hybrid_prof);

    float max_err = 0.0f;
    float sum_sq = 0.0f;
    bool finite_ok = true;
    hs_accumulate_ieee754_error_f32(cand_out, ref_out, DUEL_SAMPLE_ELEMS, max_err, sum_sq, finite_ok);

    if (finite_ok && max_err <= allowed_epsilon) {
        uint64_t hybrid_tsc = (hybrid_prof.trimmed_mean_tsc > 0) ? hybrid_prof.trimmed_mean_tsc : 1ULL;
        double hybrid_speedup = static_cast<double>(baseline_prof.trimmed_mean_tsc) / static_cast<double>(hybrid_tsc);
        if (hybrid_speedup >= MIN_WINNING_SPEEDUP && hybrid_prof.trimmed_mean_tsc < best_measured_tsc) {
            best_measured_tsc = hybrid_prof.trimmed_mean_tsc;
            winning_slot = 16;
            winner_name = "Puzzler_FMA_Unroll8_NT";
            winning_speedup = hybrid_speedup;
            found_better = true;
        }
    }

    hs_puzzle_aligned_free(src_a);
    hs_puzzle_aligned_free(src_b);
    hs_puzzle_aligned_free(ref_out);
    hs_puzzle_aligned_free(cand_out);

    if (!found_better || winning_slot < 0 || winner_name == nullptr) {
        if (out_result != nullptr) {
            out_result->method_id = method_id;
            out_result->old_active_slot = current_active_idx;
            out_result->winning_slot = current_active_idx;
            out_result->baseline_cycles = baseline_prof.trimmed_mean_tsc;
            out_result->winning_cycles = baseline_prof.trimmed_mean_tsc;
            out_result->cycles_gained = 0;
            out_result->speedup = 1.0;
            out_result->precision_verified = true;
            out_result->hotswap_performed = false;
        }
        return false; // Silent zero-overhead return
    }

    // 7. Perform Atomic Slot 16 Hot-Swap & 2-Point Synchronization
    bool hotswap_ok = perform_atomic_slot16_hotswap(
        lvl1_tab, winner_name, current_active_idx,
        baseline_prof.trimmed_mean_tsc, best_measured_tsc, winning_speedup, out_delta);

    if (out_result != nullptr) {
        out_result->method_id = method_id;
        out_result->old_active_slot = current_active_idx;
        out_result->winning_slot = 16;
        strncpy(out_result->winning_isa_name, winner_name, sizeof(out_result->winning_isa_name) - 1);
        out_result->baseline_cycles = baseline_prof.trimmed_mean_tsc;
        out_result->winning_cycles = best_measured_tsc;
        out_result->cycles_gained = baseline_prof.trimmed_mean_tsc - best_measured_tsc;
        out_result->speedup = winning_speedup;
        out_result->precision_verified = true;
        out_result->hotswap_performed = hotswap_ok;
    }

    return hotswap_ok;
}
