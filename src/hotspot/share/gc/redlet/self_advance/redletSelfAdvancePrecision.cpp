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

#include "gc/redlet/self_advance/redletSelfAdvancePrecision.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>

#ifndef REDLET_STANDALONE_BUILD
#include "logging/log.hpp"
#else
#define log_info(...) if (false) printf
#define log_debug(...) if (false) printf
#define log_warning(...) if (false) printf
#define log_error(...) if (false) printf
#endif

RedletPrecisionGuardEngine g_redlet_precision_guard;

// Thread-local active precision scope variable
static thread_local float tl_active_precision_target = 0.0f;

/* ========================================================================= */
/* 1. RedletPrecisionScope                                                   */
/* ========================================================================= */

RedletPrecisionScope::RedletPrecisionScope(float target_epsilon) {
    _prev_epsilon = tl_active_precision_target;
    tl_active_precision_target = target_epsilon;
    _active = true;
}

RedletPrecisionScope::~RedletPrecisionScope() {
    tl_active_precision_target = _prev_epsilon;
    _active = false;
}

float RedletPrecisionScope::active_epsilon() const noexcept {
    return tl_active_precision_target;
}

/* ========================================================================= */
/* 2. RedletPrecisionGuardEngine                                             */
/* ========================================================================= */

RedletPrecisionGuardEngine::RedletPrecisionGuardEngine() {
    reset();
}

bool RedletPrecisionGuardEngine::is_floating_point_type(uint8_t dtype) noexcept {
    switch (dtype) {
        case SF_DTYPE_F32:
        case SF_DTYPE_F64:
        case SF_DTYPE_F16:
        case SF_DTYPE_F8:
        case SF_DTYPE_F128:
            return true;
        default:
            return false;
    }
}

RedletGuardStatus RedletPrecisionGuardEngine::validate_target(uint8_t dtype, float epsilon, const char* context_info) {
    _guards_enforced.fetch_add(1, std::memory_order_relaxed);

    // Rule 1: Non-FP structural or integer types MUST reject any non-zero epsilon
    if (!is_floating_point_type(dtype)) {
        if (epsilon != 0.0f) {
            _violations_caught.fetch_add(1, std::memory_order_relaxed);
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "[Redlet Precision Guard VIOLATION] Rejected target epsilon %e on non-FP dtype 0x%02X (%s)",
                     epsilon, dtype, context_info ? context_info : "unknown context");
            sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_ERROR, 0, "precision_guard_reject", msg);
            return RedletGuardStatus::ERR_TYPE_NOT_FP;
        }
        return RedletGuardStatus::GUARD_OK;
    }

    // Rule 2: Epsilon cannot be negative, NaN, or infinite
    if (std::isnan(epsilon) || std::isinf(epsilon) || epsilon < 0.0f) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
        return RedletGuardStatus::ERR_EPSILON_OUT_OF_BOUNDS;
    }

    // Rule 3: Upper bounds guard for FP32 and FP64
    if (dtype == SF_DTYPE_F32 && epsilon > 0.05f) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
        return RedletGuardStatus::ERR_EPSILON_OUT_OF_BOUNDS;
    }
    if (dtype == SF_DTYPE_F64 && epsilon > 0.005f) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
        return RedletGuardStatus::ERR_EPSILON_OUT_OF_BOUNDS;
    }

    return RedletGuardStatus::GUARD_OK;
}

bool RedletPrecisionGuardEngine::verify_numeric_f32(const float* cand, const float* ref, size_t count, float target_epsilon) {
    _numeric_verifications.fetch_add(1, std::memory_order_relaxed);
    if (!cand || !ref) return false;

    float max_err = 0.0f;
    float sum_sq = 0.0f;
    bool finite_ok = true;

    sf_accumulate_ieee754_error_f32(cand, ref, count, max_err, sum_sq, finite_ok);

    if (!finite_ok) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    float mse = (count > 0) ? (sum_sq / static_cast<float>(count)) : 0.0f;
    float max_allowable_mse = target_epsilon * target_epsilon;

    bool passed = (max_err <= target_epsilon) && (mse <= max_allowable_mse);
    if (!passed) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
    }
    return passed;
}

