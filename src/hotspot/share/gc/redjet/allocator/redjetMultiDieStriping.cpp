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

#include "gc/redjet/allocator/redjetMultiDieStriping.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef REDJET_STANDALONE_BUILD
#include "runtime/os.hpp"
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#endif

RedjetMultiDieStriping* RedjetMultiDieStriping::_instance = nullptr;
std::atomic<bool> RedjetMultiDieStriping::_initialized{ false };

RedjetMultiDieStriping::RedjetMultiDieStriping()
  : _num_channels(12),
    _next_channel_seq(0),
    _granularity(RedjetStripeGranularity::GRANULARITY_256KB),
    _total_stripes_allocated(0),
    _total_bytes_striped(0),
    _total_channel_rotations(0),
    _channel_imbalances_mitigated(0) {
  hs_ticket_lock_init(&_striping_lock);
  for (size_t i = 0; i < MAX_CHANNELS; ++i) {
    _channels[i].channel_id = static_cast<uint32_t>(i);
    _channels[i].numa_node = static_cast<uint32_t>(i / 4); // 4 channels per NUMA node default
    _channels[i].bytes_allocated.store(0, std::memory_order_relaxed);
    _channels[i].active_stripes.store(0, std::memory_order_relaxed);
    _channels[i].bytes_transferred.store(0, std::memory_order_relaxed);
    _channels[i].measured_bandwidth_gbps = 0.0;
  }
}

RedjetMultiDieStriping::~RedjetMultiDieStriping() {
}

void RedjetMultiDieStriping::initialize(size_t channels) {
  bool expected = false;
  if (_initialized.compare_exchange_strong(expected, true)) {
    _instance = new RedjetMultiDieStriping();
    _instance->_num_channels = std::max<size_t>(1, std::min<size_t>(channels, MAX_CHANNELS));
  }
}

void RedjetMultiDieStriping::destroy() {
  if (_initialized.exchange(false)) {
    delete _instance;
    _instance = nullptr;
  }
}

void RedjetMultiDieStriping::configure_topology(size_t active_channels, const uint32_t* numa_mapping) {
  hs_ticket_lock_acquire(&_striping_lock);
  _num_channels = std::max<size_t>(1, std::min<size_t>(active_channels, MAX_CHANNELS));

  for (size_t i = 0; i < _num_channels; ++i) {
    _channels[i].channel_id = static_cast<uint32_t>(i);
    if (numa_mapping != nullptr) {
      _channels[i].numa_node = numa_mapping[i];
    } else {
      _channels[i].numa_node = static_cast<uint32_t>(i / 4);
    }
  }
  hs_ticket_lock_release(&_striping_lock);
}

size_t RedjetMultiDieStriping::stripe_page_bytes() const noexcept {
  switch (_granularity) {
    case RedjetStripeGranularity::GRANULARITY_64KB:  return 64 * 1024;
    case RedjetStripeGranularity::GRANULARITY_128KB: return 128 * 1024;
    case RedjetStripeGranularity::GRANULARITY_256KB: return 256 * 1024;
    case RedjetStripeGranularity::GRANULARITY_1MB:   return 1024 * 1024;
    default:                                         return 256 * 1024;
  }
}

uint32_t RedjetMultiDieStriping::select_next_channel() noexcept {
  // Round-robin with least-loaded bias
  uint32_t seq = _next_channel_seq.fetch_add(1, std::memory_order_relaxed);
  uint32_t primary = seq % _num_channels;

  // Check if primary channel is significantly overloaded compared to adjacent channel
  size_t primary_alloc = _channels[primary].bytes_allocated.load(std::memory_order_relaxed);
  uint32_t alternate = (primary + 1) % _num_channels;
  size_t alt_alloc = _channels[alternate].bytes_allocated.load(std::memory_order_relaxed);

  if (primary_alloc > alt_alloc + (stripe_page_bytes() * 4)) {
    _channel_imbalances_mitigated.fetch_add(1, std::memory_order_relaxed);
    primary = alternate;
  }

  _total_channel_rotations.fetch_add(1, std::memory_order_relaxed);
  return primary;
}

