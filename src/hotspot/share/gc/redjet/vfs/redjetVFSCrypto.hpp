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

#ifndef SHARE_GC_REDJET_VFS_REDJETVFSCRYPTO_HPP
#define SHARE_GC_REDJET_VFS_REDJETVFSCRYPTO_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. REDJET VFS CRYPTO ENGINE (7-CIPHER DERIVATION, SHA/CRC, W^X VALIDATION)*/
/* ========================================================================= */

enum class RedjetMemoryProtectionType : uint8_t {
    PROT_RAWCODE_CELL           = 0,
    PROT_VAC_PROCESSED_CHUNK    = 1,
    PROT_MIRC_READY_PAGE_LVL1   = 2,
    PROT_MIRC_READY_PAGE_LVL2   = 3,
    PROT_UNRESTRICTED           = 4
};

class RedjetVFSCrypto {
public:
    static const uint64_t DEFAULT_VAC_SECRETS[7];
    static const uint64_t DEFAULT_MIRC_SECRETS[7];

    static const uint64_t KEY_VAC_READ;
    static const uint64_t KEY_MIRC_READ;
    static const uint64_t KEY_JIT_TRANSITION;

private:
    static std::atomic<uint64_t> _key_derivation_calls;
    static std::atomic<uint64_t> _crypto_checks_passed;
    static std::atomic<uint64_t> _crypto_checks_failed;
    static std::atomic<uint64_t> _tamper_events_detected;

public:
    // 7-Cipher Key Derivation Function:
    // Key = (((K0 ^ K1) >>> 13) ^ ((K2 & K3) <<< 17) ^ (K4 + K5)) ^ K6
    RJ_API static uint64_t derive_7cipher_key(const uint64_t K[7]) noexcept;

    // Multi-round Feistel cipher key derivation with session salt
    RJ_API static uint64_t derive_session_key(const uint64_t K[7], uint32_t session_salt, uint32_t rounds = 4) noexcept;

    // Integrity Checksums
    RJ_API static uint32_t compute_crc32(const void* data, size_t len) noexcept;
    RJ_API static uint64_t compute_crc64(const void* data, size_t len) noexcept;
    RJ_API static void     compute_sha256(const void* data, size_t len, uint8_t out_hash[32]) noexcept;

    // Memory Seal Verification
    RJ_API static bool verify_memory_seal(const void* data, size_t len, uint64_t expected_seal) noexcept;

    // Protection Level & Key Validation
    RJ_API static bool validate_rawcode_cell(const void* addr, size_t len, uint64_t access_key);
    RJ_API static bool validate_vac_processed_chunk(const void* addr, size_t len, uint64_t access_key);
    RJ_API static bool validate_mirc_ready_page(const void* addr, size_t len, uint64_t access_key, int mirc_level = 1);

    // Generic VFS path key check
    RJ_API static bool check_vfs_path_permission(const char* path, uint64_t access_key);

    // Statistics
    RJ_API static uint64_t total_derivations() noexcept { return _key_derivation_calls.load(std::memory_order_relaxed); }
    RJ_API static uint64_t checks_passed() noexcept { return _crypto_checks_passed.load(std::memory_order_relaxed); }
    RJ_API static uint64_t checks_failed() noexcept { return _crypto_checks_failed.load(std::memory_order_relaxed); }
    RJ_API static uint64_t tamper_events() noexcept { return _tamper_events_detected.load(std::memory_order_relaxed); }
};

#endif // SHARE_GC_REDJET_VFS_REDJETVFSCRYPTO_HPP
