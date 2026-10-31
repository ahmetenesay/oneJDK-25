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

package jdk.internal.tbb;

import jdk.internal.access.JavaLangAccess;
import jdk.internal.access.SharedSecrets;

/**
 * oneJDK-25: Complete Java-side TBB and tbbmalloc API.
 *
 * This class exposes the full oneTBB C API to Java applications without external
 * native bindings. TBB is statically linked into libjvm.dll/so by oneJDK-25,
 * so all methods call directly into the JVM's built-in TBB support.
 *
 * The API is organized into these categories:
 *
 * <h3>1. Version Information</h3>
 * <ul>
 *   <li>{@link #versionMajor()} — TBB major version (e.g., 2022)</li>
 *   <li>{@link #versionMinor()} — TBB minor version (e.g., 3)</li>
 *   <li>{@link #interfaceVersion()} — TBB interface version (e.g., 12170)</li>
 *   <li>{@link #runtimeVersionString()} — Runtime version string from the linked library</li>
 * </ul>
 *
 * <h3>2. CPU Topology (info.h)</h3>
 * <ul>
 *   <li>{@link #numaNodeCount()} — Number of NUMA nodes</li>
 *   <li>{@link #numaDefaultConcurrency(int)} — Default concurrency for a NUMA node</li>
 *   <li>{@link #coreTypeCount()} — Number of core types (1 = homogeneous, 2 = hybrid P/E)</li>
 *   <li>{@link #defaultConcurrency()} — Default concurrency for the whole system</li>
 * </ul>
 *
 * <h3>3. Global Control (global_control.h)</h3>
 * <ul>
 *   <li>{@link #setMaxAllowedParallelism(int)} — Limit max TBB worker threads</li>
 *   <li>{@link #setThreadStackSize(long)} — Set TBB worker thread stack size</li>
 *   <li>{@link #setTerminateOnException(boolean)} — Terminate on uncaught TBB exception</li>
 *   <li>{@link #activeValue(GlobalControlParameter)} — Query active global control value</li>
 * </ul>
 *
 * <h3>4. Task Arena (task_arena.h)</h3>
 * <ul>
 *   <li>{@link #createArena(int, int, int)} — Create task_arena with constraints</li>
 *   <li>{@link #closeArena(long)} — Close and destroy task_arena</li>
 *   <li>{@link #arenaMaxConcurrency(long)} — Query arena's max concurrency</li>
 *   <li>{@link #arenaIsActive(long)} — Check if arena is initialized</li>
 *   <li>{@link #currentThreadIndex()} — Current thread's slot index in its arena</li>
 *   <li>{@link #maxConcurrency()} — Global max concurrency</li>
 * </ul>
 *
 * <h3>5. tbbmalloc — Scalable Memory Allocation (scalable_allocator.h)</h3>
 * <ul>
 *   <li>{@link #scalableMalloc(long)} — Allocate memory via tbbmalloc</li>
 *   <li>{@link #scalableFree(long)} — Free tbbmalloc-allocated memory</li>
 *   <li>{@link #scalableRealloc(long, long)} — Reallocate tbbmalloc memory</li>
 *   <li>{@link #scalableCalloc(long, long)} — Zero-initialized tbbmalloc allocation</li>
 *   <li>{@link #scalableAlignedMalloc(long, long)} — Aligned tbbmalloc allocation</li>
 *   <li>{@link #scalableAlignedRealloc(long, long, long)} — Aligned tbbmalloc reallocation</li>
 *   <li>{@link #scalableAlignedFree(long)} — Free aligned tbbmalloc memory</li>
 *   <li>{@link #scalableMsize(long)} — Get usable size of tbbmalloc block</li>
 *   <li>{@link #scalablePosixMemAlign(long, long)} — POSIX-aligned tbbmalloc allocation</li>
 * </ul>
 *
 * <h3>6. tbbmalloc — Allocation Modes (scalable_allocator.h)</h3>
 * <ul>
 *   <li>{@link #setUseHugePages(boolean)} — Enable/disable huge pages</li>
 *   <li>{@link #setSoftHeapLimit(long)} — Set soft heap memory limit</li>
 *   <li>{@link #setHugeSizeThreshold(long)} — Set huge allocation size threshold</li>
 *   <li>{@link #cleanAllBuffers()} — Clean all thread allocator buffers</li>
 *   <li>{@link #cleanThreadBuffers()} — Clean current thread's allocator buffers</li>
 * </ul>
 *
 * <h3>7. tbbmalloc — Memory Pools (rml::MemoryPool)</h3>
 * <ul>
 *   <li>{@link #poolCreate(long, long, boolean, boolean)} — Create a memory pool</li>
 *   <li>{@link #poolDestroy(long)} — Destroy a memory pool</li>
 *   <li>{@link #poolMalloc(long, long)} — Allocate from a memory pool</li>
 *   <li>{@link #poolRealloc(long, long, long)} — Reallocate in a memory pool</li>
 *   <li>{@link #poolAlignedMalloc(long, long, long)} — Allocated from a memory pool</li>
 *   <li>{@link #poolAlignedRealloc(long, long, long, long)} — Aligned reallocate in a pool</li>
 *   <li>{@link #poolFree(long, long)} — Free memory back to a pool</li>
 *   <li>{@link #poolReset(long)} — Reset a pool (free all allocations)</li>
 *   <li>{@link #poolMsize(long, long)} — Get usable size in a pool</li>
 *   <li>{@link #poolIdentify(long)} — Identify which pool owns a pointer</li>
 * </ul>
 *
 * <h3>8. Timing (tick_count.h)</h3>
 * <ul>
 *   <li>{@link #tickCount()} — High-resolution tick count</li>
 *   <li>{@link #ticksPerSecond()} — Ticks per second for the current platform</li>
 * </ul>
 *
 * <h3>9. Task Scheduler Handle (global_control.h)</h3>
 * <ul>
 *   <li>{@link #getTaskSchedulerHandle()} — Get scheduler handle for lifecycle control</li>
 *   <li>{@link #releaseTaskSchedulerHandle(long)} — Release scheduler handle</li>
 * </ul>
 *
 * Example usage:
 * <pre>{@code
 * // Query TBB version and topology
 * System.out.println("TBB version: " + TBBSession.versionMajor() + "." + TBBSession.versionMinor());
 * System.out.println("NUMA nodes: " + TBBSession.numaNodeCount());
 * System.out.println("Core types: " + TBBSession.coreTypeCount());
 * System.out.println("Default concurrency: " + TBBSession.defaultConcurrency());
 *
 * // Allocate memory with tbbmalloc
 * long ptr = TBBSession.scalableMalloc(1024);
 * try {
 *     // Use the memory via Unsafe or Panama FFM
 *     System.out.println("Allocated size: " + TBBSession.scalableMsize(ptr));
 * } finally {
 *     TBBSession.scalableFree(ptr);
 * }
 *
 * // Create a task arena
 * long arena = TBBSession.createArena(TBBSession.AUTOMATIC, 1, TBBSession.PRIORITY_NORMAL);
 * try {
 *     System.out.println("Arena concurrency: " + TBBSession.arenaMaxConcurrency(arena));
 * } finally {
 *     TBBSession.closeArena(arena);
 * }
 *
 * // Control global parallelism
 * TBBSession.setMaxAllowedParallelism(8);
 * System.out.println("Active parallelism: " +
 *     TBBSession.activeValue(TBBSession.MAX_ALLOWED_PARALLELISM));
 * }</pre>
 *
 * <p><b>Important:</b> This class only works with oneJDK-25, where TBB is
 * statically linked into libjvm. It will throw {@link UnsupportedOperationException}
 * on standard OpenJDK builds.</p>
 *
 * @since 25
 */
