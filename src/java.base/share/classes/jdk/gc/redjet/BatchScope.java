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
 * AutoCloseable RAII scope connecting Java applications directly to the Redjet
 * high-performance batch execution runtime.
 *
 * <pre>{@code
 * try (BatchScope scope = BatchScope.open()) {
 *     // High-throughput vectorized / batch processing block
 * }
 * }</pre>
 */
public final class BatchScope implements AutoCloseable {
    private final long handle;
    private boolean closed;

    /**
     * Constructs a batch scope with default priority (5) and warmup bypass disabled.
     */
    public BatchScope() {
        this(5, false);
    }

    /**
     * Constructs a batch scope with specified priority and warmup bypass flag.
     *
     * @param priority     batch execution priority
     * @param bypassWarmup whether to bypass compilation warmup for this batch
     */
    public BatchScope(int priority, boolean bypassWarmup) {
        long h = 0L;
        try {
            h = enterBatch0(priority, bypassWarmup);
        } catch (UnsatisfiedLinkError ignored) {
            // Interpreter or non-Redjet fallback
        }
        this.handle = h;
    }

    /**
     * Opens a new default batch scope.
     *
     * @return an open BatchScope instance
     */
    public static BatchScope open() {
        return new BatchScope();
    }

    /**
     * Opens a new batch scope with specified priority and warmup bypass option.
     *
     * @param priority     batch execution priority
     * @param bypassWarmup whether to bypass compilation warmup for this batch
     * @return an open BatchScope instance
     */
    public static BatchScope open(int priority, boolean bypassWarmup) {
        return new BatchScope(priority, bypassWarmup);
    }

    /**
     * Returns the underlying native batch handle.
     *
     * @return the 64-bit runtime handle
     */
    public long handle() {
        return handle;
    }

    /**
     * Closes the batch scope, exiting the batch execution runtime frame.
     */
    @Override
    public void close() {
        if (!closed) {
            closed = true;
            if (handle != 0L) {
                try {
                    exitBatch0(handle);
                } catch (UnsatisfiedLinkError ignored) {
                    // Fallback
                }
            }
        }
    }

    /**
     * Aborts the batch scope in the event of an exceptional termination.
     */
    public void abort() {
        if (!closed) {
            closed = true;
            if (handle != 0L) {
                try {
                    abortBatch0(handle);
                } catch (UnsatisfiedLinkError ignored) {
                    // Fallback
                }
            }
        }
    }

    private static native long enterBatch0(int priority, boolean bypassWarmup);
    private static native void exitBatch0(long handle);
    private static native void abortBatch0(long handle);
}
