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

#include "gc/redjet/runtime/redjetBPUDirector.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

RedjetBPUDirector g_redjet_bpu_director;

RedjetBPUDirector& RedjetBPUDirector::instance() noexcept {
  return g_redjet_bpu_director;
}

RedjetBPUDirector::RedjetBPUDirector() noexcept {
  reset();
}

void RedjetBPUDirector::reset() noexcept {
  // Initialize bimodal table to weakly taken (2 in [0..3]) — most non-escaping local scopes die cleanly
  std::memset(_bimodal, 2, sizeof(_bimodal));
  std::memset(_tage, 0, sizeof(_tage));
  std::memset(_perceptrons, 0, sizeof(_perceptrons));
  for (size_t i = 0; i < PERCEPTRON_COUNT; ++i) {
    _perceptrons[i][0] = 4; // Mild positive bias toward clean local death
  }
  _ghr_lo.store(0xA5A55A5AA5A55A5AULL, std::memory_order_relaxed);
  _ghr_hi.store(0x5A5AA5A55A5AA5A5ULL, std::memory_order_relaxed);
  _total_predictions.store(0, std::memory_order_relaxed);
  _correct_predictions.store(0, std::memory_order_relaxed);
  _tage_hits.store(0, std::memory_order_relaxed);
  _tailtrace_fast_grants.store(0, std::memory_order_relaxed);
  _tailtrace_escape_vetoes.store(0, std::memory_order_relaxed);
}

static inline uint64_t fold_history(uint64_t ghr_lo, uint64_t ghr_hi, int hist_len, int out_bits) noexcept {
  uint64_t acc = 0;
  const uint64_t mask = (1ULL << out_bits) - 1ULL;
  int remaining = hist_len;
  uint64_t cur = ghr_lo;
  int bit_pos = 0;
  while (remaining > 0) {
    int chunk = (remaining < out_bits) ? remaining : out_bits;
    uint64_t slice = cur & ((1ULL << chunk) - 1ULL);
    acc ^= slice;
    remaining -= chunk;
    bit_pos += chunk;
    if (bit_pos >= 64) {
      cur = ghr_hi >> (bit_pos - 64);
    } else {
      cur = ghr_lo >> bit_pos;
    }
  }
  return acc & mask;
}

uint32_t RedjetBPUDirector::compute_tage_index(uint64_t pc, int bank, uint64_t ghr_lo, uint64_t ghr_hi) const noexcept {
  uint64_t pc_mix = (pc >> 2) ^ (pc >> 13) ^ (static_cast<uint64_t>(bank) * 0x9E3779B97F4A7C15ULL);
  uint64_t folded = fold_history(ghr_lo, ghr_hi, HIST_LENGTHS[bank], 10);
  return static_cast<uint32_t>((pc_mix ^ folded) & (TAGE_BANK_ENTRIES - 1));
}

uint16_t RedjetBPUDirector::compute_tage_tag(uint64_t pc, int bank, uint64_t ghr_lo, uint64_t ghr_hi) const noexcept {
  uint64_t f1 = fold_history(ghr_lo, ghr_hi, HIST_LENGTHS[bank], 11);
  uint64_t f2 = fold_history(ghr_lo, ghr_hi, HIST_LENGTHS[bank], 9);
  uint64_t raw = (pc >> 3) ^ f1 ^ (f2 << 2);
  uint16_t tag = static_cast<uint16_t>(raw & 0x07FFu);
  return (tag == 0u) ? 1u : tag; // Reserve 0 as empty tag
}

int32_t RedjetBPUDirector::compute_perceptron_sum(uint64_t pc, uint64_t ghr_lo) const noexcept {
  size_t idx = static_cast<size_t>(((pc >> 2) ^ (ghr_lo & 0xFFULL)) & (PERCEPTRON_COUNT - 1));
  const int8_t* w = _perceptrons[idx];
  int32_t sum = static_cast<int32_t>(w[0]); // Bias weight
  for (size_t i = 0; i < PERCEPTRON_HIST; ++i) {
    int32_t hist_bipolar = ((ghr_lo >> i) & 1ULL) ? 1 : -1;
    sum += static_cast<int32_t>(w[i + 1]) * hist_bipolar;
  }
  return sum;
}