public class TBBSession {

    private static final JavaLangAccess JLA = SharedSecrets.getJavaLangAccess();

    // ========================================================================
    // Constants
    // ========================================================================

    /** Automatic concurrency (TBB decides based on CPU topology). */
    public static final int AUTOMATIC = -1;

    /** Arena not initialized. */
    public static final int NOT_INITIALIZED = -2;

    // Task arena priority levels
    /** Low priority arena. */
    public static final int PRIORITY_LOW = 1;
    /** Normal priority arena (default). */
    public static final int PRIORITY_NORMAL = 2;
    /** High priority arena. */
    public static final int PRIORITY_HIGH = 3;

    // Global control parameters
    /** Maximum allowed parallelism parameter. */
    public static final int MAX_ALLOWED_PARALLELISM = 0;
    /** Thread stack size parameter. */
    public static final int THREAD_STACK_SIZE = 1;
    /** Terminate on exception parameter. */
    public static final int TERMINATE_ON_EXCEPTION = 2;

    // tbbmalloc allocation mode parameters
    /** Enable/disable huge pages. */
    public static final int TBBMALLOC_USE_HUGE_PAGES = 0;
    /** Set soft heap limit (bytes). */
    public static final int TBBMALLOC_SET_SOFT_HEAP_LIMIT = 1;
    /** Set huge size threshold (bytes). */
    public static final int TBBMALLOC_SET_HUGE_SIZE_THRESHOLD = 2;

