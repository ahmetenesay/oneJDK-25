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

#include "gc/redjet/vfs/redjetVFSCrypto.hpp"
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include <cstring>

namespace {

static inline uint64_t crypto_ror64(uint64_t v, int s) noexcept {
    return (v >> s) | (v << (64 - s));
}

static inline uint64_t crypto_rol64(uint64_t v, int s) noexcept {
    return (v << s) | (v >> (64 - s));
}

static inline uint32_t crypto_ror32(uint32_t v, int s) noexcept {
    return (v >> s) | (v << (32 - s));
}

// Precomputed CRC32 Table (IEEE 802.3: 0xEDB88320)
static uint32_t s_crc32_table[256];
static bool     s_crc32_initialized = false;

static void init_crc32_table() noexcept {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
        }
        s_crc32_table[i] = c;
    }
    s_crc32_initialized = true;
}

// Precomputed CRC64 Table (ECMA-182: 0x42F0E1EBA9EA3693ULL)
static uint64_t s_crc64_table[256];
static bool     s_crc64_initialized = false;

static void init_crc64_table() noexcept {
    for (uint64_t i = 0; i < 256; ++i) {
        uint64_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (0x42F0E1EBA9EA3693ULL ^ (c >> 1)) : (c >> 1);
        }
        s_crc64_table[i] = c;
    }
    s_crc64_initialized = true;
}

// SHA-256 Constants
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

} // namespace

const uint64_t RedjetVFSCrypto::DEFAULT_VAC_SECRETS[7] = {
    0x1A2B3C4D5E6F7081ULL, 0x82736451A0B1C2D3ULL,
    0x0F1E2D3C4B5A6978ULL, 0x9876543210ABCDEFULL,
    0xFEEDC0FFEE010203ULL, 0x0405060708090A0BULL,
    0xCAFEBABEDEADBEEFULL
};

const uint64_t RedjetVFSCrypto::DEFAULT_MIRC_SECRETS[7] = {
    0xA1B2C3D4E5F60718ULL, 0x283746150ACBDCEDULL,
    0xF0E1D2C3B4A59687ULL, 0x8967452301BAEDFCULL,
    0xC0FFEEFEED030201ULL, 0x0B0A090807060504ULL,
    0xDEADBEEFCAFEBABEULL
};

const uint64_t RedjetVFSCrypto::KEY_VAC_READ =
    RedjetVFSCrypto::derive_7cipher_key(RedjetVFSCrypto::DEFAULT_VAC_SECRETS);

const uint64_t RedjetVFSCrypto::KEY_MIRC_READ =
    RedjetVFSCrypto::derive_7cipher_key(RedjetVFSCrypto::DEFAULT_MIRC_SECRETS);

const uint64_t RedjetVFSCrypto::KEY_JIT_TRANSITION = 0x9FA8B7C6D5E4F3E2ULL;

std::atomic<uint64_t> RedjetVFSCrypto::_key_derivation_calls{ 0 };
std::atomic<uint64_t> RedjetVFSCrypto::_crypto_checks_passed{ 0 };
std::atomic<uint64_t> RedjetVFSCrypto::_crypto_checks_failed{ 0 };
std::atomic<uint64_t> RedjetVFSCrypto::_tamper_events_detected{ 0 };

uint64_t RedjetVFSCrypto::derive_7cipher_key(const uint64_t K[7]) noexcept {
    if (!K) return 0;
    _key_derivation_calls.fetch_add(1, std::memory_order_relaxed);

    uint64_t term1 = crypto_ror64(K[0] ^ K[1], 13);
    uint64_t term2 = crypto_rol64(K[2] & K[3], 17);
    uint64_t term3 = K[4] + K[5];
    return term1 ^ term2 ^ term3 ^ K[6];
}

uint64_t RedjetVFSCrypto::derive_session_key(const uint64_t K[7], uint32_t session_salt, uint32_t rounds) noexcept {
    if (!K) return 0;
    uint64_t base_key = derive_7cipher_key(K);
    uint64_t l = base_key ^ static_cast<uint64_t>(session_salt);
    uint64_t r = crypto_rol64(base_key, 31) ^ (static_cast<uint64_t>(session_salt) << 32);

    for (uint32_t i = 0; i < rounds; ++i) {
        uint64_t f = crypto_ror64(r ^ K[i % 7], 19) + 0x9E3779B97F4A7C15ULL;
        uint64_t new_r = l ^ f;
        l = r;
        r = new_r;
    }
    return (l << 32) | (r & 0xFFFFFFFFULL);
}

