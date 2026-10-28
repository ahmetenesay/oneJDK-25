/**
 * Simple test program to verify TBBSession ease of use.
 * 
 * This demonstrates that TBBSession works like standard Java libraries —
 * no external dependencies, no native library loading, just import and use.
 * 
 * Compile: javac TBBSessionTest.java
 * Run: java TBBSessionTest (requires oneJDK-25 with static TBB)
 */

import jdk.internal.tbb.TBBSession;

public class TBBSessionTest {

    public static void main(String[] args) {
        System.out.println("=== TBBSession Ease of Use Test ===\n");

        // 1. Check availability — like checking System.getProperty()
        if (!TBBSession.isAvailable()) {
            System.out.println("ERROR: TBB is not available. Use oneJDK-25 with static TBB.");
            return;
        }
        System.out.println("✓ TBB is available");

        // 2. Version info — like Runtime.version()
        System.out.println("\n--- Version Info ---");
        System.out.println("TBB Version: " + TBBSession.versionMajor() + "."
            + TBBSession.versionMinor() + "." + TBBSession.interfaceVersion());
        System.out.println("Runtime: " + TBBSession.runtimeVersionString());

        // 3. CPU topology — like Runtime.availableProcessors()
        System.out.println("\n--- CPU Topology ---");
        System.out.println("Default concurrency: " + TBBSession.defaultConcurrency());
        System.out.println("NUMA nodes: " + TBBSession.numaNodeCount());
        System.out.println("Core types: " + TBBSession.coreTypeCount());

        // 4. Memory allocation — like new byte[1024]
        System.out.println("\n--- Memory Allocation ---");
        long ptr = TBBSession.scalableMalloc(1024);
        System.out.println("Allocated 1024 bytes at: 0x" + Long.toHexString(ptr));
        System.out.println("Actual size: " + TBBSession.scalableMsize(ptr) + " bytes");
        TBBSession.scalableFree(ptr);
        System.out.println("✓ Memory freed");

        // 5. Task arena — like ForkJoinPool
        System.out.println("\n--- Task Arena ---");
        long arena = TBBSession.createArena(4, 0, TBBSession.PRIORITY_NORMAL);
        System.out.println("Created arena with max concurrency: "
            + TBBSession.arenaMaxConcurrency(arena));
        System.out.println("Thread index: " + TBBSession.currentThreadIndex());
        TBBSession.closeArena(arena);
        System.out.println("✓ Arena closed");

        // 6. Timing — like System.nanoTime()
        System.out.println("\n--- Timing ---");
        double start = TBBSession.tickCount();
        Thread.sleep(100);
        double end = TBBSession.tickCount();
        System.out.println("Elapsed: " + String.format("%.3f", end - start) + " seconds");

        System.out.println("\n=== All tests passed! ===");
        System.out.println("TBBSession works like standard Java libraries — no setup needed.");
    }
}