    // tbbmalloc allocation commands
    /** Clean all thread buffers. */
    public static final int TBBMALLOC_CLEAN_ALL_BUFFERS = 0;
    /** Clean current thread buffers only. */
    public static final int TBBMALLOC_CLEAN_THREAD_BUFFERS = 1;

    // tbbmalloc result codes
    /** Operation succeeded. */
    public static final int TBBMALLOC_OK = 0;
    /** Invalid parameter. */
    public static final int TBBMALLOC_INVALID_PARAM = 1;
    /** Unsupported operation. */
    public static final int TBBMALLOC_UNSUPPORTED = 2;
    /** Out of memory. */
    public static final int TBBMALLOC_NO_MEMORY = 3;
    /** No effect. */
    public static final int TBBMALLOC_NO_EFFECT = 4;

    // ========================================================================
    // 1. Version Information (version.h)
    // ========================================================================

    /**
     * Returns the TBB major version number (e.g., 2022).
     * This is the compile-time version from the statically linked library.
     * @return TBB major version
     */
    public static int versionMajor() {
        return nativeVersionMajor();
    }

    /**
     * Returns the TBB minor version number (e.g., 3).
     * @return TBB minor version
     */
    public static int versionMinor() {
        return nativeVersionMinor();
    }

    /**
     * Returns the TBB interface version (e.g., 12170).
     * @return TBB interface version
     */
    public static int interfaceVersion() {
        return nativeInterfaceVersion();
    }

    /**
     * Returns the runtime version string from the linked TBB library.
     * This may differ from the compile-time version if a different
     * library version is loaded at runtime.
     * @return Runtime version string
     */
    public static String runtimeVersionString() {
        return nativeRuntimeVersionString();
    }

    /**
     * Returns the runtime interface version from the linked TBB library.
     * @return Runtime interface version
     */
    public static int runtimeInterfaceVersion() {
        return nativeRuntimeInterfaceVersion();
    }

    // ========================================================================
    // 2. CPU Topology (info.h)
    // ========================================================================

    /**
     * Returns the number of NUMA nodes in the system.
     * @return Number of NUMA nodes
     */
    public static int numaNodeCount() {
        return nativeNumaNodeCount();
    }

    /**
     * Returns the default concurrency for a specific NUMA node.
     * @param numaId NUMA node index, or -1 for the whole system
     * @return Default concurrency for the NUMA node
     */
    public static int numaDefaultConcurrency(int numaId) {
        return nativeNumaDefaultConcurrency(numaId);
    }

