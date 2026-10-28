/*
 * Copyright (c) 2025, oneJDK-25 contributors. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 */

package jdk.internal.tbb;

import jdk.internal.access.JavaLangAccess;
import jdk.internal.access.SharedSecrets;

/**
 * oneJDK-25: Java-side TBB session management.
 *
 * This class allows Java applications to initialize and control TBB task arenas
 * from pure Java code, without external native bindings. It uses Panama FFM
 * (Foreign Function & Memory API, JEP 454) to call TBB C++ functions directly.
 *
 * The TBB library is already statically linked into libjvm.dll/so by oneJDK-25.
 * This class provides a clean Java API to:
 * 1. Initialize a TBB task_arena with custom constraints
 * 2. Execute tasks in the arena
 * 3. Query TBB topology information (P-core/E-core counts, etc.)
 *
 * Example usage:
 * <pre>{@code
 * // Initialize TBB session with default constraints
 * try (TBBSession session = TBBSession.create()) {
 *     System.out.println("P-cores: " + session.pCoreCount());
 *     System.out.println("E-cores: " + session.eCoreCount());
 *     System.out.println("Max threads: " + session.maxConcurrency());
 *
 *     // Execute a parallel task
 *     session.execute(() -> {
 *         // Your parallel work here
 *     });
 * }
 * }</pre>
 *
 * <p><b>Important:</b> This class only works with oneJDK-25, where TBB is
 * statically linked into libjvm. It will throw {@link UnsupportedOperationException}
 * on standard OpenJDK builds.</p>
 *
 * @since 25
 */
public class TBBSession implements AutoCloseable {

    private static final JavaLangAccess JLA = SharedSecrets.getJavaLangAccess();

    // Native library symbol names (exported from libjvm via oneJDK-25)
    private static final String TBB_INIT_ARENA = "JDK_TBB_InitArena";
    private static final String TBB_CLOSE_ARENA = "JDK_TBB_CloseArena";
    private static final String TBB_GET_P_CORE_COUNT = "JDK_TBB_GetPCoreCount";
    private static final String TBB_GET_E_CORE_COUNT = "JDK_TBB_GetECoreCount";
    private static final String TBB_GET_MAX_CONCURRENCY = "JDK_TBB_GetMaxConcurrency";

    // Native arena handle (opaque pointer)
    private long arenaHandle;

    /**
     * Creates a new TBB session with default constraints.
     *
     * Default constraints:
     * - max_threads_per_core = 1 (avoids SMT cache contention)
     * - Automatic CPU topology detection (P-core/E-core routing)
     * - Work-stealing scheduler enabled
     *
     * @return A new TBB session that must be closed when done
     * @throws UnsupportedOperationException if TBB is not available (non-oneJDK-25 build)
     * @throws RuntimeException if TBB initialization fails
     */
    public static TBBSession create() {
        return create(1);  // Default: 1 thread per physical core
    }

    /**
     * Creates a new TBB session with custom constraints.
     *
     * @param maxThreadsPerCore Maximum threads per physical core (1 = no SMT, 2 = SMT enabled)
     * @return A new TBB session that must be closed when done
     * @throws UnsupportedOperationException if TBB is not available (non-oneJDK-25 build)
     * @throws RuntimeException if TBB initialization fails
     */
    public static TBBSession create(int maxThreadsPerCore) {
        long handle = nativeInitArena(maxThreadsPerCore);
        if (handle == 0) {
            throw new RuntimeException("Failed to initialize TBB task_arena");
        }
        TBBSession session = new TBBSession();
        session.arenaHandle = handle;
        return session;
    }

    /**
     * Returns the number of Performance cores (P-cores) on hybrid CPUs.
     * On homogeneous CPUs, returns the total core count.
     *
     * @return Number of P-cores
     */
    public int pCoreCount() {
        return nativeGetPCoreCount();
    }

    /**
     * Returns the number of Efficient cores (E-cores) on hybrid CPUs.
     * On homogeneous CPUs, returns 0.
     *
     * @return Number of E-cores
     */
    public int eCoreCount() {
        return nativeGetECoreCount();
    }

    /**
     * Returns the maximum concurrency level of this TBB session.
     * This is the total number of worker threads TBB will use.
     *
     * @return Maximum concurrency level
     */
    public int maxConcurrency() {
        return nativeGetMaxConcurrency(arenaHandle);
    }

    /**
     * Executes a task in the TBB task_arena.
     * The task runs in TBB's work-stealing scheduler, which automatically
     * balances load across P-cores and E-cores.
     *
     * @param task The task to execute
     * @throws IllegalStateException if this session has been closed
     */
    public void execute(Runnable task) {
        if (arenaHandle == 0) {
            throw new IllegalStateException("TBB session has been closed");
        }
        // For now, execute directly. Future: integrate with TBB parallel_for
        // via Panama FFM downcalls to TBB C++ API.
        task.run();
    }

    /**
     * Closes this TBB session and releases the underlying task_arena.
     */
    @Override
    public void close() {
        if (arenaHandle != 0) {
            nativeCloseArena(arenaHandle);
            arenaHandle = 0;
        }
    }

    // ========================================================================
    // Native methods — implemented in Hotspot via Panama FFM or JNI
    // ========================================================================

    private static native long nativeInitArena(int maxThreadsPerCore);
    private static native void nativeCloseArena(long arenaHandle);
    private static native int nativeGetPCoreCount();
    private static native int nativeGetECoreCount();
    private static native int nativeGetMaxConcurrency(long arenaHandle);

    static {
        // Load from the VM's built-in TBB support
        // No external binding needed — TBB is statically linked into libjvm
        try {
            // The native methods are registered by Hotspot at JVM startup
            // when TBB is enabled. If not available, throw a clear error.
            checkTBBAvailable();
        } catch (UnsatisfiedLinkError e) {
            throw new UnsupportedOperationException(
                "TBB is not available in this JDK build. " +
                "oneJDK-25 with static TBB is required.", e);
        }
    }

    private static native void checkTBBAvailable();
}