void RedjetMultiDieStriping::record_stripe_allocation(uint32_t channel_id, size_t bytes) noexcept {
  if (channel_id >= _num_channels) return;

  _channels[channel_id].bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
  _channels[channel_id].active_stripes.fetch_add(1, std::memory_order_relaxed);
  _channels[channel_id].bytes_transferred.fetch_add(bytes, std::memory_order_relaxed);

  _total_stripes_allocated.fetch_add(1, std::memory_order_relaxed);
  _total_bytes_striped.fetch_add(bytes, std::memory_order_relaxed);
}

void RedjetMultiDieStriping::record_stripe_release(uint32_t channel_id, size_t bytes) noexcept {
  if (channel_id >= _num_channels) return;

  size_t cur = _channels[channel_id].bytes_allocated.load(std::memory_order_relaxed);
  if (cur >= bytes) {
    _channels[channel_id].bytes_allocated.fetch_sub(bytes, std::memory_order_relaxed);
  } else {
    _channels[channel_id].bytes_allocated.store(0, std::memory_order_relaxed);
  }

  size_t s = _channels[channel_id].active_stripes.load(std::memory_order_relaxed);
  if (s > 0) {
    _channels[channel_id].active_stripes.fetch_sub(1, std::memory_order_relaxed);
  }
}

bool RedjetMultiDieStriping::is_channel_balanced() const noexcept {
  if (_num_channels <= 1) return true;

  size_t min_bytes = _channels[0].bytes_allocated.load(std::memory_order_relaxed);
  size_t max_bytes = min_bytes;

  for (size_t i = 1; i < _num_channels; ++i) {
    size_t b = _channels[i].bytes_allocated.load(std::memory_order_relaxed);
    if (b < min_bytes) min_bytes = b;
    if (b > max_bytes) max_bytes = b;
  }

  // Balanced if max is within 25% of min or diff < 4MB
  size_t diff = max_bytes - min_bytes;
  if (diff < 4 * 1024 * 1024) return true;
  return (min_bytes > 0) && (diff * 4 < min_bytes);
}

void RedjetMultiDieStriping::print_on(outputStream* st) const {
  if (st == nullptr) return;

  st->print_cr("--- Redjet Multi-Die Memory Striping Coordinator ---");
  st->print_cr("  Active DRAM Channels:    %zu", _num_channels);
  st->print_cr("  Stripe Granularity:      %zu KB", stripe_page_bytes() / 1024);
  st->print_cr("  Total Stripes Allocated: %llu", static_cast<unsigned long long>(_total_stripes_allocated.load(std::memory_order_relaxed)));
  st->print_cr("  Total Bytes Striped:     %llu bytes (%llu MB)",
               static_cast<unsigned long long>(_total_bytes_striped.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(_total_bytes_striped.load(std::memory_order_relaxed) / (1024 * 1024)));
  st->print_cr("  Channel Rotations:       %llu", static_cast<unsigned long long>(_total_channel_rotations.load(std::memory_order_relaxed)));
  st->print_cr("  Imbalances Mitigated:    %llu", static_cast<unsigned long long>(_channel_imbalances_mitigated.load(std::memory_order_relaxed)));
  st->print_cr("  Bus Balanced:            %s", is_channel_balanced() ? "true" : "false");

  for (size_t i = 0; i < _num_channels; ++i) {
    st->print_cr("    Channel [%02zu] NUMA Node %u: allocated=%zu KB, active_stripes=%zu",
                 i, _channels[i].numa_node,
                 _channels[i].bytes_allocated.load(std::memory_order_relaxed) / 1024,
                 _channels[i].active_stripes.load(std::memory_order_relaxed));
  }
}