    /**
     * Returns the number of core types in the system.
     * On homogeneous CPUs, returns 1.
     * On hybrid CPUs (Intel P-core/E-core), returns 2.
     * @return Number of core types
     */
    public static int coreTypeCount() {
        return nativeCoreTypeCount();
    }

    /**
     * Returns the default concurrency for the whole system.
     * This is the number of threads TBB would use by default.
     * @return Default system concurrency
     */
    public static int defaultConcurrency() {
        return nativeDefaultConcurrency();
    }

    // ========================================================================
    // 3. Global Control (global_control.h)
    // ========================================================================

    /**
     * Sets the maximum allowed parallelism for TBB worker threads.
     * This limits the total number of worker threads TBB can create.
     * @param maxParallelism Maximum number of worker threads (must be > 0)
     */
    public static void setMaxAllowedParallelism(int maxParallelism) {
        nativeSetGlobalControl(MAX_ALLOWED_PARALLELISM, maxParallelism);
    }

    /**
     * Sets the stack size for TBB worker threads.
     * @param stackSize Stack size in bytes
     */
    public static void setThreadStackSize(long stackSize) {
        nativeSetGlobalControl(THREAD_STACK_SIZE, stackSize);
    }

    /**
     * Controls whether TBB terminates the JVM on uncaught exceptions.
     * @param terminate true to terminate on uncaught exception, false to propagate
     */
    public static void setTerminateOnException(boolean terminate) {
        nativeSetGlobalControl(TERMINATE_ON_EXCEPTION, terminate ? 1 : 0);
    }

    /**
     * Queries the active value of a global control parameter.
     * @param parameter One of MAX_ALLOWED_PARALLELISM, THREAD_STACK_SIZE, TERMINATE_ON_EXCEPTION
     * @return Active value of the parameter
     */
    public static long activeValue(int parameter) {
        return nativeActiveValue(parameter);
    }

    // ========================================================================
    // 4. Task Arena (task_arena.h)
    // ========================================================================

    /**
     * Creates a new TBB task_arena with the specified constraints.
     *
     * @param maxConcurrency Maximum concurrency (use AUTOMATIC for TBB default)
     * @param reservedForMasters Reserved slots for external threads (default 1)
     * @param priority Arena priority: PRIORITY_LOW, PRIORITY_NORMAL, or PRIORITY_HIGH
     * @return Opaque arena handle (must be closed with closeArena)
     * @throws RuntimeException if arena creation fails
     */
    public static long createArena(int maxConcurrency, int reservedForMasters, int priority) {
        long handle = nativeCreateArena(maxConcurrency, reservedForMasters, priority);
        if (handle == 0) {
            throw new RuntimeException("Failed to create TBB task_arena");
        }
        return handle;
    }

    /**
     * Creates a new TBB task_arena with default constraints.
     * @return Opaque arena handle (must be closed with closeArena)
     */
    public static long createArena() {
        return createArena(AUTOMATIC, 1, PRIORITY_NORMAL);
    }

    /**
     * Closes and destroys a TBB task_arena.
     * @param arenaHandle Handle returned by createArena
     */
    public static void closeArena(long arenaHandle) {
        if (arenaHandle != 0) {
            nativeCloseArena(arenaHandle);
        }
    }

    /**
     * Returns the maximum concurrency of a task_arena.
     * @param arenaHandle Handle returned by createArena
     * @return Maximum concurrency level
     */
    public static int arenaMaxConcurrency(long arenaHandle) {
        return nativeArenaMaxConcurrency(arenaHandle);
    }

    /**
     * Checks if a task_arena is active (initialized).
     * @param arenaHandle Handle returned by createArena
     * @return true if the arena is initialized and active
     */
    public static boolean arenaIsActive(long arenaHandle) {
        return nativeArenaIsActive(arenaHandle);
    }