void RedjetBPUDirector::push_history_bit(bool taken) noexcept {
  uint64_t lo = _ghr_lo.load(std::memory_order_relaxed);
  uint64_t hi = _ghr_hi.load(std::memory_order_relaxed);
  uint64_t carry = (lo >> 63) & 1ULL;
  uint64_t next_hi = (hi << 1) | carry;
  uint64_t next_lo = (lo << 1) | (taken ? 1ULL : 0ULL);
  _ghr_hi.store(next_hi, std::memory_order_relaxed);
  _ghr_lo.store(next_lo, std::memory_order_relaxed);
}

int RedjetBPUDirector::predict_only(uint64_t branch_pc, uint8_t* out_confidence_pct) const noexcept {
  uint64_t ghr_lo = _ghr_lo.load(std::memory_order_relaxed);
  uint64_t ghr_hi = _ghr_hi.load(std::memory_order_relaxed);

  // 1. Longest matching TAGE bank search
  int provider_bank = -1;
  int alt_bank      = -1;
  uint32_t indices[NUM_TAGE_BANKS];
  uint16_t tags[NUM_TAGE_BANKS];

  for (int b = static_cast<int>(NUM_TAGE_BANKS) - 1; b >= 0; --b) {
    indices[b] = compute_tage_index(branch_pc, b, ghr_lo, ghr_hi);
    tags[b]    = compute_tage_tag(branch_pc, b, ghr_lo, ghr_hi);
    if (_tage[b][indices[b]].tag == tags[b]) {
      if (provider_bank < 0) {
        provider_bank = b;
      } else if (alt_bank < 0) {
        alt_bank = b;
      }
    }
  }

  uint32_t bim_idx = static_cast<uint32_t>((branch_pc >> 2) & (BIMODAL_SIZE - 1));
  bool bim_pred = (_bimodal[bim_idx] >= 2);

  bool base_pred = bim_pred;
  int  tage_conf = ( _bimodal[bim_idx] == 3 || _bimodal[bim_idx] == 0 ) ? 72 : 55;

  if (provider_bank >= 0) {
    int8_t ctr = _tage[provider_bank][indices[provider_bank]].pred;
    // Weak newly-allocated TAGE entry (ctr == 0 or -1 with u == 0) defers to alternate predictor
    if ((ctr == 0 || ctr == -1) && _tage[provider_bank][indices[provider_bank]].u == 0) {
      if (alt_bank >= 0) {
        base_pred = (_tage[alt_bank][indices[alt_bank]].pred >= 0);
      }
      tage_conf = 62;
    } else {
      base_pred = (ctr >= 0);
      int mag = (ctr >= 0) ? (ctr + 1) : (-ctr); // [1..4]
      tage_conf = 60 + mag * 9;                  // [69..96]
    }
  }

  // 2. Fuse Hashed Perceptron neural liveness score
  int32_t p_sum = compute_perceptron_sum(branch_pc, ghr_lo);
  bool perc_pred = (p_sum >= 0);
  int32_t abs_p = (p_sum >= 0) ? p_sum : -p_sum;

  bool final_pred = base_pred;
  int final_conf = tage_conf;
  if (perc_pred == base_pred) {
    final_conf = std::min(99, tage_conf + std::min(8, abs_p / 8));
  } else if (provider_bank < 0 && abs_p > 24) {
    // When TAGE misses and Perceptron has strong linear margin, trust Perceptron
    final_pred = perc_pred;
    final_conf = std::min(90, 60 + abs_p / 4);
  } else {
    final_conf = std::max(50, tage_conf - 10);
  }

  if (out_confidence_pct != nullptr) {
    *out_confidence_pct = static_cast<uint8_t>(final_conf);
  }
  return final_pred ? 1 : 0;
}

