/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * RedletGC: High-Performance Generative & Vectorized Garbage Collector.
 * Ported from SacFor_VM.cpp:12400-12600.
 */

#ifndef SHARE_GC_REDLET_REDLETPUZZLEARENA_HPP
#define SHARE_GC_REDLET_REDLETPUZZLEARENA_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <mutex>
#include "gc/redlet/redletGearkit.hpp"

enum class sf_puzzle_execution_mode_e : uint8_t {
    ADAPTIVE = 0,
    STRICT   = 1
};

struct sf_candidate_eval_t {
    int      idx{ -1 };
    char     variant_name[32]{ 0 };
    char     isa_tag[16]{ 0 };
    bool     executed_ok{ false };
    uint64_t cycles{ UINT64_MAX };
    float    max_abs_err{ 1e9f };
    float    mse{ 1e9f };
    bool     precision_passed{ false };
    bool     speed_passed{ false };
    double   score{ -1.0 };
};

/* ========================================================================= */
/* REDLET PUZZLE ARENA (sacculus_puzzle_arena_t)                             */
/* ========================================================================= */

class RedletPuzzleArena {
private:
    std::atomic<bool>    _running{ false };
    std::atomic<uint8_t> _execution_mode{ static_cast<uint8_t>(sf_puzzle_execution_mode_e::ADAPTIVE) };
    std::atomic<size_t>  _stream_sample_count{ 5 };
    std::mutex           _lock;
    std::vector<sf_candidate_eval_t> _eval_history;

public:
    RedletPuzzleArena();
    ~RedletPuzzleArena();

    void start();
    void stop();

    bool is_running() const { return _running.load(std::memory_order_relaxed); }
    void evaluate_candidate(sf_candidate_eval_t& candidate);
    double score_variant(uint64_t baseline_cycles, uint64_t candidate_cycles, float max_err);
    size_t history_count() const { return _eval_history.size(); }
};

using sacculus_puzzle_arena_t = RedletPuzzleArena;

#endif // SHARE_GC_REDLET_REDLETPUZZLEARENA_HPP