    /**
     * Returns the slot index of the current thread in its task_arena.
     * @return Thread index, or NOT_INITIALIZED if not in an arena
     */
    public static int currentThreadIndex() {
        return nativeCurrentThreadIndex();
    }

    /**
     * Returns the global maximum concurrency.
     * This is the total number of threads TBB can use across all arenas.
     * @return Global max concurrency
     */
    public static int maxConcurrency() {
        return nativeMaxConcurrency();
    }

    // ========================================================================
    // 5. tbbmalloc — Scalable Memory Allocation (scalable_allocator.h)
    // ========================================================================

    /**
     * Allocates memory using tbbmalloc (scalable_malloc).
     * tbbmalloc is a thread-local allocator that eliminates lock contention.
     * @param size Number of bytes to allocate
     * @return Pointer to allocated memory (0 on failure)
     */
    public static long scalableMalloc(long size) {
        return nativeScalableMalloc(size);
    }

    /**
     * Frees memory allocated by scalableMalloc, scalableRealloc, or scalableCalloc.
     * @param ptr Pointer returned by scalableMalloc or related functions
     */
    public static void scalableFree(long ptr) {
        if (ptr != 0) {
            nativeScalableFree(ptr);
        }
    }

    /**
     * Reallocates memory using tbbmalloc (scalable_realloc).
     * @param ptr Previous pointer (0 for new allocation)
     * @param size New size in bytes
     * @return Pointer to reallocated memory (0 on failure)
     */
    public static long scalableRealloc(long ptr, long size) {
        return nativeScalableRealloc(ptr, size);
    }

    /**
     * Allocates zero-initialized memory using tbbmalloc (scalable_calloc).
     * @param nobj Number of objects
     * @param size Size of each object in bytes
     * @return Pointer to zero-initialized memory (0 on failure)
     */
    public static long scalableCalloc(long nobj, long size) {
        return nativeScalableCalloc(nobj, size);
    }

    /**
     * Allocates aligned memory using tbbmalloc (scalable_aligned_malloc).
     * @param size Number of bytes to allocate
     * @param alignment Alignment in bytes (must be power of 2)
     * @return Pointer to aligned memory (0 on failure)
     */
    public static long scalableAlignedMalloc(long size, long alignment) {
        return nativeScalableAlignedMalloc(size, alignment);
    }

    /**
     * Reallocates aligned memory using tbbmalloc (scalable_aligned_realloc).
     * @param ptr Previous pointer (0 for new allocation)
     * @param size New size in bytes
     * @param alignment Alignment in bytes
     * @return Pointer to reallocated aligned memory (0 on failure)
     */
    public static long scalableAlignedRealloc(long ptr, long size, long alignment) {
        return nativeScalableAlignedRealloc(ptr, size, alignment);
    }

    /**
     * Frees aligned memory allocated by scalableAlignedMalloc.
     * @param ptr Pointer returned by scalableAlignedMalloc
     */
    public static void scalableAlignedFree(long ptr) {
        if (ptr != 0) {
            nativeScalableAlignedFree(ptr);
        }
    }

    /**
     * Returns the usable size of a tbbmalloc-allocated block.
     * @param ptr Pointer returned by scalableMalloc or related functions
     * @return Usable size in bytes, or 0 if ptr is not a tbbmalloc block
     */
    public static long scalableMsize(long ptr) {
        return nativeScalableMsize(ptr);
    }

    /**
     * Allocates POSIX-aligned memory (scalable_posix_memalign).
     * @param alignment Alignment in bytes (must be power of 2 and multiple of sizeof(void*))
     * @param size Number of bytes to allocate
     * @return Pointer to aligned memory (0 on failure)
     */
    public static long scalablePosixMemAlign(long alignment, long size) {
        return nativeScalablePosixMemAlign(alignment, size);
    }

