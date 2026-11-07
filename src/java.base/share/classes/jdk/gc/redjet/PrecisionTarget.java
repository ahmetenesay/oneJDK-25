/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation. Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
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

package jdk.gc.redjet;

import java.lang.annotation.Documented;
import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * RedjetGC Precision Target Annotation.
 *
 * Annotates methods, types, or constructors to specify numerical precision relaxation (e.g. IEEE-754 epsilon tolerances),
 * allowing SIMD vectorization and fast-math optimizations under bounded error thresholds.
 */
@Documented
@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.METHOD, ElementType.TYPE, ElementType.CONSTRUCTOR})
public @interface PrecisionTarget {
    /**
     * Target error tolerance (epsilon) allowed for numerical transformations.
     * Default is 0.0f (strict IEEE-754 exactness).
     *
     * @return allowed numerical error tolerance
     */
    float epsilon() default 0.0f;

    /**
     * Precision tier mode name or specification (e.g. "FP32", "FP16", "RELAXED", "FAST_MATH").
     *
     * @return precision tier name
     */
    String tier() default "RELAXED";
}