uint32_t RedjetVFSCrypto::compute_crc32(const void* data, size_t len) noexcept {
    if (!s_crc32_initialized) init_crc32_table();
    if (!data || len == 0) return 0;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ s_crc32_table[(crc ^ p[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

uint64_t RedjetVFSCrypto::compute_crc64(const void* data, size_t len) noexcept {
    if (!s_crc64_initialized) init_crc64_table();
    if (!data || len == 0) return 0;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ s_crc64_table[(crc ^ p[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

void RedjetVFSCrypto::compute_sha256(const void* data, size_t len, uint8_t out_hash[32]) noexcept {
    if (!out_hash) return;
    if (!data && len > 0) return;

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t total_bits = len * 8;

    // Process complete 64-byte blocks
    size_t block_count = len / 64;
    for (size_t b = 0; b < block_count; ++b) {
        uint32_t w[64];
        const uint8_t* blk = p + (b * 64);
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(blk[i * 4]) << 24) |
                   (static_cast<uint32_t>(blk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(blk[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(blk[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = crypto_ror32(w[i - 15], 7) ^ crypto_ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = crypto_ror32(w[i - 2], 17) ^ crypto_ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b_v = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], h_v = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = crypto_ror32(e, 6) ^ crypto_ror32(e, 11) ^ crypto_ror32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h_v + S1 + ch + K256[i] + w[i];
            uint32_t S0 = crypto_ror32(a, 2) ^ crypto_ror32(a, 13) ^ crypto_ror32(a, 22);
            uint32_t maj = (a & b_v) ^ (a & c) ^ (b_v & c);
            uint32_t temp2 = S0 + maj;

            h_v = g; g = f; f = e; e = d + temp1;
            d = c; c = b_v; b_v = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b_v; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += h_v;
    }

    // Process final padded block(s)
    uint8_t final_block[128] = { 0 };
    size_t rem = len % 64;
    if (rem > 0) {
        std::memcpy(final_block, p + (block_count * 64), rem);
    }
    final_block[rem] = 0x80;

    size_t final_block_len = (rem < 56) ? 64 : 128;
    for (int i = 0; i < 8; ++i) {
        final_block[final_block_len - 1 - i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }

    size_t extra_blocks = final_block_len / 64;
    for (size_t b = 0; b < extra_blocks; ++b) {
        uint32_t w[64];
        const uint8_t* blk = final_block + (b * 64);
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(blk[i * 4]) << 24) |
                   (static_cast<uint32_t>(blk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(blk[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(blk[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = crypto_ror32(w[i - 15], 7) ^ crypto_ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = crypto_ror32(w[i - 2], 17) ^ crypto_ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b_v = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], h_v = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = crypto_ror32(e, 6) ^ crypto_ror32(e, 11) ^ crypto_ror32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h_v + S1 + ch + K256[i] + w[i];
            uint32_t S0 = crypto_ror32(a, 2) ^ crypto_ror32(a, 13) ^ crypto_ror32(a, 22);
            uint32_t maj = (a & b_v) ^ (a & c) ^ (b_v & c);
            uint32_t temp2 = S0 + maj;

            h_v = g; g = f; f = e; e = d + temp1;
            d = c; c = b_v; b_v = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b_v; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += h_v;
    }

    for (int i = 0; i < 8; ++i) {
        out_hash[i * 4]     = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        out_hash[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        out_hash[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        out_hash[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
}

bool RedjetVFSCrypto::verify_memory_seal(const void* data, size_t len, uint64_t expected_seal) noexcept {
    uint64_t actual_crc = compute_crc64(data, len);
    return (actual_crc == expected_seal);
}

bool RedjetVFSCrypto::validate_rawcode_cell(const void* addr, size_t len, uint64_t access_key) {
    (void)addr; (void)len;
    if (access_key == KEY_VAC_READ || access_key == KEY_JIT_TRANSITION) {
        _crypto_checks_passed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    _crypto_checks_failed.fetch_add(1, std::memory_order_relaxed);
    _tamper_events_detected.fetch_add(1, std::memory_order_relaxed);
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "vfs_crypto_fault",
                        "[VFS-CRYPTO] Unauthorized read attempt on rawcode_cell!");
    return false;
}

bool RedjetVFSCrypto::validate_vac_processed_chunk(const void* addr, size_t len, uint64_t access_key) {
    (void)addr; (void)len;
    if (access_key == KEY_MIRC_READ || access_key == KEY_JIT_TRANSITION) {
        _crypto_checks_passed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    _crypto_checks_failed.fetch_add(1, std::memory_order_relaxed);
    _tamper_events_detected.fetch_add(1, std::memory_order_relaxed);
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "vfs_crypto_fault",
                        "[VFS-CRYPTO] Unauthorized read attempt on vac_processed_code_chunk!");
    return false;
}

bool RedjetVFSCrypto::validate_mirc_ready_page(const void* addr, size_t len, uint64_t access_key, int mirc_level) {
    (void)addr; (void)len; (void)mirc_level;
    if (access_key == KEY_MIRC_READ || access_key == KEY_VAC_READ || access_key == KEY_JIT_TRANSITION) {
        _crypto_checks_passed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    _crypto_checks_failed.fetch_add(1, std::memory_order_relaxed);
    _tamper_events_detected.fetch_add(1, std::memory_order_relaxed);
    hs_unified_log_emit(HS_Ebin0_ERROR, HS_Ebin1_ERROR, 0, "vfs_crypto_fault",
                        "[VFS-CRYPTO] W^X violation on mirc_execution_ready_page!");
    return false;
}

bool RedjetVFSCrypto::check_vfs_path_permission(const char* path, uint64_t access_key) {
    if (!path) return false;
    if (std::strstr(path, "rawcode_cell") != nullptr) {
        return validate_rawcode_cell(nullptr, 0, access_key);
    }
    if (std::strstr(path, "vac_processed_code_chunk") != nullptr) {
        return validate_vac_processed_chunk(nullptr, 0, access_key);
    }
    if (std::strstr(path, "mirc_execution_ready_page") != nullptr) {
        return validate_mirc_ready_page(nullptr, 0, access_key);
    }
    return true;
}
