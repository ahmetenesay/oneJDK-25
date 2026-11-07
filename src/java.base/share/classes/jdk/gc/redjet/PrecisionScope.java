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

/**
 * AutoCloseable RAII thread-local scope for relaxing numerical precision in a scoped block.
 *
 * <pre>{@code
 * try (PrecisionScope scope = PrecisionScope.of(1e-4f)) {
 *     // Vectorized computations within epsilon error threshold
 * }
 * }</pre>
 */
public final class PrecisionScope implements AutoCloseable {
    private final float previousEpsilon;
    private final float activeEpsilon;
    private boolean closed;

    /**
     * Constructs a precision scope with target epsilon tolerance.
     *
     * @param targetEpsilon numerical tolerance allowed within this scope
     */
    public PrecisionScope(float targetEpsilon) {
        this.activeEpsilon = targetEpsilon;
        float prev = 0.0f;
        try {
            prev = enterPrecision0(targetEpsilon);
        } catch (UnsatisfiedLinkError ignored) {
            // Interpreter fallback
        }
        this.previousEpsilon = prev;
    }

    /**
     * Opens a precision scope with specified target epsilon.
     *
     * @param targetEpsilon numerical tolerance allowed within this scope
     * @return an open PrecisionScope instance
     */
    public static PrecisionScope of(float targetEpsilon) {
        return new PrecisionScope(targetEpsilon);
    }

    /**
     * Returns the target epsilon of this scope.
     *
     * @return the active target epsilon
     */
    public float targetEpsilon() {
        return activeEpsilon;
    }

    /**
     * Returns the previous epsilon restored upon closing this scope.
     *
     * @return the previous epsilon
     */
    public float previousEpsilon() {
        return previousEpsilon;
    }

    @Override
    public void close() {
        if (!closed) {
            closed = true;
            try {
                exitPrecision0(previousEpsilon);
            } catch (UnsatisfiedLinkError ignored) {
                // Fallback
            }
        }
    }

    private static native float enterPrecision0(float epsilon);
    private static native void exitPrecision0(float prevEpsilon);
}