bool RedletPrecisionGuardEngine::verify_numeric_f64(const double* cand, const double* ref, size_t count, double target_epsilon) {
    _numeric_verifications.fetch_add(1, std::memory_order_relaxed);
    if (!cand || !ref) return false;

    double max_err = 0.0;
    double sum_sq = 0.0;

    for (size_t k = 0; k < count; ++k) {
        double c_v = cand[k];
        double r_v = ref[k];
        if (!std::isfinite(c_v) || !std::isfinite(r_v)) {
            _violations_caught.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        double diff = std::abs(c_v - r_v);
        if (diff > max_err) max_err = diff;
        sum_sq += diff * diff;
    }

    double mse = (count > 0) ? (sum_sq / static_cast<double>(count)) : 0.0;
    double max_allowable_mse = target_epsilon * target_epsilon;

    bool passed = (max_err <= target_epsilon) && (mse <= max_allowable_mse);
    if (!passed) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
    }
    return passed;
}

bool RedletPrecisionGuardEngine::verify_algebraic_exact(const void* cand, const void* ref, size_t bytes) {
    _algebraic_verifications.fetch_add(1, std::memory_order_relaxed);
    if (!cand || !ref) return false;
    if (bytes == 0) return true;

    bool match = (std::memcmp(cand, ref, bytes) == 0);
    if (!match) {
        _violations_caught.fetch_add(1, std::memory_order_relaxed);
    }
    return match;
}

void RedletPrecisionGuardEngine::reset() {
    _guards_enforced.store(0, std::memory_order_relaxed);
    _violations_caught.store(0, std::memory_order_relaxed);
    _algebraic_verifications.store(0, std::memory_order_relaxed);
    _numeric_verifications.store(0, std::memory_order_relaxed);
}

/* ========================================================================= */
/* 3. RedletPrecisionVerifier                                                */
/* ========================================================================= */

bool RedletPrecisionVerifier::is_fp_primitive(char c) {
    return (c == 'F' || c == 'D');
}

bool RedletPrecisionVerifier::is_fp_descriptor(const char* desc) {
    if (!desc || desc[0] == '\0') return false;
    const char* p = desc;
    if (*p == '(') {
        while (*p != '\0') {
            if (*p == '(' || *p == ')') {
                ++p;
                continue;
            }
            while (*p == '[') ++p;
            if (*p == 'F' || *p == 'D') return true;
            if (*p == 'L') {
                while (*p != ';' && *p != '\0') ++p;
                if (*p == ';') ++p;
            } else {
                if (*p != '\0') ++p;
            }
        }
        return false;
    }
    while (*p == '[') {
        ++p;
    }
    return (*p == 'F' || *p == 'D');
}

RedletPrecisionTier RedletPrecisionVerifier::classify_descriptor(const char* desc) {
    return is_fp_descriptor(desc) ? RedletPrecisionTier::NUMERIC_IEEE_FP
                                  : RedletPrecisionTier::EXACT_ALGEBRAIC;
}

RedletGuardStatus RedletPrecisionVerifier::verify_method_signature(const char* method_descriptor,
                                                                   float requested_epsilon,
                                                                   char* out_reason,
                                                                   size_t reason_sz) {
    if (std::isnan(requested_epsilon) || std::isinf(requested_epsilon) || requested_epsilon < 0.0f) {
        if (out_reason && reason_sz > 0) {
            snprintf(out_reason, reason_sz, "Invalid epsilon: must be non-negative finite float");
        }
        return RedletGuardStatus::ERR_EPSILON_OUT_OF_BOUNDS;
    }

    // Exact Algebraic (epsilon == 0.0f) is always valid across all JVM types
    if (requested_epsilon == 0.0f) {
        if (out_reason && reason_sz > 0) {
            snprintf(out_reason, reason_sz, "Valid: Exact Algebraic Mode (0.0 epsilon)");
        }
        return RedletGuardStatus::GUARD_OK;
    }

    if (!method_descriptor || method_descriptor[0] == '\0') {
        if (out_reason && reason_sz > 0) {
            snprintf(out_reason, reason_sz, "Method descriptor is null or empty");
        }
        return RedletGuardStatus::ERR_BYTECODE_VIOLATION;
    }

    if (method_descriptor[0] != '(') {
        if (out_reason && reason_sz > 0) {
            snprintf(out_reason, reason_sz, "Malformed descriptor: must start with '('");
        }
        return RedletGuardStatus::ERR_BYTECODE_VIOLATION;
    }
    const char* close_paren = strchr(method_descriptor, ')');
    if (!close_paren || *(close_paren + 1) == '\0') {
        if (out_reason && reason_sz > 0) {
            snprintf(out_reason, reason_sz, "Malformed descriptor: missing ')' or missing return type");
        }
        return RedletGuardStatus::ERR_BYTECODE_VIOLATION;
    }

    // Parse method descriptor: (Parameters)ReturnType
    // If epsilon > 0, signature MUST contain at least one floating-point component (F, D, [F, [D)
    bool has_any_fp = false;
    const char* p = method_descriptor;

    while (*p != '\0') {
        if (*p == '(' || *p == ')') {
            ++p;
            continue;
        }

        while (*p == '[') {
            ++p;
        }

        if (*p == 'F' || *p == 'D') {
            has_any_fp = true;
            ++p;
        } else if (*p == 'L') {
            while (*p != ';' && *p != '\0') {
                ++p;
            }
            if (*p == ';') ++p;
        } else {
            ++p;
        }
    }

    if (!has_any_fp) {
        if (out_reason && reason_sz > 0) {
            snprintf(out_reason, reason_sz,
                     "IncompatiblePrecisionTargetException: Descriptor '%s' contains no float/double types, "
                     "cannot specify loose precision epsilon=%.6f",
                     method_descriptor, requested_epsilon);
        }
        return RedletGuardStatus::ERR_TYPE_NOT_FP;
    }

    if (out_reason && reason_sz > 0) {
        snprintf(out_reason, reason_sz, "Valid: Floating-point precision target accepted (epsilon=%.6e)", requested_epsilon);
    }
    return RedletGuardStatus::GUARD_OK;
}