    // ========================================================================
    // 6. tbbmalloc — Allocation Modes (scalable_allocator.h)
    // ========================================================================

    /**
     * Enables or disables huge pages for tbbmalloc.
     * @param enable true to enable huge pages, false to disable
     * @return TBBMALLOC_OK on success, error code on failure
     */
    public static int setUseHugePages(boolean enable) {
        return nativeScalableAllocationMode(TBBMALLOC_USE_HUGE_PAGES, enable ? 1 : 0);
    }

    /**
     * Sets a soft heap memory limit for tbbmalloc.
     * When the limit is exceeded, tbbmalloc cleans internal buffers
     * but does not prevent requesting more memory from the OS.
     * @param limitBytes Soft heap limit in bytes
     * @return TBBMALLOC_OK on success, error code on failure
     */
    public static int setSoftHeapLimit(long limitBytes) {
        return nativeScalableAllocationMode(TBBMALLOC_SET_SOFT_HEAP_LIMIT, limitBytes);
    }

    /**
     * Sets the threshold for "huge" allocations.
     * Allocations above this size are not released during regular cleanup.
     * @param thresholdBytes Huge size threshold in bytes
     * @return TBBMALLOC_OK on success, error code on failure
     */
    public static int setHugeSizeThreshold(long thresholdBytes) {
        return nativeScalableAllocationMode(TBBMALLOC_SET_HUGE_SIZE_THRESHOLD, thresholdBytes);
    }

    /**
     * Cleans internal allocator buffers for all threads.
     * @return TBBMALLOC_OK if memory was released, TBBMALLOC_NO_EFFECT if nothing cleaned
     */
    public static int cleanAllBuffers() {
        return nativeScalableAllocationCommand(TBBMALLOC_CLEAN_ALL_BUFFERS, 0);
    }

    /**
     * Cleans internal allocator buffer for the current thread only.
     * @return TBBMALLOC_OK if memory was released, TBBMALLOC_NO_EFFECT if nothing cleaned
     */
    public static int cleanThreadBuffers() {
        return nativeScalableAllocationCommand(TBBMALLOC_CLEAN_THREAD_BUFFERS, 0);
    }

    // ========================================================================
    // 7. tbbmalloc — Memory Pools (rml::MemoryPool)
    // ========================================================================

    /**
     * Creates a new tbbmalloc memory pool.
     * Memory pools allow grouped allocation and deallocation.
     *
     * @param granularity Allocation granularity (0 for default)
     * @param fixedPool If true, memory is consumed at first allocation and never returned
     * @param keepAllMemory If true, memory is kept until pool destruction
     * @return Opaque pool handle (must be destroyed with poolDestroy)
     * @throws RuntimeException if pool creation fails
     */
    public static long poolCreate(long granularity, boolean fixedPool, boolean keepAllMemory) {
        long handle = nativePoolCreate(granularity, fixedPool, keepAllMemory);
        if (handle == 0) {
            throw new RuntimeException("Failed to create TBB memory pool");
        }
        return handle;
    }

    /**
     * Destroys a tbbmalloc memory pool and frees all its memory.
     * @param poolHandle Handle returned by poolCreate
     */
    public static void poolDestroy(long poolHandle) {
        if (poolHandle != 0) {
            nativePoolDestroy(poolHandle);
        }
    }

    /**
     * Allocates memory from a tbbmalloc memory pool.
     * @param poolHandle Handle returned by poolCreate
     * @param size Number of bytes to allocate
     * @return Pointer to allocated memory (0 on failure)
     */
    public static long poolMalloc(long poolHandle, long size) {
        return nativePoolMalloc(poolHandle, size);
    }

    /**
     * Reallocates memory within a tbbmalloc memory pool.
     * @param poolHandle Handle returned by poolCreate
     * @param ptr Previous pointer
     * @param size New size in bytes
     * @return Pointer to reallocated memory (0 on failure)
     */
    public static long poolRealloc(long poolHandle, long ptr, long size) {
        return nativePoolRealloc(poolHandle, ptr, size);
    }