int RedjetBPUDirector::predict_and_update(uint64_t branch_pc, int actual_taken) noexcept {
  uint8_t conf = 0;
  int pred = predict_only(branch_pc, &conf);
  bool outcome = (actual_taken != 0);

  _total_predictions.fetch_add(1, std::memory_order_relaxed);
  if ((pred != 0) == outcome) {
    _correct_predictions.fetch_add(1, std::memory_order_relaxed);
  }

  hs_ticket_guard_t guard(_update_lock);
  uint64_t ghr_lo = _ghr_lo.load(std::memory_order_relaxed);
  uint64_t ghr_hi = _ghr_hi.load(std::memory_order_relaxed);

  // Update Bimodal base table
  uint32_t bim_idx = static_cast<uint32_t>((branch_pc >> 2) & (BIMODAL_SIZE - 1));
  if (outcome) {
    if (_bimodal[bim_idx] < 3) _bimodal[bim_idx]++;
  } else {
    if (_bimodal[bim_idx] > 0) _bimodal[bim_idx]--;
  }

  // Locate TAGE provider & alternate
  int provider_bank = -1;
  int alt_bank      = -1;
  uint32_t indices[NUM_TAGE_BANKS];
  uint16_t tags[NUM_TAGE_BANKS];
  for (int b = static_cast<int>(NUM_TAGE_BANKS) - 1; b >= 0; --b) {
    indices[b] = compute_tage_index(branch_pc, b, ghr_lo, ghr_hi);
    tags[b]    = compute_tage_tag(branch_pc, b, ghr_lo, ghr_hi);
    if (_tage[b][indices[b]].tag == tags[b]) {
      if (provider_bank < 0) provider_bank = b;
      else if (alt_bank < 0) alt_bank = b;
    }
  }

  if (provider_bank >= 0) {
    _tage_hits.fetch_add(1, std::memory_order_relaxed);
    RedjetTAGEEntry& e = _tage[provider_bank][indices[provider_bank]];
    bool prov_pred = (e.pred >= 0);
    bool alt_pred  = (alt_bank >= 0) ? (_tage[alt_bank][indices[alt_bank]].pred >= 0) : (_bimodal[bim_idx] >= 2);
    if (prov_pred != alt_pred) {
      if (prov_pred == outcome) {
        if (e.u < 3) e.u++;
      } else {
        if (e.u > 0) e.u--;
      }
    }
    if (outcome) {
      if (e.pred < 3) e.pred++;
    } else {
      if (e.pred > -4) e.pred--;
    }
  }

  // Allocate longer-history TAGE entry on misprediction
  if ((pred != 0) != outcome) {
    int start_bank = (provider_bank >= 0) ? (provider_bank + 1) : 0;
    bool allocated = false;
    for (int b = start_bank; b < static_cast<int>(NUM_TAGE_BANKS); ++b) {
      RedjetTAGEEntry& cand = _tage[b][indices[b]];
      if (cand.u == 0) {
        cand.tag  = tags[b];
        cand.pred = outcome ? 0 : -1;
        cand.u    = 0;
        allocated = true;
        break;
      }
    }
    if (!allocated) {
      for (int b = start_bank; b < static_cast<int>(NUM_TAGE_BANKS); ++b) {
        if (_tage[b][indices[b]].u > 0) {
          _tage[b][indices[b]].u--;
        }
      }
    }
  }

  // Train Hashed Perceptron if mispredicted or within training theta (theta = 1.93*H + 14 = 45)
  int32_t p_sum = compute_perceptron_sum(branch_pc, ghr_lo);
  if (((p_sum >= 0) != outcome) || (std::abs(p_sum) <= 45)) {
    size_t pidx = static_cast<size_t>(((branch_pc >> 2) ^ (ghr_lo & 0xFFULL)) & (PERCEPTRON_COUNT - 1));
    int8_t* w = _perceptrons[pidx];
    int target = outcome ? 1 : -1;
    int32_t next_b = static_cast<int32_t>(w[0]) + target;
    w[0] = static_cast<int8_t>(std::max(-127, std::min(127, next_b)));
    for (size_t i = 0; i < PERCEPTRON_HIST; ++i) {
      int hist_bipolar = ((ghr_lo >> i) & 1ULL) ? 1 : -1;
      int delta = target * hist_bipolar;
      int32_t nw = static_cast<int32_t>(w[i + 1]) + delta;
      w[i + 1] = static_cast<int8_t>(std::max(-127, std::min(127, nw)));
    }
  }

  push_history_bit(outcome);
  return pred;
}

