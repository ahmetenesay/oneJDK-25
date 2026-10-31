/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * Global HotSpot Option Recognition & "Did You Mean?" Engine.
 */

#include "runtime/flags/jvmFlagDidYouMean.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/stringUtils.hpp"
#include <cctype>
#include <cstring>

// Massive technical intent dictionary mapping developer jargon, abbreviations,
// aliases, and intent concepts to canonical HotSpot JVM flags.
const HotSpotIntentEntry JVMFlagDidYouMean::INTENT_REGISTRY[] = {
  // === CATEGORY 1: Garbage Collector Selection ===
  { "g1",                       "UseG1GC",                 "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "g1gc",                     "UseG1GC",                 "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "useg1",                    "UseG1GC",                 "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "g1_gc",                    "UseG1GC",                 "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "garbage_first",            "UseG1GC",                 "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "zgc",                      "UseZGC",                  "Intent: ZGC low-latency scalable garbage collector" },
  { "usezgc",                   "UseZGC",                  "Intent: ZGC low-latency scalable garbage collector" },
  { "z_gc",                     "UseZGC",                  "Intent: ZGC low-latency scalable garbage collector" },
  { "gen_zgc",                  "UseZGC",                  "Intent: Generational ZGC low-latency collector" },
  { "generational_zgc",         "UseZGC",                  "Intent: Generational ZGC low-latency collector" },
  { "shenandoah",               "UseShenandoahGC",         "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "useshenandoah",            "UseShenandoahGC",         "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "shenandoahgc",             "UseShenandoahGC",         "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "shenandoah_gc",            "UseShenandoahGC",         "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "parallel",                 "UseParallelGC",           "Intent: Parallel throughput generational collector" },
  { "parallelgc",               "UseParallelGC",           "Intent: Parallel throughput generational collector" },
  { "useparallel",              "UseParallelGC",           "Intent: Parallel throughput generational collector" },
  { "throughput",               "UseParallelGC",           "Intent: Parallel throughput generational collector" },
  { "throughputgc",             "UseParallelGC",           "Intent: Parallel throughput generational collector" },
  { "serial",                   "UseSerialGC",             "Intent: Serial single-threaded stop-the-world collector" },
  { "serialgc",                 "UseSerialGC",             "Intent: Serial single-threaded stop-the-world collector" },
  { "useserial",                "UseSerialGC",             "Intent: Serial single-threaded stop-the-world collector" },
  { "epsilon",                  "UseEpsilonGC",            "Intent: Epsilon no-op allocation-only collector" },
  { "epsilongc",                "UseEpsilonGC",            "Intent: Epsilon no-op allocation-only collector" },
  { "nogc",                     "UseEpsilonGC",            "Intent: Epsilon no-op allocation-only collector" },
  { "redlet",                   "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },
  { "useredlet",                "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },
  { "redletgc",                 "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },
  { "redlet_gc",                "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },
  { "fast_gc",                  "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },
  { "simd_gc",                  "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },
  { "vector_gc",                "UseRedletGC",             "Intent: RedletGC vectorized microsecond generational GC" },

  // === CATEGORY 2: GC Tuning, Threads, Pause Targets & Heuristics ===
  { "gc_threads",               "ParallelGCThreads",       "Intent: Number of parallel GC evaluation worker threads" },
  { "gcthreads",                "ParallelGCThreads",       "Intent: Number of parallel GC evaluation worker threads" },
  { "parallel_threads",         "ParallelGCThreads",       "Intent: Number of parallel GC evaluation worker threads" },
  { "worker_threads",           "ParallelGCThreads",       "Intent: Number of parallel GC evaluation worker threads" },
  { "conc_threads",             "ConcGCThreads",           "Intent: Number of concurrent GC marking/sweeping threads" },
  { "concthreads",              "ConcGCThreads",           "Intent: Number of concurrent GC marking/sweeping threads" },
  { "concurrent_threads",       "ConcGCThreads",           "Intent: Number of concurrent GC marking/sweeping threads" },
  { "pause_time",               "MaxGCPauseMillis",        "Intent: Target maximum GC pause time in milliseconds" },
  { "max_pause",                "MaxGCPauseMillis",        "Intent: Target maximum GC pause time in milliseconds" },
  { "pause_target",             "MaxGCPauseMillis",        "Intent: Target maximum GC pause time in milliseconds" },
  { "max_gc_pause",             "MaxGCPauseMillis",        "Intent: Target maximum GC pause time in milliseconds" },
  { "gc_pause",                 "MaxGCPauseMillis",        "Intent: Target maximum GC pause time in milliseconds" },
  { "target_pause",             "MaxGCPauseMillis",        "Intent: Target maximum GC pause time in milliseconds" },
  { "pause_interval",           "GCPauseIntervalMillis",   "Intent: Target GC pause interval goal in milliseconds" },
  { "gc_interval",              "GCPauseIntervalMillis",   "Intent: Target GC pause interval goal in milliseconds" },
  { "ihop",                     "InitiatingHeapOccupancyPercent", "Intent: Old gen occupancy percentage triggering concurrent cycle" },
  { "g1_ihop",                  "InitiatingHeapOccupancyPercent", "Intent: G1 initiating heap occupancy percent (IHOP)" },
  { "survivor_ratio",           "SurvivorRatio",           "Intent: Ratio of eden to survivor space size" },
  { "survivorratio",            "SurvivorRatio",           "Intent: Ratio of eden to survivor space size" },
  { "new_ratio",                "NewRatio",                "Intent: Ratio of old gen to young gen space size" },
  { "newratio",                 "NewRatio",                "Intent: Ratio of old gen to young gen space size" },
  { "tenuring_threshold",       "MaxTenuringThreshold",    "Intent: Maximum object age threshold before promotion to old gen" },
  { "tenuring",                 "MaxTenuringThreshold",    "Intent: Maximum object age threshold before promotion to old gen" },
  { "string_dedup",             "UseStringDeduplication",  "Intent: Deduplicate identical String char/byte arrays in memory" },
  { "string_deduplication",     "UseStringDeduplication",  "Intent: Deduplicate identical String char/byte arrays in memory" },
  { "dedup_strings",            "UseStringDeduplication",  "Intent: Deduplicate identical String char/byte arrays in memory" },
  { "periodic_gc",              "G1PeriodicGCInterval",    "Intent: Periodic GC invocation interval for idle heaps" },
  { "idle_gc",                  "G1PeriodicGCInterval",    "Intent: Periodic GC invocation interval for idle heaps" },
  { "reserve_percent",          "G1ReservePercent",        "Intent: Headroom percentage reserved against promotion failures" },
  { "shrink_heap",              "ShrinkHeapInSteps",       "Intent: Gradually uncommit and shrink idle heap pages back to OS" },

  // === CATEGORY 3: Memory Allocation, Paging, Huge Pages & Footprint ===
  { "hugepages",                "UseLargePages",           "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "huge_pages",               "UseLargePages",           "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "largepages",               "UseLargePages",           "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "large_pages",              "UseLargePages",           "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "hugepage",                 "UseLargePages",           "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "thp",                      "UseLargePages",           "Intent: 2MB/1GB hardware huge pages (transparent/explicit)" },
  { "transparent_hugepages",    "UseLargePages",           "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "huge_page_size",           "LargePageSizeInBytes",    "Intent: Page size in bytes for hardware large pages" },
  { "large_page_size",          "LargePageSizeInBytes",    "Intent: Page size in bytes for hardware large pages" },
  { "pretouch",                 "AlwaysPreTouch",          "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "pre_touch",                "AlwaysPreTouch",          "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "alwayspretouch",           "AlwaysPreTouch",          "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "always_pretouch",          "AlwaysPreTouch",          "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "touch_pages",              "AlwaysPreTouch",          "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "advance_payment",          "AlwaysPreTouch",          "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "compressed_oops",          "UseCompressedOops",       "Intent: 32-bit compressed object pointers on 64-bit platforms" },
  { "coops",                    "UseCompressedOops",       "Intent: 32-bit compressed object pointers on 64-bit platforms" },
  { "compressed_refs",          "UseCompressedOops",       "Intent: 32-bit compressed object pointers on 64-bit platforms" },
  { "compressed_class_pointers","UseCompressedClassPointers", "Intent: 32-bit compressed class references in object headers" },
  { "ccp",                      "UseCompressedClassPointers", "Intent: 32-bit compressed class references in object headers" },
  { "compressed_classes",       "UseCompressedClassPointers", "Intent: 32-bit compressed class references in object headers" },
  { "max_ram_percentage",       "MaxRAMPercentage",        "Intent: Maximum heap size as percentage of total available RAM" },
  { "maxrampercentage",         "MaxRAMPercentage",        "Intent: Maximum heap size as percentage of total available RAM" },
  { "maxrampercent",            "MaxRAMPercentage",        "Intent: Maximum heap size as percentage of total available RAM" },
  { "max_ram_percent",          "MaxRAMPercentage",        "Intent: Maximum heap size as percentage of total available RAM" },
  { "ram_percentage",           "MaxRAMPercentage",        "Intent: Maximum heap size as percentage of total available RAM" },
  { "min_ram_percentage",       "MinRAMPercentage",        "Intent: Minimum heap size as percentage of total available RAM" },
  { "minrampercentage",         "MinRAMPercentage",        "Intent: Minimum heap size as percentage of total available RAM" },
  { "initial_ram_percentage",   "InitialRAMPercentage",    "Intent: Initial heap size as percentage of total available RAM" },
  { "max_ram",                  "MaxRAM",                  "Intent: Real physical memory limit cap considered by the VM" },
  { "maxram",                   "MaxRAM",                  "Intent: Real physical memory limit cap considered by the VM" },
  { "metaspace_size",           "MetaspaceSize",           "Intent: Initial metadata and class footprint space size" },
  { "metaspacesize",            "MetaspaceSize",           "Intent: Initial metadata and class footprint space size" },
  { "permgen",                  "MetaspaceSize",           "Intent: Initial metadata and class footprint space (replaces PermGen)" },
  { "perm_gen",                 "MetaspaceSize",           "Intent: Initial metadata and class footprint space (replaces PermGen)" },
  { "max_metaspace_size",       "MaxMetaspaceSize",        "Intent: Maximum permissible metadata and class footprint size" },
  { "maxmetaspacesize",         "MaxMetaspaceSize",        "Intent: Maximum permissible metadata and class footprint size" },
  { "compressed_class_size",    "CompressedClassSpaceSize","Intent: Maximum virtual address space for compressed class pointers" },
  { "soft_ref_policy",          "SoftRefLRUPolicyMSPerMB", "Intent: Lifetime of cleared SoftReferences per MB of free heap" },
  { "soft_max_heap",            "SoftMaxHeapSize",         "Intent: Soft maximum heap target cap for GC ergonomics" },
  { "compact_strings",          "CompactStrings",          "Intent: Use compact Latin-1 byte[] representations for Strings" },

  // === CATEGORY 4: Crash Handling, Core Dumps, OOM Aborts & Telemetry ===
  { "heap_dump",                "HeapDumpOnOutOfMemoryError", "Intent: Automatically write HPROF heap dump on fatal OutOfMemoryError" },
  { "heapdump",                 "HeapDumpOnOutOfMemoryError", "Intent: Automatically write HPROF heap dump on fatal OutOfMemoryError" },
  { "dump_oom",                 "HeapDumpOnOutOfMemoryError", "Intent: Automatically write HPROF heap dump on fatal OutOfMemoryError" },
  { "dumponoom",                "HeapDumpOnOutOfMemoryError", "Intent: Automatically write HPROF heap dump on fatal OutOfMemoryError" },
  { "oom_dump",                 "HeapDumpOnOutOfMemoryError", "Intent: Automatically write HPROF heap dump on fatal OutOfMemoryError" },
  { "hprof",                    "HeapDumpOnOutOfMemoryError", "Intent: Automatically write HPROF heap dump on fatal OutOfMemoryError" },
  { "heap_dump_path",           "HeapDumpPath",            "Intent: Filesystem directory path for HPROF heap dumps" },
  { "dumppath",                 "HeapDumpPath",            "Intent: Filesystem directory path for HPROF heap dumps" },
  { "hprof_path",               "HeapDumpPath",            "Intent: Filesystem directory path for HPROF heap dumps" },
  { "dump_dir",                 "HeapDumpPath",            "Intent: Filesystem directory path for HPROF heap dumps" },
  { "crash_on_oom",             "CrashOnOutOfMemoryError", "Intent: Immediately abort and crash the VM process when heap exhausts" },
  { "die_on_oom",               "CrashOnOutOfMemoryError", "Intent: Immediately abort and crash the VM process when heap exhausts" },
  { "abort_on_oom",             "CrashOnOutOfMemoryError", "Intent: Immediately abort and crash the VM process when heap exhausts" },
  { "exit_on_oom",              "ExitOnOutOfMemoryError",  "Intent: Immediately invoke exit() and terminate process on OutOfMemoryError" },
  { "terminate_on_oom",         "ExitOnOutOfMemoryError",  "Intent: Immediately invoke exit() and terminate process on OutOfMemoryError" },
  { "hs_err",                   "ErrorFile",               "Intent: Target filesystem path for HotSpot crash dump logs (hs_err_pid*.log)" },
  { "error_file",               "ErrorFile",               "Intent: Target filesystem path for HotSpot crash dump logs (hs_err_pid*.log)" },
  { "crash_file",               "ErrorFile",               "Intent: Target filesystem path for HotSpot crash dump logs (hs_err_pid*.log)" },
  { "crash_log",                "ErrorFile",               "Intent: Target filesystem path for HotSpot crash dump logs (hs_err_pid*.log)" },
  { "on_error",                 "OnError",                 "Intent: Execute user shell command or script upon VM fatal crash" },
  { "on_crash",                 "OnError",                 "Intent: Execute user shell command or script upon VM fatal crash" },
  { "on_oom",                   "OnOutOfMemoryError",      "Intent: Execute user shell command or script upon OutOfMemoryError" },
  { "null_pointer_details",     "ShowCodeDetailsInExceptionMessages", "Intent: Print helpful contextual expressions in NullPointerExceptions" },
  { "helpful_npe",              "ShowCodeDetailsInExceptionMessages", "Intent: Print helpful contextual expressions in NullPointerExceptions" },
  { "core_dump",                "CreateCoredumpOnCrash",   "Intent: Allow OS kernel to generate coredump file upon crash" },
  { "coredump",                 "CreateCoredumpOnCrash",   "Intent: Allow OS kernel to generate coredump file upon crash" },

  // === CATEGORY 5: JIT Compilation, C1/C2 Tiers, Inlining & Optimization ===
  { "tiered",                   "TieredCompilation",       "Intent: Enable tiered compilation (Tier 1-3 C1 Client + Tier 4 C2 Server)" },
  { "tiered_compilation",       "TieredCompilation",       "Intent: Enable tiered compilation (Tier 1-3 C1 Client + Tier 4 C2 Server)" },
  { "tieredcompilation",        "TieredCompilation",       "Intent: Enable tiered compilation (Tier 1-3 C1 Client + Tier 4 C2 Server)" },
  { "stop_at_level",            "TieredStopAtLevel",       "Intent: Cap compilation tier (1=C1 simple, 2=C1 limited, 3=C1 full, 4=C2)" },
  { "c1_only",                  "TieredStopAtLevel",       "Intent: Restrict JIT compilation to Tier 1 Client Compiler only" },
  { "compile_threshold",        "CompileThreshold",        "Intent: Invocations before method is compiled by JIT" },
  { "jit_threshold",            "CompileThreshold",        "Intent: Invocations before method is compiled by JIT" },
  { "jit_threads",              "CICompilerCount",         "Intent: Number of background parallel compiler worker threads" },
  { "compiler_threads",         "CICompilerCount",         "Intent: Number of background parallel compiler worker threads" },
  { "inline",                   "Inline",                  "Intent: Enable method inlining optimizations in JIT compilers" },
  { "inlining",                 "Inline",                  "Intent: Enable method inlining optimizations in JIT compilers" },
  { "max_inline_level",         "MaxInlineLevel",          "Intent: Maximum call tree recursion depth for method inlining" },
  { "inline_depth",             "MaxInlineLevel",          "Intent: Maximum call tree recursion depth for method inlining" },
  { "max_inline_size",          "MaxInlineSize",           "Intent: Maximum bytecode size of a method considered for inlining" },
  { "inline_size",              "MaxInlineSize",           "Intent: Maximum bytecode size of a method considered for inlining" },
  { "hot_inline_size",          "FreqInlineSize",          "Intent: Maximum bytecode size of a frequent hot method to inline" },
  { "inline_small_code",        "InlineSmallCode",         "Intent: Inline methods already compiled into small machine code blobs" },
  { "print_compilation",        "PrintCompilation",        "Intent: Print telemetry as JIT compiles methods in real-time" },
  { "printcompilation",         "PrintCompilation",        "Intent: Print telemetry as JIT compiles methods in real-time" },
  { "trace_compilation",        "PrintCompilation",        "Intent: Print telemetry as JIT compiles methods in real-time" },
  { "print_inlining",           "PrintInlining",           "Intent: Print tree explaining which methods were inlined or rejected" },
  { "printinlining",            "PrintInlining",           "Intent: Print tree explaining which methods were inlined or rejected" },
  { "print_assembly",           "PrintAssembly",           "Intent: Disassemble and print JIT-generated machine code assembly" },
  { "disassemble",              "PrintAssembly",           "Intent: Disassemble and print JIT-generated machine code assembly" },
  { "disasm",                   "PrintAssembly",           "Intent: Disassemble and print JIT-generated machine code assembly" },
  { "print_opto_assembly",      "PrintOptoAssembly",       "Intent: Print C2 JIT internal opto assembly representation" },
  { "log_compilation",          "LogCompilation",          "Intent: Log XML JIT compilation decisions and IR passes to hotspot.log" },
  { "escape_analysis",          "DoEscapeAnalysis",        "Intent: Perform escape analysis to scalarize or stack-allocate objects" },
  { "do_escape_analysis",       "DoEscapeAnalysis",        "Intent: Perform escape analysis to scalarize or stack-allocate objects" },
  { "scalar_replacement",       "EliminateAllocations",    "Intent: Eliminate heap allocations of non-escaping objects" },
  { "eliminate_allocations",    "EliminateAllocations",    "Intent: Eliminate heap allocations of non-escaping objects" },
  { "optimize_string_concat",   "OptimizeStringConcat",    "Intent: Optimize chained StringBuilder concatenations into single buffers" },
  { "loop_unroll",              "LoopUnrollLimit",         "Intent: Maximum instruction factor for unrolling loop iterations in C2" },
  { "unroll_limit",             "LoopUnrollLimit",         "Intent: Maximum instruction factor for unrolling loop iterations in C2" },
  { "loop_predicate",           "UseLoopPredicate",        "Intent: Generate loop invariant range check guards before loop pre-headers" },

  // === CATEGORY 6: SIMD, Vectorization, Hardware Instructions & Cryptography ===
  { "vectorize",                "UseSuperWord",            "Intent: Auto-vectorize loop bodies into SIMD parallel operations (SuperWord)" },
  { "vectorization",            "UseSuperWord",            "Intent: Auto-vectorize loop bodies into SIMD parallel operations (SuperWord)" },
  { "superword",                "UseSuperWord",            "Intent: Auto-vectorize loop bodies into SIMD parallel operations (SuperWord)" },
  { "auto_vectorize",           "UseSuperWord",            "Intent: Auto-vectorize loop bodies into SIMD parallel operations (SuperWord)" },
  { "simd",                     "UseSuperWord",            "Intent: Auto-vectorize loop bodies into SIMD parallel operations (SuperWord)" },
  { "max_vector_size",          "MaxVectorSize",           "Intent: Maximum SIMD hardware vector register width in bytes" },
  { "vector_size",              "MaxVectorSize",           "Intent: Maximum SIMD hardware vector register width in bytes" },
  { "vector_width",             "MaxVectorSize",           "Intent: Maximum SIMD hardware vector register width in bytes" },
  { "avx",                      "UseAVX",                  "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "avx2",                     "UseAVX",                  "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "avx512",                   "UseAVX",                  "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "avx10",                    "UseAVX",                  "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "apx",                      "UseAPX",                  "Intent: Intel Advanced Performance Extensions (32 GPRs & CCMP instructions)" },
  { "intel_apx",                "UseAPX",                  "Intent: Intel Advanced Performance Extensions (32 GPRs & CCMP instructions)" },
  { "sse",                      "UseSSE",                  "Intent: Target Streaming SIMD Extensions (SSE level 1 through 4.2)" },
  { "aes",                      "UseAES",                  "Intent: Hardware AES encryption acceleration instructions" },
  { "aes_ni",                   "UseAES",                  "Intent: Hardware AES encryption acceleration instructions" },
  { "aes_intrinsics",           "UseAESIntrinsics",        "Intent: Hardware cryptographic intrinsic functions for AES cipher blocks" },
  { "sha",                      "UseSHA",                  "Intent: Hardware cryptographic SHA acceleration instructions" },
  { "sha_intrinsics",           "UseSHAIntrinsics",        "Intent: Hardware cryptographic intrinsic functions for SHA digest computation" },
  { "fma",                      "UseFMA",                  "Intent: Hardware fused multiply-add (FMA) instructions" },
  { "bmi",                      "UseBMI2Instructions",     "Intent: Bit Manipulation Instruction Set 2 (BZHI, MULX, PDEP, PEXT, etc.)" },
  { "bmi2",                     "UseBMI2Instructions",     "Intent: Bit Manipulation Instruction Set 2 (BZHI, MULX, PDEP, PEXT, etc.)" },
  { "clmul",                    "UseCLMUL",                "Intent: Carry-less multiplication hardware instructions for CRC32 / GCM" },
  { "simd_mismatch",            "UseVectorizedMismatchIntrinsic", "Intent: SIMD vector registers for array comparison and Arrays.mismatch" },
  { "popcount",                 "UsePopCountInstruction",  "Intent: Hardware POPCNT population bit count instructions" },

  // === CATEGORY 7: Threading, Locks, TLAB, Virtual Threads & NUMA ===
  { "tlab",                     "UseTLAB",                 "Intent: Thread-local allocation buffers for lock-free object allocations" },
  { "use_tlab",                 "UseTLAB",                 "Intent: Thread-local allocation buffers for lock-free object allocations" },
  { "tlab_size",                "TLABSize",                "Intent: Starting thread-local allocation buffer initial capacity" },
  { "min_tlab_size",            "MinTLABSize",             "Intent: Minimum thread-local allocation buffer size in bytes" },
  { "zero_tlab",                "ZeroTLAB",                "Intent: Pre-zero thread-local allocation buffers on creation" },
  { "resize_tlab",              "ResizeTLAB",              "Intent: Dynamically resize TLAB allocations based on allocation velocity" },
  { "numa",                     "UseNUMA",                 "Intent: Allocate heap memory across local NUMA nodes" },
  { "numa_interleave",          "UseNUMAInterleaving",     "Intent: Evenly interleave physical memory pages across all NUMA nodes" },
  { "cores",                    "ActiveProcessorCount",    "Intent: Override host CPU core count detected by the JVM runtime" },
  { "num_cores",                "ActiveProcessorCount",    "Intent: Override host CPU core count detected by the JVM runtime" },
  { "cpu_count",                "ActiveProcessorCount",    "Intent: Override host CPU core count detected by the JVM runtime" },
  { "cpus",                     "ActiveProcessorCount",    "Intent: Override host CPU core count detected by the JVM runtime" },
  { "stack_size",               "ThreadStackSize",         "Intent: Default OS stack size per Java application thread" },
  { "thread_stack",             "ThreadStackSize",         "Intent: Default OS stack size per Java application thread" },
  { "compiler_stack",           "CompilerThreadStackSize", "Intent: Stack size for background JIT compiler worker threads" },
  { "frame_pointer",            "PreserveFramePointer",    "Intent: Preserve RBP/frame pointer register for native perf profilers" },
  { "perf_profiling",           "PreserveFramePointer",    "Intent: Preserve RBP/frame pointer register for native perf profilers" },

  // === CATEGORY 8: Unlocking Flags, Diagnostics, Profiling & JFR ===
  { "unlock_diagnostic",        "UnlockDiagnosticVMOptions","Intent: Unlock hidden diagnostic VM options and runtime telemetry" },
  { "diagnostic",               "UnlockDiagnosticVMOptions","Intent: Unlock hidden diagnostic VM options and runtime telemetry" },
  { "unlock_experimental",      "UnlockExperimentalVMOptions","Intent: Unlock bleeding-edge experimental VM flags and features" },
  { "experimental",             "UnlockExperimentalVMOptions","Intent: Unlock bleeding-edge experimental VM flags and features" },
  { "print_flags",              "PrintFlagsFinal",         "Intent: Print values of all VM flags at process exit" },
  { "print_flags_final",        "PrintFlagsFinal",         "Intent: Print values of all VM flags at process exit" },
  { "show_flags",               "PrintFlagsFinal",         "Intent: Print values of all VM flags at process exit" },
  { "print_command_line",       "PrintCommandLineFlags",   "Intent: Print all JVM options explicitly passed on command line" },
  { "print_cmdline",            "PrintCommandLineFlags",   "Intent: Print all JVM options explicitly passed on command line" },
  { "jfr",                      "FlightRecorder",          "Intent: Enable Java Flight Recorder low-overhead event engine" },
  { "flight_recorder",          "FlightRecorder",          "Intent: Enable Java Flight Recorder low-overhead event engine" },
  { "start_flight_recording",   "StartFlightRecording",    "Intent: Start continuous JFR diagnostic profile recording at boot" },
  { "show_version",             "ShowVersion",             "Intent: Print VM version banner and build details before execution" },
  { "version",                  "ShowVersion",             "Intent: Print VM version banner and build details before execution" },
  { "verify_during_gc",         "VerifyDuringGC",          "Intent: Run deep consistency verification checks on heap during GC pauses" },
  { "class_unloading",          "ClassUnloading",          "Intent: Perform class unloading during concurrent marking cycles" },

  // === CATEGORY 9: RedletGC Vectorized Engine Specifics ===
  { "redlet_options",           "RedletOptions",           "Intent: RedletGC direct comma-separated intent sequence" },
  { "turbo",                    "RedletOptions",           "Intent: RedletGC hardware performance governor boost during GC" },
  { "boost",                    "RedletOptions",           "Intent: RedletGC hardware performance governor boost during GC" },
  { "upclock",                  "RedletOptions",           "Intent: RedletGC host CPU upclocking during intensive sweeps" },
  { "strict_vfs",               "RedletVFSStrict",         "Intent: Validate hierarchical VFS memory branches on every slab allocation" },
  { "vfs_strict",               "RedletVFSStrict",         "Intent: Validate hierarchical VFS memory branches on every slab allocation" },
  { "vfs_dialog",               "RedletVFSPanicDialog",    "Intent: Display desktop modal panic dialog on fatal VFS panics" },
  { "rmrp",                     "RedletRMRP",              "Intent: Reduce Memory Resident Set Reclamation (RMRP)" },
  { "trim",                     "RedletRMRP",              "Intent: Reduce Memory Resident Set Reclamation (RMRP)" },
  { "rmrp_method",              "RedletRMRPMethod",        "Intent: RMRP memory reclamation method: Plump or Sequential" },
  { "falline_ratio",            "RedletFallineCompactRatio","Intent: Falline compaction threshold ratio (0.05 to 0.95)" },
  { "redlet_simd",              "RedletSIMD",              "Intent: RedletGC vector engine: auto, avx10_2, avx512, sve2, rvv" },
  { "hetero_arenas",            "RedletHeteroArenas",      "Intent: Enable P-Core and E-Core heterogeneous task arenas via oneTBB" },
  { "gpu_offload",              "RedletGPUOffload",        "Intent: GPU accelerator backend: none, levelzero, hip" },
  { "redlet_tlab",              "RedletMaxTLABSize",       "Intent: Maximum TLAB size to use with RedletGC" },
  { "elastic_tlab",             "RedletElasticTLAB",       "Intent: Use elastic policy to dynamically manage Redlet TLAB sizes" }
};

const size_t JVMFlagDidYouMean::INTENT_REGISTRY_COUNT =
    sizeof(JVMFlagDidYouMean::INTENT_REGISTRY) / sizeof(JVMFlagDidYouMean::INTENT_REGISTRY[0]);

void JVMFlagDidYouMean::normalize_string(const char* src, size_t src_len, char* dst, size_t dst_sz) {
  if (dst == nullptr || dst_sz == 0) return;
  dst[0] = '\0';
  if (src == nullptr || src_len == 0) return;

  size_t out_idx = 0;
  for (size_t i = 0; i < src_len && out_idx + 1 < dst_sz; ++i) {
    char c = src[i];
    if (c == '_' || c == '-' || c == '+' || c == ':' || c == '=') {
      continue; // skip punctuation and option prefixes
    }
    dst[out_idx++] = (char)tolower((unsigned char)c);
  }
  dst[out_idx] = '\0';
}

int JVMFlagDidYouMean::damerau_levenshtein_distance(const char* s1, size_t l1, const char* s2, size_t l2) {
  if (l1 > 64) l1 = 64;
  if (l2 > 64) l2 = 64;
  int d[65][65];
  for (size_t i = 0; i <= l1; ++i) d[i][0] = (int)i;
  for (size_t j = 0; j <= l2; ++j) d[0][j] = (int)j;

  for (size_t i = 1; i <= l1; ++i) {
    for (size_t j = 1; j <= l2; ++j) {
      int cost = (tolower((unsigned char)s1[i - 1]) == tolower((unsigned char)s2[j - 1])) ? 0 : 1;
      int del = d[i - 1][j] + 1;
      int ins = d[i][j - 1] + 1;
      int sub = d[i - 1][j - 1] + cost;
      int m = (del < ins) ? del : ins;
      d[i][j] = (m < sub) ? m : sub;
      // Damerau transposition check
      if (i > 1 && j > 1 &&
          tolower((unsigned char)s1[i - 1]) == tolower((unsigned char)s2[j - 2]) &&
          tolower((unsigned char)s1[i - 2]) == tolower((unsigned char)s2[j - 1])) {
        if (d[i - 2][j - 2] + 1 < d[i][j]) {
          d[i][j] = d[i - 2][j - 2] + 1;
        }
      }
    }
  }
  return d[l1][l2];
}

double JVMFlagDidYouMean::jaro_winkler_similarity(const char* s1, size_t l1, const char* s2, size_t l2) {
  if (l1 == 0 && l2 == 0) return 1.0;
  if (l1 == 0 || l2 == 0) return 0.0;
  if (l1 > 64) l1 = 64;
  if (l2 > 64) l2 = 64;

  int match_distance = (int)(MAX2(l1, l2) / 2) - 1;
  if (match_distance < 0) match_distance = 0;

  bool s1_matches[64] = { false };
  bool s2_matches[64] = { false };
  int matches = 0;

  for (size_t i = 0; i < l1; ++i) {
    int start = MAX2(0, (int)i - match_distance);
    int end = MIN2((int)i + match_distance + 1, (int)l2);
    for (int j = start; j < end; ++j) {
      if (s2_matches[j]) continue;
      if (tolower((unsigned char)s1[i]) == tolower((unsigned char)s2[j])) {
        s1_matches[i] = true;
        s2_matches[j] = true;
        matches++;
        break;
      }
    }
  }
  if (matches == 0) return 0.0;

  double transpositions = 0;
  size_t k = 0;
  for (size_t i = 0; i < l1; ++i) {
    if (!s1_matches[i]) continue;
    while (k < l2 && !s2_matches[k]) k++;
    if (k < l2 && tolower((unsigned char)s1[i]) != tolower((unsigned char)s2[k])) {
      transpositions += 0.5;
    }
    k++;
  }

  double m = (double)matches;
  double jaro = ((m / (double)l1) + (m / (double)l2) + ((m - transpositions) / m)) / 3.0;

  // Winkler prefix boost (up to 4 chars)
  int prefix = 0;
  size_t max_prefix = MIN2((size_t)4, MIN2(l1, l2));
  for (size_t i = 0; i < max_prefix; ++i) {
    if (tolower((unsigned char)s1[i]) == tolower((unsigned char)s2[i])) {
      prefix++;
    } else {
      break;
    }
  }
  return jaro + prefix * 0.1 * (1.0 - jaro);
}

JVMFlag* JVMFlagDidYouMean::resolve(const char* name, size_t length, bool allow_locked, const char** out_hint) {
  if (out_hint != nullptr) {
    *out_hint = nullptr;
  }
  if (name == nullptr || length == 0) {
    return nullptr;
  }

  char clean_query[128];
  normalize_string(name, length, clean_query, sizeof(clean_query));
  size_t query_len = strlen(clean_query);
  if (query_len == 0) {
    return nullptr;
  }

  // Best candidate tracking
  JVMFlag* best_flag = nullptr;
  double best_score = -1.0;
  const char* best_hint = nullptr;

  // 1. Direct Intent Registry Lookup
  for (size_t i = 0; i < INTENT_REGISTRY_COUNT; ++i) {
    const HotSpotIntentEntry& entry = INTENT_REGISTRY[i];
    char clean_alias[128];
    normalize_string(entry.alias, strlen(entry.alias), clean_alias, sizeof(clean_alias));
    size_t alias_len = strlen(clean_alias);

    if (strcmp(clean_query, clean_alias) == 0) {
      // Exact intent alias match!
      JVMFlag* target = JVMFlag::find_declared_flag(entry.canonical_flag);
      if (target != nullptr) {
        if (allow_locked || target->is_unlocked() || target->is_unlocker()) {
          if (out_hint != nullptr) {
            *out_hint = entry.technical_doc;
          }
          return target; // Immediate high-priority resolution!
        }
      }
    } else {
      // Fuzzy intent alias check
      int dl = damerau_levenshtein_distance(clean_query, query_len, clean_alias, alias_len);
      double jw = jaro_winkler_similarity(clean_query, query_len, clean_alias, alias_len);
      double intent_score = 0.0;
      if (dl <= 1) {
        intent_score = MAX2(jw, 0.94);
      } else if (dl == 2 && query_len >= 5 && alias_len >= 5) {
        intent_score = MAX2(jw, 0.86);
      } else if (jw >= 0.88) {
        intent_score = jw;
      }

      if (intent_score > best_score) {
        JVMFlag* target = JVMFlag::find_declared_flag(entry.canonical_flag);
        if (target != nullptr && (allow_locked || target->is_unlocked() || target->is_unlocker())) {
          best_score = intent_score;
          best_flag = target;
          best_hint = entry.technical_doc;
        }
      }
    }
  }

  // 2. Scan All JVM Flags in the Flag Table
  for (JVMFlag* current = JVMFlag::flags; current->name() != nullptr; current++) {
    if (current->is_constant_in_binary()) {
      continue;
    }
    if (!allow_locked && !(current->is_unlocked() || current->is_unlocker())) {
      continue;
    }

    const char* flag_name = current->name();
    size_t flag_raw_len = strlen(flag_name);
    char clean_target[128];
    normalize_string(flag_name, flag_raw_len, clean_target, sizeof(clean_target));
    size_t target_len = strlen(clean_target);

    // 2a. Case/separator exact match
    if (strcmp(clean_query, clean_target) == 0) {
      if (out_hint != nullptr) {
        *out_hint = "Case or separator mismatch";
      }
      return current; // Immediate match!
    }

    // 2b. Affix match (e.g. user typed "LargePages" for "UseLargePages")
    if (target_len == query_len + 3 && strncmp(clean_target, "use", 3) == 0 && strcmp(clean_target + 3, clean_query) == 0) {
      if (0.97 > best_score) {
        best_score = 0.97;
        best_flag = current;
        best_hint = "Affix match ('Use' prefix omitted)";
      }
      continue;
    }
    if (query_len == target_len + 3 && strncmp(clean_query, "use", 3) == 0 && strcmp(clean_query + 3, clean_target) == 0) {
      if (0.97 > best_score) {
        best_score = 0.97;
        best_flag = current;
        best_hint = "Affix match ('Use' prefix added)";
      }
      continue;
    }

    // 2c. Edit distance and typo similarity
    int dl = damerau_levenshtein_distance(clean_query, query_len, clean_target, target_len);
    double jw = jaro_winkler_similarity(clean_query, query_len, clean_target, target_len);
    double dice = StringUtils::similarity(flag_name, flag_raw_len, name, length);

    double flag_score = 0.0;
    const char* flag_hint = nullptr;

    if (dl == 1) {
      flag_score = MAX2(jw, 0.95);
      flag_hint = "Typo match via edit distance";
    } else if (dl == 2 && query_len >= 6 && target_len >= 6) {
      flag_score = MAX2(jw, 0.88);
      flag_hint = "Typo match via edit distance";
    } else {
      flag_score = MAX2(jw, dice);
      if (flag_score >= 0.78) {
        flag_hint = "Fuzzy string similarity match";
      }
    }

    if (flag_score > best_score) {
      best_score = flag_score;
      best_flag = current;
      best_hint = flag_hint;
    }
  }

  // 3. Final Threshold Validation (HotSpot standard similarity threshold is 0.70)
  double threshold = 0.70;
  if (best_flag != nullptr && best_score >= threshold) {
    if (out_hint != nullptr) {
      *out_hint = best_hint;
    }
    return best_flag;
  }

  return nullptr;
}