bool RedletPrecisionVerifier::validate_method(const char* class_name,
                                              const char* method_name,
                                              const char* descriptor,
                                              float requested_epsilon,
                                              char* out_error_buf,
                                              size_t error_buf_sz) {
    char reason[256]{ 0 };
    auto status = verify_method_signature(descriptor, requested_epsilon, reason, sizeof(reason));

    if (status != RedletGuardStatus::GUARD_OK) {
        if (out_error_buf && error_buf_sz > 0) {
            snprintf(out_error_buf, error_buf_sz,
                     "IncompatiblePrecisionTargetException at %s.%s%s: %s",
                     class_name ? class_name : "<unknown>",
                     method_name ? method_name : "<unknown>",
                     descriptor ? descriptor : "",
                     reason);
        }
        char log_msg[384];
        snprintf(log_msg, sizeof(log_msg), "[Precision Guard] Rejected %s.%s%s: %s",
                 class_name ? class_name : "", method_name ? method_name : "",
                 descriptor ? descriptor : "", reason);
        sf_unified_log_emit(SF_Ebin0_ERROR, SF_Ebin1_WARN, static_cast<uint32_t>(status),
                            "precision_guard_reject", log_msg);
        return false;
    }

    return true;
}

/* ========================================================================= */
/* 4. C EXPORT APIS                                                          */
/* ========================================================================= */

MSIMD_API int SF_precision_guard_validate(uint8_t dtype, float epsilon, const char* context) {
    return static_cast<int>(g_redlet_precision_guard.validate_target(dtype, epsilon, context));
}

MSIMD_API int SF_precision_guard_verify_f32(const float* cand, const float* ref, size_t count, float eps) {
    return g_redlet_precision_guard.verify_numeric_f32(cand, ref, count, eps) ? 1 : 0;
}

MSIMD_API int SF_precision_guard_verify_f64(const double* cand, const double* ref, size_t count, double eps) {
    return g_redlet_precision_guard.verify_numeric_f64(cand, ref, count, eps) ? 1 : 0;
}

MSIMD_API int SF_precision_guard_verify_exact(const void* cand, const void* ref, size_t bytes) {
    return g_redlet_precision_guard.verify_algebraic_exact(cand, ref, bytes) ? 1 : 0;
}

MSIMD_API uint64_t SF_precision_guard_get_violations(void) {
    return g_redlet_precision_guard.get_violations_caught();
}

MSIMD_API int SF_precision_verifier_verify_descriptor(const char* desc, float eps, char* reason, size_t sz) {
    return static_cast<int>(RedletPrecisionVerifier::verify_method_signature(desc, eps, reason, sz));
}

MSIMD_API int SF_precision_verifier_validate_method(const char* cls, const char* method, const char* desc,
                                                    float eps, char* err_buf, size_t sz) {
    return RedletPrecisionVerifier::validate_method(cls, method, desc, eps, err_buf, sz) ? 1 : 0;
}