RedjetBPULivenessDecision RedjetBPUDirector::evaluate_tailtrace_clasp(uint64_t callsite_or_region_hash,
                                                                      uint32_t thread_id,
                                                                      size_t object_bytes) noexcept {
  // Mix callsite/region hash with thread role & size bucket so BPU learns per-allocation-site escape behavior
  uint32_t sz_bucket = (object_bytes <= 64) ? 0u : (object_bytes <= 512 ? 1u : (object_bytes <= 4096 ? 2u : 3u));
  uint64_t synthetic_pc = (callsite_or_region_hash * 0x9E3779B97F4A7C15ULL) ^
                          (static_cast<uint64_t>(thread_id & 0x3Fu) << 4) ^
                          (static_cast<uint64_t>(sz_bucket) << 2);

  uint8_t conf = 0;
  int pred = predict_only(synthetic_pc, &conf);

  RedjetBPULivenessDecision dec{};
  dec.predicted_truly_dead    = (pred != 0);
  dec.confidence_percent      = conf;
  dec.bypass_long_probation   = dec.predicted_truly_dead && (conf >= 94);
  dec.emit_likely_fallthrough = dec.predicted_truly_dead;

  if (dec.bypass_long_probation) {
    _tailtrace_fast_grants.fetch_add(1, std::memory_order_relaxed);
  } else if (!dec.predicted_truly_dead && conf >= 75) {
    _tailtrace_escape_vetoes.fetch_add(1, std::memory_order_relaxed);
  }
  return dec;
}

void RedjetBPUDirector::record_tailtrace_outcome(uint64_t callsite_or_region_hash,
                                                 uint32_t thread_id,
                                                 bool was_truly_dead) noexcept {
  uint64_t synthetic_pc = (callsite_or_region_hash * 0x9E3779B97F4A7C15ULL) ^
                          (static_cast<uint64_t>(thread_id & 0x3Fu) << 4);
  (void)predict_and_update(synthetic_pc, was_truly_dead ? 1 : 0);
}

#ifndef REDJET_STANDALONE_BUILD
void RedjetBPUDirector::print_on(outputStream* st) const {
  if (st == nullptr) return;
  st->print_cr("Redjet BPU Symbiotic Liveness Director (TAGE + Hashed Perceptron):");
  st->print_cr("  Predictions:             %llu (Correct: %llu, Accuracy: %.2f%%)",
               static_cast<unsigned long long>(total_predictions()),
               static_cast<unsigned long long>(correct_predictions()),
               accuracy_percent());
  st->print_cr("  TAGE Bank Hits:          %llu | TailTraces Fast-Reclaim Grants: %llu | Escape Vetoes: %llu",
               static_cast<unsigned long long>(tage_hits()),
               static_cast<unsigned long long>(tailtrace_fast_grants()),
               static_cast<unsigned long long>(tailtrace_escape_vetoes()));
}
#endif

extern "C" {

RJ_API int HS_hotspot_branch_predict(uint64_t branch_pc, int actual_taken) {
  return RedjetBPUDirector::instance().predict_and_update(branch_pc, actual_taken);
}

RJ_API double HS_hotspot_bpu_accuracy_percent(void) {
  return RedjetBPUDirector::instance().accuracy_percent();
}

} // extern "C"
