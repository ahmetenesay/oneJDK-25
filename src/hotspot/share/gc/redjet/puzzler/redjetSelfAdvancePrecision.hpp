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

#ifndef SHARE_GC_REDJET_PUZZLER_REDJETSELFADVANCEPRECISION_HPP
#define SHARE_GC_REDJET_PUZZLER_REDJETSELFADVANCEPRECISION_HPP

#include <cstdint>
#include <cstddef>
#include <cfloat>
#include <atomic>
#include "gc/redjet/runtime/redjetGearkit.hpp"
#include "gc/redjet/puzzler/redjetPuzzleArena.hpp"

#ifndef RJ_API
#if defined(_WIN32)
#define RJ_API __declspec(dllexport)
#else
#define RJ_API __attribute__((visibility("default")))
#endif
#endif

/* ========================================================================= */
/* 1. PRECISION TARGET DEFINITIONS & TWO-TIER ENUMS                          */
/* ========================================================================= */

enum class RedjetPrecisionTier : uint8_t {
    NUMERIC_IEEE_FP = 0, // Float, Double, FP Tensors: Strict IEEE 754 default, epsilon allowed
    EXACT_ALGEBRAIC = 1  // Objects (oop), Strings, Bitmaps, Card Table: Strictly 0-bit deviation
};

enum class RedjetGuardStatus : uint8_t {
    GUARD_OK                  = 0,
    ERR_TYPE_NOT_FP           = 1, // Non-FP tried to set epsilon > 0 (IncompatiblePrecisionTargetException)
    ERR_EPSILON_OUT_OF_BOUNDS = 2, // Epsilon negative or greater than max allowable loose bound
    ERR_NAN_INF_PRODUCED      = 3, // NaN or Infinity produced in candidate execution
    ERR_BITWISE_MISMATCH      = 4, // EXACT_ALGEBRAIC memcmp != 0
    ERR_BYTECODE_VIOLATION    = 5  // Null or corrupt descriptor
};

struct RedjetPrecisionDescriptor {
    uint8_t dtype{ SACFOR_F32 };
    float   epsilon{ FLT_EPSILON };
    bool    is_fp{ true };
    char    type_name[32]{ "float" };
};

/* ========================================================================= */
/* 2. THREAD-LOCAL PRECISION SCOPE (RAII Guard for PrecisionScope API)       */
/* ========================================================================= */

class RedjetPrecisionScope {
private:
    float _prev_epsilon{ 0.0f };
    bool  _active{ false };

public:
    explicit RedjetPrecisionScope(float target_epsilon);
    ~RedjetPrecisionScope();

    // Non-copyable
    RedjetPrecisionScope(const RedjetPrecisionScope&) = delete;
    RedjetPrecisionScope& operator=(const RedjetPrecisionScope&) = delete;

    bool is_active() const noexcept { return _active; }
    float active_epsilon() const noexcept;
};

/* ========================================================================= */
/* 3. REDJET PRECISION GUARD ENGINE                                          */
/* ========================================================================= */

class RedjetPrecisionGuardEngine {
private:
    std::atomic<uint64_t> _guards_enforced{ 0 };
    std::atomic<uint64_t> _violations_caught{ 0 };
    std::atomic<uint64_t> _algebraic_verifications{ 0 };
    std::atomic<uint64_t> _numeric_verifications{ 0 };

public:
    RedjetPrecisionGuardEngine();

    // Check whether a data type qualifies for NUMERIC_IEEE_FP relaxation
    static bool is_floating_point_type(uint8_t dtype) noexcept;

    // Validate a @PrecisionTarget or PrecisionScope request against the type
    RedjetGuardStatus validate_target(uint8_t dtype, float epsilon, const char* context_info);

    // Verify numerical outputs within epsilon (checks finiteness, max abs err, and MSE)
    bool verify_numeric_f32(const float* cand, const float* ref, size_t count, float target_epsilon);
    bool verify_numeric_f64(const double* cand, const double* ref, size_t count, double target_epsilon);

    // Verify algebraic exact invariance (strictly zero bit variance, memcmp == 0)
    bool verify_algebraic_exact(const void* cand, const void* ref, size_t bytes);

    uint64_t get_violations_caught() const noexcept {
        return _violations_caught.load(std::memory_order_relaxed);
    }

    uint64_t get_guards_enforced() const noexcept {
        return _guards_enforced.load(std::memory_order_relaxed);
    }

    void reset();
};

/* ========================================================================= */
/* 4. REDJET PRECISION VERIFIER (Java Bytecode Method / Descriptor Guard)    */
/* ========================================================================= */

class RedjetPrecisionVerifier {
public:
    static bool is_fp_primitive(char c);
    static bool is_fp_descriptor(const char* desc);
    static RedjetPrecisionTier classify_descriptor(const char* desc);

    // Verifies whether method descriptor allows specified epsilon.
    // Throws IncompatiblePrecisionTargetException semantics if non-FP requests epsilon > 0.
    static RedjetGuardStatus verify_method_signature(const char* method_descriptor,
                                                     float requested_epsilon,
                                                     char* out_reason = nullptr,
                                                     size_t reason_sz = 0);

    static bool validate_method(const char* class_name,
                                const char* method_name,
                                const char* descriptor,
                                float requested_epsilon,
                                char* out_error_buf = nullptr,
                                size_t error_buf_sz = 0);
};

/* ========================================================================= */
/* 5. GLOBAL INSTANCE & C EXPORT APIS                                        */
/* ========================================================================= */

extern RedjetPrecisionGuardEngine g_redjet_precision_guard;

#ifdef __cplusplus
extern "C" {
#endif

RJ_API int      SF_precision_guard_validate(uint8_t dtype, float epsilon, const char* context);
RJ_API int      SF_precision_guard_verify_f32(const float* cand, const float* ref, size_t count, float eps);
RJ_API int      SF_precision_guard_verify_f64(const double* cand, const double* ref, size_t count, double eps);
RJ_API int      SF_precision_guard_verify_exact(const void* cand, const void* ref, size_t bytes);
RJ_API uint64_t SF_precision_guard_get_violations(void);

RJ_API int      SF_precision_verifier_verify_descriptor(const char* desc, float eps, char* reason, size_t sz);
RJ_API int      SF_precision_verifier_validate_method(const char* cls, const char* method, const char* desc,
                                                         float eps, char* err_buf, size_t sz);

#ifdef __cplusplus
}
#endif

#endif // SHARE_GC_REDJET_PUZZLER_REDJETSELFADVANCEPRECISION_HPP