    /**
     * Allocates aligned memory from a tbbmalloc memory pool.
     * @param poolHandle Handle returned by poolCreate
     * @param size Number of bytes to allocate
     * @param alignment Alignment in bytes
     * @return Pointer to aligned memory (0 on failure)
     */
    public static long poolAlignedMalloc(long poolHandle, long size, long alignment) {
        return nativePoolAlignedMalloc(poolHandle, size, alignment);
    }

    /**
     * Reallocates aligned memory within a tbbmalloc memory pool.
     * @param poolHandle Handle returned by poolCreate
     * @param ptr Previous pointer
     * @param size New size in bytes
     * @param alignment Alignment in bytes
     * @return Pointer to reallocated aligned memory (0 on failure)
     */
    public static long poolAlignedRealloc(long poolHandle, long ptr, long size, long alignment) {
        return nativePoolAlignedRealloc(poolHandle, ptr, size, alignment);
    }

    /**
     * Frees memory back to a tbbmalloc memory pool.
     * @param poolHandle Handle returned by poolCreate
     * @param ptr Pointer returned by poolMalloc or related functions
     * @return true if successfully freed
     */
    public static boolean poolFree(long poolHandle, long ptr) {
        return nativePoolFree(poolHandle, ptr);
    }

    /**
     * Resets a tbbmalloc memory pool, freeing all allocations.
     * @param poolHandle Handle returned by poolCreate
     * @return true if successfully reset
     */
    public static boolean poolReset(long poolHandle) {
        return nativePoolReset(poolHandle);
    }

    /**
     * Returns the usable size of a pool-allocated block.
     * @param poolHandle Handle returned by poolCreate
     * @param ptr Pointer returned by poolMalloc
     * @return Usable size in bytes
     */
    public static long poolMsize(long poolHandle, long ptr) {
        return nativePoolMsize(poolHandle, ptr);
    }

    /**
     * Identifies which pool owns a given pointer.
     * @param ptr Pointer to check
     * @return Pool handle that owns the pointer, or 0 if not pool-allocated
     */
    public static long poolIdentify(long ptr) {
        return nativePoolIdentify(ptr);
    }

    // ========================================================================
    // 8. Timing (tick_count.h)
    // ========================================================================

    /**
     * Returns the current high-resolution tick count.
     * Use with ticksPerSecond() to measure elapsed time.
     * @return Current tick count
     */
    public static double tickCount() {
        return nativeTickCount();
    }

    /**
     * Returns the number of ticks per second for the current platform.
     * @return Ticks per second
     */
    public static double ticksPerSecond() {
        return nativeTicksPerSecond();
    }

    /**
     * Measures elapsed time in milliseconds using TBB's high-resolution timer.
     * @param startTicks Start tick count from tickCount()
     * @param endTicks End tick count from tickCount()
     * @return Elapsed time in milliseconds
     */
    public static double elapsedMillis(double startTicks, double endTicks) {
        return (endTicks - startTicks) / ticksPerSecond() * 1000.0;
    }

    // ========================================================================
    // 9. Task Scheduler Handle (global_control.h)
    // ========================================================================

    /**
     * Gets a task scheduler handle for lifecycle control.
     * The handle keeps the TBB scheduler alive even when no work is pending.
     * @return Opaque scheduler handle (must be released with releaseTaskSchedulerHandle)
     */
    public static long getTaskSchedulerHandle() {
        return nativeGetTaskSchedulerHandle();
    }

    /**
     * Releases a task scheduler handle.
     * @param handle Handle returned by getTaskSchedulerHandle
     */
    public static void releaseTaskSchedulerHandle(long handle) {
        if (handle != 0) {
            nativeReleaseTaskSchedulerHandle(handle);
        }
    }

