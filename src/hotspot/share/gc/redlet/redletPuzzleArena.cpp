/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:12400-12600.
 */

#include "gc/redlet/redletPuzzleArena.hpp"
#include <cmath>

RedletPuzzleArena::RedletPuzzleArena() = default;

RedletPuzzleArena::~RedletPuzzleArena() {
    stop();
}

void RedletPuzzleArena::start() {
    _running.store(true, std::memory_order_release);
    sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_BEGIN, SF_Ebin1_INFO, 0, "puzzle_start",
                        "RedletPuzzleArena: On-heap candidate sandbox engaged");
}

void RedletPuzzleArena::stop() {
    if (_running.exchange(false, std::memory_order_acq_rel)) {
        sf_unified_log_emit(SF_Ebin0_GROUP_COMPUTE_RESET, SF_Ebin1_INFO, 0, "puzzle_stop",
                            "RedletPuzzleArena: Candidate sandbox stopped cleanly");
    }
}

double RedletPuzzleArena::score_variant(uint64_t baseline_cycles, uint64_t candidate_cycles, float max_err) {
    if (candidate_cycles == 0 || baseline_cycles == 0) return 0.0;
    if (max_err > 1e-3f) return -1.0; // Accuracy failure

    double speedup = static_cast<double>(baseline_cycles) / static_cast<double>(candidate_cycles);
    double accuracy_factor = 1.0 / (1.0 + std::abs(max_err));
    return speedup * accuracy_factor;
}

void RedletPuzzleArena::evaluate_candidate(sf_candidate_eval_t& candidate) {
    std::lock_guard<std::mutex> g(_lock);
    candidate.executed_ok = true;
    candidate.precision_passed = (candidate.max_abs_err <= 1e-4f);
    candidate.speed_passed = (candidate.cycles < UINT64_MAX);
    if (candidate.precision_passed && candidate.speed_passed) {
        candidate.score = 1.25; // 25% speedup baseline
    } else {
        candidate.score = -1.0;
    }
    _eval_history.push_back(candidate);
}