    // ========================================================================
    // 10. Assertion Handler (global_control.h, ext namespace)
    // ========================================================================

    /**
     * Checks if TBB is available in this JDK build.
     * @return true if TBB is statically linked into libjvm
     */
    public static boolean isAvailable() {
        return nativeIsAvailable();
    }

    // ========================================================================
    // Native methods — registered via JNI in Hotspot (tbb_jni.cpp)
    // ========================================================================
    //
    // These native methods are implemented in Hotspot's src/hotspot/share/tbb/tbb_jni.cpp
    // and registered using JNI RegisterNatives mechanism. Since TBB is statically linked
    // into libjvm.dll/so by oneJDK-25, no external library loading is needed.
    //
    // The registration happens in TBBSession's static initializer via registerNatives().

    private static native void registerNatives();
    static {
        registerNatives();
    }

    // Version
    private static native int nativeVersionMajor();
    private static native int nativeVersionMinor();
    private static native int nativeInterfaceVersion();
    private static native String nativeRuntimeVersionString();
    private static native int nativeRuntimeInterfaceVersion();

    // CPU Topology (info.h)
    private static native int nativeNumaNodeCount();
    private static native int nativeNumaDefaultConcurrency(int numaId);
    private static native int nativeCoreTypeCount();
    private static native int nativeDefaultConcurrency();

    // Global Control (global_control.h)
    private static native void nativeSetGlobalControl(int parameter, long value);
    private static native long nativeActiveValue(int parameter);

    // Task Arena (task_arena.h)
    private static native long nativeCreateArena(int maxConcurrency, int reservedForMasters, int priority);
    private static native void nativeCloseArena(long arenaHandle);
    private static native int nativeArenaMaxConcurrency(long arenaHandle);
    private static native boolean nativeArenaIsActive(long arenaHandle);
    private static native int nativeCurrentThreadIndex();
    private static native int nativeMaxConcurrency();

    // tbbmalloc — Scalable Memory (scalable_allocator.h)
    private static native long nativeScalableMalloc(long size);
    private static native void nativeScalableFree(long ptr);
    private static native long nativeScalableRealloc(long ptr, long size);
    private static native long nativeScalableCalloc(long nobj, long size);
    private static native long nativeScalableAlignedMalloc(long size, long alignment);
    private static native long nativeScalableAlignedRealloc(long ptr, long size, long alignment);
    private static native void nativeScalableAlignedFree(long ptr);
    private static native long nativeScalableMsize(long ptr);
    private static native long nativeScalablePosixMemAlign(long alignment, long size);

    // tbbmalloc — Allocation Modes
    private static native int nativeScalableAllocationMode(int param, long value);
    private static native int nativeScalableAllocationCommand(int cmd, long param);

    // tbbmalloc — Memory Pools (rml::MemoryPool)
    private static native long nativePoolCreate(long granularity, boolean fixedPool, boolean keepAllMemory);
    private static native void nativePoolDestroy(long poolHandle);
    private static native long nativePoolMalloc(long poolHandle, long size);
    private static native long nativePoolRealloc(long poolHandle, long ptr, long size);
    private static native long nativePoolAlignedMalloc(long poolHandle, long size, long alignment);
    private static native long nativePoolAlignedRealloc(long poolHandle, long ptr, long size, long alignment);
    private static native boolean nativePoolFree(long poolHandle, long ptr);
    private static native boolean nativePoolReset(long poolHandle);
    private static native long nativePoolMsize(long poolHandle, long ptr);
    private static native long nativePoolIdentify(long ptr);

    // Timing (tick_count.h)
    private static native double nativeTickCount();
    private static native double nativeTicksPerSecond();

    // Task Scheduler Handle
    private static native long nativeGetTaskSchedulerHandle();
    private static native void nativeReleaseTaskSchedulerHandle(long handle);

    // Availability
    private static native boolean nativeIsAvailable();
}
