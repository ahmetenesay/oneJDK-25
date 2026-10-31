/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * Global HotSpot Option Recognition & "Did You Mean?" Engine.
 */

#include "runtime/flags/jvmFlagDidYouMean.hpp"
#include "jvm_io.h"
#include "runtime/flags/jvmFlag.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/stringUtils.hpp"
#include <cctype>
#include <cstring>

char JVMFlagDidYouMean::_formatted_hint_buffer[256] = { 0 };

// Massive technical intent dictionary mapping developer jargon, abbreviations,
// aliases, and intent concepts to canonical HotSpot JVM flags across different types.
const HotSpotIntentEntry JVMFlagDidYouMean::INTENT_REGISTRY[] = {
  // =========================================================================
  // GROUP 1: Boolean Flags (bool)
  // =========================================================================

  // --- 1.1 Garbage Collector Selection ---
  { "g1",                       "UseG1GC",                 "bool", "true", "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "g1gc",                     "UseG1GC",                 "bool", "true", "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "useg1",                    "UseG1GC",                 "bool", "true", "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "g1_gc",                    "UseG1GC",                 "bool", "true", "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "garbage_first",            "UseG1GC",                 "bool", "true", "Intent: G1 (Garbage-First) concurrent generational collector" },
  { "zgc",                      "UseZGC",                  "bool", "true", "Intent: ZGC scalable low-latency garbage collector" },
  { "usezgc",                   "UseZGC",                  "bool", "true", "Intent: ZGC scalable low-latency garbage collector" },
  { "z_gc",                     "UseZGC",                  "bool", "true", "Intent: ZGC scalable low-latency garbage collector" },
  { "generational_zgc",         "UseZGC",                  "bool", "true", "Intent: Generational ZGC low-latency collector" },
  { "gen_zgc",                  "UseZGC",                  "bool", "true", "Intent: Generational ZGC low-latency collector" },
  { "shenandoah",               "UseShenandoahGC",         "bool", "true", "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "useshenandoah",            "UseShenandoahGC",         "bool", "true", "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "shenandoahgc",             "UseShenandoahGC",         "bool", "true", "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "shenandoah_gc",            "UseShenandoahGC",         "bool", "true", "Intent: Shenandoah ultra-low pause time garbage collector" },
  { "parallel",                 "UseParallelGC",           "bool", "true", "Intent: Parallel throughput generational collector" },
  { "parallelgc",               "UseParallelGC",           "bool", "true", "Intent: Parallel throughput generational collector" },
  { "useparallel",              "UseParallelGC",           "bool", "true", "Intent: Parallel throughput generational collector" },
  { "throughput",               "UseParallelGC",           "bool", "true", "Intent: Parallel throughput generational collector" },
  { "throughputgc",             "UseParallelGC",           "bool", "true", "Intent: Parallel throughput generational collector" },
  { "serial",                   "UseSerialGC",             "bool", "true", "Intent: Serial single-threaded stop-the-world collector" },
  { "serialgc",                 "UseSerialGC",             "bool", "true", "Intent: Serial single-threaded stop-the-world collector" },
  { "useserial",                "UseSerialGC",             "bool", "true", "Intent: Serial single-threaded stop-the-world collector" },
  { "single_thread_gc",         "UseSerialGC",             "bool", "true", "Intent: Serial single-threaded stop-the-world collector" },
  { "epsilon",                  "UseEpsilonGC",            "bool", "true", "Intent: Epsilon no-op allocation-only collector" },
  { "epsilongc",                "UseEpsilonGC",            "bool", "true", "Intent: Epsilon no-op allocation-only collector" },
  { "nogc",                     "UseEpsilonGC",            "bool", "true", "Intent: Epsilon no-op allocation-only collector" },
  { "no_gc",                    "UseEpsilonGC",            "bool", "true", "Intent: Epsilon no-op allocation-only collector" },
  { "redlet",                   "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },
  { "useredlet",                "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },
  { "redletgc",                 "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },
  { "redlet_gc",                "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },
  { "fast_gc",                  "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },
  { "simd_gc",                  "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },
  { "vector_gc",                "UseRedletGC",             "bool", "true", "Intent: RedletGC vectorized microsecond generational GC" },

  // --- 1.2 GC Features, Deduplication & Verification ---
  { "string_dedup",             "UseStringDeduplication",  "bool", "true", "Intent: Deduplicate identical String char/byte arrays in memory" },
  { "string_deduplication",     "UseStringDeduplication",  "bool", "true", "Intent: Deduplicate identical String char/byte arrays in memory" },
  { "dedup_strings",            "UseStringDeduplication",  "bool", "true", "Intent: Deduplicate identical String char/byte arrays in memory" },
  { "shrink_heap",              "ShrinkHeapInSteps",       "bool", "true", "Intent: Gradually uncommit and shrink idle heap pages back to OS" },
  { "uncommit_heap",            "ShrinkHeapInSteps",       "bool", "true", "Intent: Gradually uncommit and shrink idle heap pages back to OS" },
  { "explicit_gc_invokes_concurrent", "ExplicitGCInvokesConcurrent", "bool", "true", "Intent: System.gc() triggers concurrent cycle rather than STW" },
  { "system_gc_concurrent",     "ExplicitGCInvokesConcurrent", "bool", "true", "Intent: System.gc() triggers concurrent cycle rather than STW" },
  { "disable_explicit_gc",      "DisableExplicitGC",       "bool", "true", "Intent: Ignore all System.gc() calls from user application" },
  { "ignore_system_gc",         "DisableExplicitGC",       "bool", "true", "Intent: Ignore all System.gc() calls from user application" },
  { "always_tenure",            "AlwaysTenure",            "bool", "true", "Intent: Promote all surviving objects directly to old gen" },
  { "never_tenure",             "NeverTenure",             "bool", "true", "Intent: Keep objects in young gen as long as survivor spaces fit" },
  { "class_unloading",          "ClassUnloading",          "bool", "true", "Intent: Perform class unloading during concurrent marking cycles" },
  { "unload_classes",           "ClassUnloading",          "bool", "true", "Intent: Perform class unloading during concurrent marking cycles" },
  { "concurrent_class_unloading","ClassUnloadingWithConcurrentMark","bool","true", "Intent: Unload dynamic classes concurrently without STW pause" },
  { "verify_during_gc",         "VerifyDuringGC",          "bool", "true", "Intent: Run deep consistency verification checks on heap during GC" },
  { "verify_before_gc",         "VerifyBeforeGC",          "bool", "true", "Intent: Verify heap integrity prior to initiating GC pause" },
  { "verify_after_gc",          "VerifyAfterGC",           "bool", "true", "Intent: Verify heap integrity immediately after finishing GC pause" },

  // --- 1.3 Memory, Paging & Object Representation ---
  { "hugepages",                "UseLargePages",           "bool", "true", "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "huge_pages",               "UseLargePages",           "bool", "true", "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "largepages",               "UseLargePages",           "bool", "true", "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "large_pages",              "UseLargePages",           "bool", "true", "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "hugepage",                 "UseLargePages",           "bool", "true", "Intent: 2MB/1GB hardware huge pages for TLB cache hits" },
  { "thp",                      "UseLargePages",           "bool", "true", "Intent: 2MB/1GB hardware huge pages (transparent/explicit)" },
  { "transparent_hugepages",    "UseTransparentHugePages", "bool", "true", "Intent: Transparent Huge Pages (madvise/always) under Linux" },
  { "transparent_huge_pages",   "UseTransparentHugePages", "bool", "true", "Intent: Transparent Huge Pages (madvise/always) under Linux" },
  { "pretouch",                 "AlwaysPreTouch",          "bool", "true", "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "pre_touch",                "AlwaysPreTouch",          "bool", "true", "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "alwayspretouch",           "AlwaysPreTouch",          "bool", "true", "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "always_pretouch",          "AlwaysPreTouch",          "bool", "true", "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "touch_pages",              "AlwaysPreTouch",          "bool", "true", "Intent: Pre-touch and map committed heap pages at VM startup" },
  { "advance_payment",          "AlwaysPreTouch",          "bool", "true", "Intent: Pre-touch committed heap pages during initialization" },
  { "compressed_oops",          "UseCompressedOops",       "bool", "true", "Intent: 32-bit compressed object pointers on 64-bit platforms" },
  { "coops",                    "UseCompressedOops",       "bool", "true", "Intent: 32-bit compressed object pointers on 64-bit platforms" },
  { "compressed_refs",          "UseCompressedOops",       "bool", "true", "Intent: 32-bit compressed object pointers on 64-bit platforms" },
  { "compressed_class_pointers","UseCompressedClassPointers","bool","true", "Intent: 32-bit compressed class references in object headers" },
  { "ccp",                      "UseCompressedClassPointers","bool","true", "Intent: 32-bit compressed class references in object headers" },
  { "compressed_classes",       "UseCompressedClassPointers","bool","true", "Intent: 32-bit compressed class references in object headers" },
  { "compact_strings",          "CompactStrings",          "bool", "true", "Intent: Use compact Latin-1 byte[] representations for Strings" },
  { "latin1_strings",           "CompactStrings",          "bool", "true", "Intent: Use compact Latin-1 byte[] representations for Strings" },
  { "numa",                     "UseNUMA",                 "bool", "true", "Intent: Allocate heap memory across local NUMA nodes" },
  { "use_numa",                 "UseNUMA",                 "bool", "true", "Intent: Allocate heap memory across local NUMA nodes" },
  { "numa_interleaving",        "UseNUMAInterleaving",     "bool", "true", "Intent: Evenly interleave physical memory pages across all NUMA nodes" },
  { "numa_interleave",          "UseNUMAInterleaving",     "bool", "true", "Intent: Evenly interleave physical memory pages across all NUMA nodes" },

  // --- 1.4 Crash Diagnostics & OOM ---
  { "heap_dump",                "HeapDumpOnOutOfMemoryError","bool","true", "Intent: Automatically write HPROF heap dump on OutOfMemoryError" },
  { "heapdump",                 "HeapDumpOnOutOfMemoryError","bool","true", "Intent: Automatically write HPROF heap dump on OutOfMemoryError" },
  { "dump_oom",                 "HeapDumpOnOutOfMemoryError","bool","true", "Intent: Automatically write HPROF heap dump on OutOfMemoryError" },
  { "dumponoom",                "HeapDumpOnOutOfMemoryError","bool","true", "Intent: Automatically write HPROF heap dump on OutOfMemoryError" },
  { "oom_dump",                 "HeapDumpOnOutOfMemoryError","bool","true", "Intent: Automatically write HPROF heap dump on OutOfMemoryError" },
  { "hprof",                    "HeapDumpOnOutOfMemoryError","bool","true", "Intent: Automatically write HPROF heap dump on OutOfMemoryError" },
  { "crash_on_oom",             "CrashOnOutOfMemoryError", "bool", "true", "Intent: Immediately abort and crash VM process when heap exhausts" },
  { "die_on_oom",               "CrashOnOutOfMemoryError", "bool", "true", "Intent: Immediately abort and crash VM process when heap exhausts" },
  { "abort_on_oom",             "CrashOnOutOfMemoryError", "bool", "true", "Intent: Immediately abort and crash VM process when heap exhausts" },
  { "exit_on_oom",              "ExitOnOutOfMemoryError",  "bool", "true", "Intent: Immediately invoke exit() and terminate on OutOfMemoryError" },
  { "terminate_on_oom",         "ExitOnOutOfMemoryError",  "bool", "true", "Intent: Immediately invoke exit() and terminate on OutOfMemoryError" },
  { "core_dump",                "CreateCoredumpOnCrash",   "bool", "true", "Intent: Allow OS kernel to generate coredump file upon crash" },
  { "coredump",                 "CreateCoredumpOnCrash",   "bool", "true", "Intent: Allow OS kernel to generate coredump file upon crash" },
  { "show_message_box",         "ShowMessageBoxOnError",   "bool", "true", "Intent: Show native modal dialog box on fatal crash" },
  { "null_pointer_details",     "ShowCodeDetailsInExceptionMessages","bool","true","Intent: Print helpful contextual expressions in NullPointerExceptions" },
  { "helpful_npe",              "ShowCodeDetailsInExceptionMessages","bool","true","Intent: Print helpful contextual expressions in NullPointerExceptions" },

  // --- 1.5 JIT Compilation & Inlining ---
  { "tiered",                   "TieredCompilation",       "bool", "true", "Intent: Enable tiered compilation (Tier 1-3 C1 Client + Tier 4 C2)" },
  { "tiered_compilation",       "TieredCompilation",       "bool", "true", "Intent: Enable tiered compilation (Tier 1-3 C1 Client + Tier 4 C2)" },
  { "inline",                   "Inline",                  "bool", "true", "Intent: Enable method inlining optimizations in JIT compilers" },
  { "inlining",                 "Inline",                  "bool", "true", "Intent: Enable method inlining optimizations in JIT compilers" },
  { "inline_small_code",        "InlineSmallCode",         "bool", "true", "Intent: Inline methods already compiled into small machine code blobs" },
  { "print_compilation",        "PrintCompilation",        "bool", "true", "Intent: Print telemetry as JIT compiles methods in real-time" },
  { "printcompilation",         "PrintCompilation",        "bool", "true", "Intent: Print telemetry as JIT compiles methods in real-time" },
  { "trace_compilation",        "PrintCompilation",        "bool", "true", "Intent: Print telemetry as JIT compiles methods in real-time" },
  { "print_inlining",           "PrintInlining",           "bool", "true", "Intent: Print tree explaining which methods were inlined or rejected" },
  { "printinlining",            "PrintInlining",           "bool", "true", "Intent: Print tree explaining which methods were inlined or rejected" },
  { "print_assembly",           "PrintAssembly",           "bool", "true", "Intent: Disassemble and print JIT-generated machine code assembly" },
  { "disassemble",              "PrintAssembly",           "bool", "true", "Intent: Disassemble and print JIT-generated machine code assembly" },
  { "disasm",                   "PrintAssembly",           "bool", "true", "Intent: Disassemble and print JIT-generated machine code assembly" },
  { "print_opto_assembly",      "PrintOptoAssembly",       "bool", "true", "Intent: Print C2 JIT internal opto assembly representation" },
  { "log_compilation",          "LogCompilation",          "bool", "true", "Intent: Log XML JIT compilation decisions and IR passes to hotspot.log" },
  { "escape_analysis",          "DoEscapeAnalysis",        "bool", "true", "Intent: Perform escape analysis to scalarize or stack-allocate objects" },
  { "do_escape_analysis",       "DoEscapeAnalysis",        "bool", "true", "Intent: Perform escape analysis to scalarize or stack-allocate objects" },
  { "scalar_replacement",       "EliminateAllocations",    "bool", "true", "Intent: Eliminate heap allocations of non-escaping objects" },
  { "eliminate_allocations",    "EliminateAllocations",    "bool", "true", "Intent: Eliminate heap allocations of non-escaping objects" },
  { "optimize_string_concat",   "OptimizeStringConcat",    "bool", "true", "Intent: Optimize chained StringBuilder concatenations into single buffers" },
  { "loop_predicate",           "UseLoopPredicate",        "bool", "true", "Intent: Generate loop invariant range check guards before loop pre-headers" },
  { "vectorize",                "UseSuperWord",            "bool", "true", "Intent: Auto-vectorize loop bodies into SIMD parallel operations" },
  { "vectorization",            "UseSuperWord",            "bool", "true", "Intent: Auto-vectorize loop bodies into SIMD parallel operations" },
  { "superword",                "UseSuperWord",            "bool", "true", "Intent: Auto-vectorize loop bodies into SIMD parallel operations" },
  { "auto_vectorize",           "UseSuperWord",            "bool", "true", "Intent: Auto-vectorize loop bodies into SIMD parallel operations" },
  { "simd",                     "UseSuperWord",            "bool", "true", "Intent: Auto-vectorize loop bodies into SIMD parallel operations" },

  // --- 1.6 Hardware Instructions & Cryptography ---
  { "avx",                      "UseAVX",                  "bool", "true", "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "avx2",                     "UseAVX",                  "bool", "true", "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "avx512",                   "UseAVX",                  "bool", "true", "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "avx10",                    "UseAVX",                  "bool", "true", "Intent: Target Intel/AMD AVX extensions (1=AVX, 2=AVX2, 3=AVX-512/AVX10)" },
  { "apx",                      "UseAPX",                  "bool", "true", "Intent: Intel Advanced Performance Extensions (32 GPRs & CCMP instructions)" },
  { "intel_apx",                "UseAPX",                  "bool", "true", "Intent: Intel Advanced Performance Extensions (32 GPRs & CCMP instructions)" },
  { "sse",                      "UseSSE",                  "bool", "true", "Intent: Target Streaming SIMD Extensions (SSE level 1 through 4.2)" },
  { "aes",                      "UseAES",                  "bool", "true", "Intent: Hardware AES encryption acceleration instructions" },
  { "aes_ni",                   "UseAES",                  "bool", "true", "Intent: Hardware AES encryption acceleration instructions" },
  { "aes_intrinsics",           "UseAESIntrinsics",        "bool", "true", "Intent: Hardware cryptographic intrinsic functions for AES cipher blocks" },
  { "sha",                      "UseSHA",                  "bool", "true", "Intent: Hardware cryptographic SHA acceleration instructions" },
  { "sha_intrinsics",           "UseSHAIntrinsics",        "bool", "true", "Intent: Hardware cryptographic intrinsic functions for SHA digest computation" },
  { "fma",                      "UseFMA",                  "bool", "true", "Intent: Hardware fused multiply-add (FMA) instructions" },
  { "bmi",                      "UseBMI2Instructions",     "bool", "true", "Intent: Bit Manipulation Instruction Set 2 (BZHI, MULX, PDEP, PEXT, etc.)" },
  { "bmi2",                     "UseBMI2Instructions",     "bool", "true", "Intent: Bit Manipulation Instruction Set 2 (BZHI, MULX, PDEP, PEXT, etc.)" },
  { "clmul",                    "UseCLMUL",                "bool", "true", "Intent: Carry-less multiplication hardware instructions for CRC32 / GCM" },
  { "simd_mismatch",            "UseVectorizedMismatchIntrinsic","bool","true","Intent: SIMD vector registers for array comparison and Arrays.mismatch" },
  { "popcount",                 "UsePopCountInstruction",  "bool", "true", "Intent: Hardware POPCNT population bit count instructions" },

  // --- 1.7 TLAB, Profiling & Diagnostics ---
  { "tlab",                     "UseTLAB",                 "bool", "true", "Intent: Thread-local allocation buffers for lock-free object allocations" },
  { "use_tlab",                 "UseTLAB",                 "bool", "true", "Intent: Thread-local allocation buffers for lock-free object allocations" },
  { "zero_tlab",                "ZeroTLAB",                "bool", "true", "Intent: Pre-zero thread-local allocation buffers on creation" },
  { "resize_tlab",              "ResizeTLAB",              "bool", "true", "Intent: Dynamically resize TLAB allocations based on allocation velocity" },
  { "frame_pointer",            "PreserveFramePointer",    "bool", "true", "Intent: Preserve RBP/frame pointer register for native perf profilers" },
  { "preserve_frame_pointer",   "PreserveFramePointer",    "bool", "true", "Intent: Preserve RBP/frame pointer register for native perf profilers" },
  { "perf_profiling",           "PreserveFramePointer",    "bool", "true", "Intent: Preserve RBP/frame pointer register for native perf profilers" },
  { "unlock_diagnostic",        "UnlockDiagnosticVMOptions","bool","true", "Intent: Unlock hidden diagnostic VM options and runtime telemetry" },
  { "diagnostic",               "UnlockDiagnosticVMOptions","bool","true", "Intent: Unlock hidden diagnostic VM options and runtime telemetry" },
  { "unlock_experimental",      "UnlockExperimentalVMOptions","bool","true","Intent: Unlock bleeding-edge experimental VM flags and features" },
  { "experimental",             "UnlockExperimentalVMOptions","bool","true","Intent: Unlock bleeding-edge experimental VM flags and features" },
  { "print_flags",              "PrintFlagsFinal",         "bool", "true", "Intent: Print values of all VM flags at process exit" },
  { "print_flags_final",        "PrintFlagsFinal",         "bool", "true", "Intent: Print values of all VM flags at process exit" },
  { "show_flags",               "PrintFlagsFinal",         "bool", "true", "Intent: Print values of all VM flags at process exit" },
  { "print_command_line",       "PrintCommandLineFlags",   "bool", "true", "Intent: Print all JVM options explicitly passed on command line" },
  { "print_cmdline",            "PrintCommandLineFlags",   "bool", "true", "Intent: Print all JVM options explicitly passed on command line" },
  { "show_version",             "ShowVersion",             "bool", "true", "Intent: Print VM version banner and build details before execution" },
  { "version",                  "ShowVersion",             "bool", "true", "Intent: Print VM version banner and build details before execution" },
  { "jfr",                      "FlightRecorder",          "bool", "true", "Intent: Enable Java Flight Recorder low-overhead event engine" },
  { "flight_recorder",          "FlightRecorder",          "bool", "true", "Intent: Enable Java Flight Recorder low-overhead event engine" },

  // --- 1.8 RedletGC Booleans ---
  { "strict_vfs",               "RedletVFSStrict",         "bool", "true", "Intent: Validate hierarchical VFS memory branches on every allocation" },
  { "vfs_strict",               "RedletVFSStrict",         "bool", "true", "Intent: Validate hierarchical VFS memory branches on every allocation" },
  { "vfs_dialog",               "RedletVFSPanicDialog",    "bool", "true", "Intent: Display desktop modal panic dialog on fatal VFS panics" },
  { "panic_dialog",             "RedletVFSPanicDialog",    "bool", "true", "Intent: Display desktop modal panic dialog on fatal VFS panics" },
  { "rmrp",                     "RedletRMRP",              "bool", "true", "Intent: Reduce Memory Resident Set Reclamation (RMRP)" },
  { "trim",                     "RedletRMRP",              "bool", "true", "Intent: Reduce Memory Resident Set Reclamation (RMRP)" },
  { "hetero_arenas",            "RedletHeteroArenas",      "bool", "true", "Intent: Enable P-Core and E-Core heterogeneous task arenas via oneTBB" },
  { "elastic_tlab",             "RedletElasticTLAB",       "bool", "true", "Intent: Use elastic policy to dynamically manage Redlet TLAB sizes" },

  // =========================================================================
  // GROUP 2: Memory Sizing & Capacities (size_t)
  // =========================================================================
  { "max_heap",                 "MaxHeapSize",             "size_t", "4g",   "Intent: Maximum Java heap size cap (equivalent to -Xmx)" },
  { "max_heap_size",            "MaxHeapSize",             "size_t", "4g",   "Intent: Maximum Java heap size cap (equivalent to -Xmx)" },
  { "xmx",                      "MaxHeapSize",             "size_t", "4g",   "Intent: Maximum Java heap size cap (equivalent to -Xmx)" },
  { "heap_max",                 "MaxHeapSize",             "size_t", "4g",   "Intent: Maximum Java heap size cap (equivalent to -Xmx)" },
  { "initial_heap",             "InitialHeapSize",         "size_t", "2g",   "Intent: Initial Java heap size at VM boot (equivalent to -Xms)" },
  { "initial_heap_size",        "InitialHeapSize",         "size_t", "2g",   "Intent: Initial Java heap size at VM boot (equivalent to -Xms)" },
  { "xms",                      "InitialHeapSize",         "size_t", "2g",   "Intent: Initial Java heap size at VM boot (equivalent to -Xms)" },
  { "min_heap",                 "MinHeapSize",             "size_t", "512m", "Intent: Minimum permissible Java heap footprint" },
  { "min_heap_size",            "MinHeapSize",             "size_t", "512m", "Intent: Minimum permissible Java heap footprint" },
  { "soft_max_heap",            "SoftMaxHeapSize",         "size_t", "3g",   "Intent: Manageable soft maximum heap target cap for GC ergonomics" },
  { "soft_heap_size",           "SoftMaxHeapSize",         "size_t", "3g",   "Intent: Manageable soft maximum heap target cap for GC ergonomics" },
  { "young_size",               "NewSize",                 "size_t", "1g",   "Intent: Initial young generation memory pool size (equivalent to -Xmn)" },
  { "young_gen_size",           "NewSize",                 "size_t", "1g",   "Intent: Initial young generation memory pool size (equivalent to -Xmn)" },
  { "new_size",                 "NewSize",                 "size_t", "1g",   "Intent: Initial young generation memory pool size (equivalent to -Xmn)" },
  { "xmn",                      "NewSize",                 "size_t", "1g",   "Intent: Initial young generation memory pool size (equivalent to -Xmn)" },
  { "max_young_size",           "MaxNewSize",              "size_t", "2g",   "Intent: Maximum young generation memory pool ceiling" },
  { "max_young_gen_size",       "MaxNewSize",              "size_t", "2g",   "Intent: Maximum young generation memory pool ceiling" },
  { "max_new_size",             "MaxNewSize",              "size_t", "2g",   "Intent: Maximum young generation memory pool ceiling" },
  { "old_size",                 "OldSize",                 "size_t", "2g",   "Intent: Initial old/tenured generation memory pool capacity" },
  { "old_gen_size",             "OldSize",                 "size_t", "2g",   "Intent: Initial old/tenured generation memory pool capacity" },
  { "metaspace_size",           "MetaspaceSize",           "size_t", "256m", "Intent: Initial metadata and class footprint space (replaces PermGen)" },
  { "metaspacesize",            "MetaspaceSize",           "size_t", "256m", "Intent: Initial metadata and class footprint space (replaces PermGen)" },
  { "permgen",                  "MetaspaceSize",           "size_t", "256m", "Intent: Initial metadata and class footprint space (replaces PermGen)" },
  { "perm_gen",                 "MetaspaceSize",           "size_t", "256m", "Intent: Initial metadata and class footprint space (replaces PermGen)" },
  { "max_metaspace_size",       "MaxMetaspaceSize",        "size_t", "512m", "Intent: Maximum permissible metadata and class footprint ceiling" },
  { "maxmetaspacesize",         "MaxMetaspaceSize",        "size_t", "512m", "Intent: Maximum permissible metadata and class footprint ceiling" },
  { "max_permgen",              "MaxMetaspaceSize",        "size_t", "512m", "Intent: Maximum permissible metadata and class footprint ceiling" },
  { "compressed_class_size",    "CompressedClassSpaceSize","size_t", "1g",   "Intent: Maximum virtual address space for compressed class pointers" },
  { "large_page_size",          "LargePageSizeInBytes",    "size_t", "2m",   "Intent: Physical page size in bytes for hardware large pages" },
  { "huge_page_size",           "LargePageSizeInBytes",    "size_t", "2m",   "Intent: Physical page size in bytes for hardware large pages" },
  { "tlab_size",                "TLABSize",                "size_t", "512k", "Intent: Starting thread-local allocation buffer initial capacity" },
  { "min_tlab_size",            "MinTLABSize",             "size_t", "2k",   "Intent: Minimum thread-local allocation buffer size in bytes" },
  { "thread_stack_size",        "ThreadStackSize",         "size_t", "1024k","Intent: Default OS stack size per Java application thread (equivalent to -Xss)" },
  { "stack_size",               "ThreadStackSize",         "size_t", "1024k","Intent: Default OS stack size per Java application thread (equivalent to -Xss)" },
  { "thread_stack",             "ThreadStackSize",         "size_t", "1024k","Intent: Default OS stack size per Java application thread (equivalent to -Xss)" },
  { "xss",                      "ThreadStackSize",         "size_t", "1024k","Intent: Default OS stack size per Java application thread (equivalent to -Xss)" },
  { "compiler_stack_size",      "CompilerThreadStackSize", "size_t", "2048k","Intent: Stack size for background JIT compiler worker threads" },
  { "vm_thread_stack_size",     "VMThreadStackSize",       "size_t", "1024k","Intent: Stack size for internal non-Java VM worker threads" },
  { "code_cache_size",          "ReservedCodeCacheSize",   "size_t", "240m", "Intent: Maximum reserved memory buffer for JIT-compiled native code" },
  { "reserved_code_cache_size", "ReservedCodeCacheSize",   "size_t", "240m", "Intent: Maximum reserved memory buffer for JIT-compiled native code" },
  { "code_cache",               "ReservedCodeCacheSize",   "size_t", "240m", "Intent: Maximum reserved memory buffer for JIT-compiled native code" },
  { "initial_code_cache_size",  "InitialCodeCacheSize",    "size_t", "160m", "Intent: Initial allocated memory for JIT-compiled native code" },
  { "max_direct_memory",        "MaxDirectMemorySize",     "size_t", "4g",   "Intent: Maximum allowable off-heap direct NIO ByteBuffer memory" },
  { "direct_memory_size",       "MaxDirectMemorySize",     "size_t", "4g",   "Intent: Maximum allowable off-heap direct NIO ByteBuffer memory" },
  { "async_log_buffer_size",    "AsyncLogBufferSize",      "size_t", "2m",   "Intent: Memory buffer capacity for unified asynchronous logging" },
  { "redlet_runway",            "RedletRunwayHandselBytes","size_t", "1g",   "Intent: Pre-reserved contiguous runway virtual memory handsel bytes" },
  { "runway_size",              "RedletRunwayHandselBytes","size_t", "1g",   "Intent: Pre-reserved contiguous runway virtual memory handsel bytes" },
  { "redlet_landfill",          "RedletLandfillHandselBytes","size_t","285m","Intent: Auxiliary landfill staging memory handsel bytes" },
  { "landfill_size",            "RedletLandfillHandselBytes","size_t","285m","Intent: Auxiliary landfill staging memory handsel bytes" },
  { "pressure_threshold",       "RedletPressureThresholdBytes","size_t","256m","Intent: Heap pressure threshold triggering proactive reclamation" },
  { "slab_chunk_size",          "RedletSlabChunkSizeBytes","size_t", "2m",   "Intent: Allocation granularity slab chunk size (default 2MB)" },
  { "redlet_min_expand",        "RedletMinHeapExpand",     "size_t", "128m", "Intent: Minimum expansion increment for Redlet heap growth" },
  { "redlet_max_tlab",          "RedletMaxTLABSize",       "size_t", "4m",   "Intent: Maximum TLAB size cap allowed in RedletGC" },

  // =========================================================================
  // GROUP 3: Counts, Ratios, Limits & Intervals (uintx / intx / uint / int)
  // =========================================================================
  { "gc_threads",               "ParallelGCThreads",       "uint",   "8",    "Intent: Number of parallel GC evaluation worker threads" },
  { "gcthreads",                "ParallelGCThreads",       "uint",   "8",    "Intent: Number of parallel GC evaluation worker threads" },
  { "parallel_gc_threads",      "ParallelGCThreads",       "uint",   "8",    "Intent: Number of parallel GC evaluation worker threads" },
  { "worker_threads",           "ParallelGCThreads",       "uint",   "8",    "Intent: Number of parallel GC evaluation worker threads" },
  { "conc_threads",             "ConcGCThreads",           "uint",   "4",    "Intent: Number of concurrent GC marking/sweeping threads" },
  { "concthreads",              "ConcGCThreads",           "uint",   "4",    "Intent: Number of concurrent GC marking/sweeping threads" },
  { "concurrent_gc_threads",    "ConcGCThreads",           "uint",   "4",    "Intent: Number of concurrent GC marking/sweeping threads" },
  { "pause_time",               "MaxGCPauseMillis",        "uintx",  "200",  "Intent: Target maximum GC pause time in milliseconds" },
  { "max_pause",                "MaxGCPauseMillis",        "uintx",  "200",  "Intent: Target maximum GC pause time in milliseconds" },
  { "pause_target",             "MaxGCPauseMillis",        "uintx",  "200",  "Intent: Target maximum GC pause time in milliseconds" },
  { "max_gc_pause",             "MaxGCPauseMillis",        "uintx",  "200",  "Intent: Target maximum GC pause time in milliseconds" },
  { "pause_millis",             "MaxGCPauseMillis",        "uintx",  "200",  "Intent: Target maximum GC pause time in milliseconds" },
  { "pause_interval",           "GCPauseIntervalMillis",   "uintx",  "201",  "Intent: Target GC pause interval MMU goal in milliseconds" },
  { "gc_interval",              "GCPauseIntervalMillis",   "uintx",  "201",  "Intent: Target GC pause interval MMU goal in milliseconds" },
  { "ihop",                     "InitiatingHeapOccupancyPercent","uint","45", "Intent: Old gen occupancy percentage triggering concurrent cycle" },
  { "initiating_heap_occupancy_percent","InitiatingHeapOccupancyPercent","uint","45","Intent: Old gen occupancy percentage triggering concurrent cycle" },
  { "g1_ihop",                  "InitiatingHeapOccupancyPercent","uint","45","Intent: G1 initiating heap occupancy percent (IHOP)" },
  { "reserve_percent",          "G1ReservePercent",        "uintx",  "10",   "Intent: Headroom percentage reserved against promotion failures" },
  { "g1_reserve_percent",       "G1ReservePercent",        "uintx",  "10",   "Intent: Headroom percentage reserved against promotion failures" },
  { "survivor_ratio",           "SurvivorRatio",           "uintx",  "8",    "Intent: Ratio of eden to each survivor space (e.g. 8 means 8:1:1)" },
  { "survivorratio",            "SurvivorRatio",           "uintx",  "8",    "Intent: Ratio of eden to each survivor space (e.g. 8 means 8:1:1)" },
  { "target_survivor_ratio",    "TargetSurvivorRatio",     "uintx",  "50",   "Intent: Desired survivor space occupancy percent after scavenging" },
  { "new_ratio",                "NewRatio",                "uintx",  "2",    "Intent: Ratio of old gen to young gen capacity (e.g. 2 means 2:1)" },
  { "newratio",                 "NewRatio",                "uintx",  "2",    "Intent: Ratio of old gen to young gen capacity (e.g. 2 means 2:1)" },
  { "tenuring_threshold",       "MaxTenuringThreshold",    "uintx",  "15",   "Intent: Maximum object age threshold before promotion to old gen" },
  { "max_tenuring_threshold",   "MaxTenuringThreshold",    "uintx",  "15",   "Intent: Maximum object age threshold before promotion to old gen" },
  { "tenuring",                 "MaxTenuringThreshold",    "uintx",  "15",   "Intent: Maximum object age threshold before promotion to old gen" },
  { "initial_tenuring_threshold","InitialTenuringThreshold","uintx", "7",    "Intent: Initial starting object age threshold for promotion" },
  { "compile_threshold",        "CompileThreshold",        "intx",   "10000","Intent: Invocations before method is compiled by JIT" },
  { "jit_threshold",            "CompileThreshold",        "intx",   "10000","Intent: Invocations before method is compiled by JIT" },
  { "compiler_count",           "CICompilerCount",         "int",    "4",    "Intent: Number of background parallel compiler worker threads" },
  { "compiler_threads",         "CICompilerCount",         "int",    "4",    "Intent: Number of background parallel compiler worker threads" },
  { "jit_threads",              "CICompilerCount",         "int",    "4",    "Intent: Number of background parallel compiler worker threads" },
  { "stop_at_level",            "TieredStopAtLevel",       "int",    "1",    "Intent: Cap compilation tier (1=C1 simple, 2=C1 limited, 3=C1 full, 4=C2)" },
  { "c1_only",                  "TieredStopAtLevel",       "int",    "1",    "Intent: Restrict JIT compilation to Tier 1 Client Compiler only" },
  { "max_inline_level",         "MaxInlineLevel",          "intx",   "9",    "Intent: Maximum call tree recursion depth for method inlining" },
  { "inline_depth",             "MaxInlineLevel",          "intx",   "9",    "Intent: Maximum call tree recursion depth for method inlining" },
  { "max_inline_size",          "MaxInlineSize",           "intx",   "35",   "Intent: Maximum bytecode size of a method considered for inlining" },
  { "inline_size",              "MaxInlineSize",           "intx",   "35",   "Intent: Maximum bytecode size of a method considered for inlining" },
  { "freq_inline_size",         "FreqInlineSize",          "intx",   "325",  "Intent: Maximum bytecode size of a frequent hot method to inline" },
  { "hot_inline_size",          "FreqInlineSize",          "intx",   "325",  "Intent: Maximum bytecode size of a frequent hot method to inline" },
  { "loop_unroll_limit",        "LoopUnrollLimit",         "intx",   "60",   "Intent: Maximum instruction factor for unrolling loop iterations in C2" },
  { "unroll_limit",             "LoopUnrollLimit",         "intx",   "60",   "Intent: Maximum instruction factor for unrolling loop iterations in C2" },
  { "max_vector_size",          "MaxVectorSize",           "intx",   "64",   "Intent: Maximum SIMD hardware vector register width in bytes" },
  { "vector_size",              "MaxVectorSize",           "intx",   "64",   "Intent: Maximum SIMD hardware vector register width in bytes" },
  { "vector_width",             "MaxVectorSize",           "intx",   "64",   "Intent: Maximum SIMD hardware vector register width in bytes" },
  { "cores",                    "ActiveProcessorCount",    "int",    "16",   "Intent: Override host CPU core count detected by the JVM runtime" },
  { "num_cores",                "ActiveProcessorCount",    "int",    "16",   "Intent: Override host CPU core count detected by the JVM runtime" },
  { "cpu_count",                "ActiveProcessorCount",    "int",    "16",   "Intent: Override host CPU core count detected by the JVM runtime" },
  { "active_processors",        "ActiveProcessorCount",    "int",    "16",   "Intent: Override host CPU core count detected by the JVM runtime" },
  { "soft_ref_policy",          "SoftRefLRUPolicyMSPerMB", "intx",   "1000", "Intent: Lifetime of cleared SoftReferences per MB of free heap" },
  { "string_dedup_age",         "StringDeduplicationAgeThreshold","uintx","3","Intent: Minimum object age before candidate string deduplication" },
  { "periodic_gc_interval",     "G1PeriodicGCInterval",    "uintx",  "0",    "Intent: Periodic GC invocation interval in ms for idle heaps" },
  { "safepoint_timeout_delay",  "SafepointTimeoutDelay",   "intx",   "10000","Intent: Delay in ms before reporting safepoint timeout warnings" },
  { "autobox_cache_max",        "AutoBoxCacheMax",         "intx",   "1024", "Intent: High end of cache size for boxing autoboxed Integers" },
  { "locking_mode",             "LockingMode",             "int",    "1",    "Intent: Synchronization locking mode (1=lightweight, 2=heavy monitors)" },
  { "wakes_interval_ns",        "RedletWakesGCIntervalNs", "uint64_t","1000000","Intent: Proactive background GC wake-up interval in nanoseconds" },
  { "line_compaction_freq_ns",  "RedletLineCompactionFreqNs","uint64_t","350000000","Intent: Background line compaction interval in nanoseconds" },

  // =========================================================================
  // GROUP 4: Percentages, Floats & Threshold Ratios (double)
  // =========================================================================
  { "max_ram_percentage",       "MaxRAMPercentage",        "double", "80.0", "Intent: Maximum heap size as percentage of total RAM (container-aware)" },
  { "maxrampercentage",         "MaxRAMPercentage",        "double", "80.0", "Intent: Maximum heap size as percentage of total RAM (container-aware)" },
  { "maxrampercent",            "MaxRAMPercentage",        "double", "80.0", "Intent: Maximum heap size as percentage of total RAM (container-aware)" },
  { "max_ram_percent",          "MaxRAMPercentage",        "double", "80.0", "Intent: Maximum heap size as percentage of total RAM (container-aware)" },
  { "ram_percentage",           "MaxRAMPercentage",        "double", "80.0", "Intent: Maximum heap size as percentage of total RAM (container-aware)" },
  { "min_ram_percentage",       "MinRAMPercentage",        "double", "50.0", "Intent: Minimum heap size as percentage of total RAM (small heaps)" },
  { "minrampercentage",         "MinRAMPercentage",        "double", "50.0", "Intent: Minimum heap size as percentage of total RAM (small heaps)" },
  { "initial_ram_percentage",   "InitialRAMPercentage",    "double", "25.0", "Intent: Initial heap size as percentage of total RAM at boot" },
  { "initialrampercentage",     "InitialRAMPercentage",    "double", "25.0", "Intent: Initial heap size as percentage of total RAM at boot" },
  { "periodic_gc_system_load",  "G1PeriodicGCSystemLoadThreshold","double","0.0","Intent: System 1-minute load average threshold for periodic GC" },
  { "falline_ratio",            "RedletFallineCompactRatio","double","0.30", "Intent: Falline compaction threshold ratio (0.05 to 0.95)" },
  { "falline_compact_ratio",    "RedletFallineCompactRatio","double","0.30", "Intent: Falline compaction threshold ratio (0.05 to 0.95)" },

  // =========================================================================
  // GROUP 5: Paths, Commands, Directives & Options (ccstr / ccstrlist)
  // =========================================================================
  { "heap_dump_path",           "HeapDumpPath",            "ccstr",  "/path/dump.hprof", "Intent: Target filesystem directory or file path for HPROF dumps" },
  { "dumppath",                 "HeapDumpPath",            "ccstr",  "/path/dump.hprof", "Intent: Target filesystem directory or file path for HPROF dumps" },
  { "hprof_path",               "HeapDumpPath",            "ccstr",  "/path/dump.hprof", "Intent: Target filesystem directory or file path for HPROF dumps" },
  { "dump_dir",                 "HeapDumpPath",            "ccstr",  "/var/log/java",    "Intent: Target filesystem directory for HPROF dumps" },
  { "hs_err",                   "ErrorFile",               "ccstr",  "/path/hs_err.log", "Intent: Target filesystem path for HotSpot crash dump logs" },
  { "error_file",               "ErrorFile",               "ccstr",  "/path/hs_err.log", "Intent: Target filesystem path for HotSpot crash dump logs" },
  { "crash_file",               "ErrorFile",               "ccstr",  "/path/hs_err.log", "Intent: Target filesystem path for HotSpot crash dump logs" },
  { "crash_log",                "ErrorFile",               "ccstr",  "/path/hs_err.log", "Intent: Target filesystem path for HotSpot crash dump logs" },
  { "on_error",                 "OnError",                 "ccstr",  "gdb %p",           "Intent: Execute user shell command or script upon VM fatal crash" },
  { "on_crash",                 "OnError",                 "ccstr",  "gdb %p",           "Intent: Execute user shell command or script upon VM fatal crash" },
  { "onerror",                  "OnError",                 "ccstr",  "gdb %p",           "Intent: Execute user shell command or script upon VM fatal crash" },
  { "on_oom",                   "OnOutOfMemoryError",      "ccstr",  "kill -9 %p",       "Intent: Execute user shell command or script upon OutOfMemoryError" },
  { "oom_command",              "OnOutOfMemoryError",      "ccstr",  "kill -9 %p",       "Intent: Execute user shell command or script upon OutOfMemoryError" },
  { "start_flight_recording",   "StartFlightRecording",    "ccstr",  "dumponexit=true",  "Intent: Start continuous JFR diagnostic profile recording at boot" },
  { "start_jfr",                "StartFlightRecording",    "ccstr",  "dumponexit=true",  "Intent: Start continuous JFR diagnostic profile recording at boot" },
  { "flight_recorder_options",  "FlightRecorderOptions",   "ccstr",  "stackdepth=128",   "Intent: Configuration options for Java Flight Recorder" },
  { "jfr_options",              "FlightRecorderOptions",   "ccstr",  "stackdepth=128",   "Intent: Configuration options for Java Flight Recorder" },
  { "compile_only",             "CompileOnly",             "ccstr",  "pkg/Class.method", "Intent: Restrict compilation to matching method signature pattern" },
  { "compile_command",          "CompileCommand",          "ccstrlist","dontinline,Foo.bar","Intent: Prepend JIT compilation directive commands" },
  { "redlet_options",           "RedletOptions",           "ccstr",  "turbo,trim",       "Intent: RedletGC direct comma-separated intent sequence" },
  { "turbo",                    "RedletOptions",           "ccstr",  "turbo",            "Intent: Engage hardware performance governor boost during GC" },
  { "boost",                    "RedletOptions",           "ccstr",  "boost",            "Intent: Engage hardware performance governor boost during GC" },
  { "upclock",                  "RedletOptions",           "ccstr",  "upclock",          "Intent: Engage host CPU upclocking during intensive sweeps" },
  { "rmrp_method",              "RedletRMRPMethod",        "ccstr",  "Plump",            "Intent: RMRP memory reclamation method: Plump or Sequential" },
  { "redlet_simd",              "RedletSIMD",              "ccstr",  "avx10_2",          "Intent: Vector engine: auto, avx10_2, avx512, sve2, rvv" },
  { "gpu_offload",              "RedletGPUOffload",        "ccstr",  "levelzero",        "Intent: GPU accelerator backend: none, levelzero, hip" }
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
  const HotSpotIntentEntry* best_intent_entry = nullptr;
  const char* best_typo_hint = nullptr;

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
            if (entry.expected_value != nullptr && strcmp(entry.expected_value, "true") != 0) {
              jio_snprintf(_formatted_hint_buffer, sizeof(_formatted_hint_buffer),
                           "[%s, e.g. %s] %s",
                           entry.flag_type ? entry.flag_type : target->type_string(),
                           entry.expected_value, entry.technical_doc);
            } else {
              jio_snprintf(_formatted_hint_buffer, sizeof(_formatted_hint_buffer),
                           "[%s] %s",
                           entry.flag_type ? entry.flag_type : target->type_string(),
                           entry.technical_doc);
            }
            *out_hint = _formatted_hint_buffer;
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
          best_intent_entry = &entry;
          best_typo_hint = nullptr;
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
        jio_snprintf(_formatted_hint_buffer, sizeof(_formatted_hint_buffer),
                     "[%s] Case or separator mismatch", current->type_string());
        *out_hint = _formatted_hint_buffer;
      }
      return current; // Immediate match!
    }

    // 2b. Affix match (e.g. user typed "LargePages" for "UseLargePages")
    if (target_len == query_len + 3 && strncmp(clean_target, "use", 3) == 0 && strcmp(clean_target + 3, clean_query) == 0) {
      if (0.97 > best_score) {
        best_score = 0.97;
        best_flag = current;
        best_intent_entry = nullptr;
        best_typo_hint = "Affix match ('Use' prefix omitted)";
      }
      continue;
    }
    if (query_len == target_len + 3 && strncmp(clean_query, "use", 3) == 0 && strcmp(clean_query + 3, clean_target) == 0) {
      if (0.97 > best_score) {
        best_score = 0.97;
        best_flag = current;
        best_intent_entry = nullptr;
        best_typo_hint = "Affix match ('Use' prefix added)";
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
      best_intent_entry = nullptr;
      best_typo_hint = flag_hint;
    }
  }

  // 3. Final Threshold Validation (HotSpot standard similarity threshold is 0.70)
  double threshold = 0.70;
  if (best_flag != nullptr && best_score >= threshold) {
    if (out_hint != nullptr) {
      if (best_intent_entry != nullptr) {
        if (best_intent_entry->expected_value != nullptr && strcmp(best_intent_entry->expected_value, "true") != 0) {
          jio_snprintf(_formatted_hint_buffer, sizeof(_formatted_hint_buffer),
                       "[%s, e.g. %s] %s",
                       best_intent_entry->flag_type ? best_intent_entry->flag_type : best_flag->type_string(),
                       best_intent_entry->expected_value, best_intent_entry->technical_doc);
        } else {
          jio_snprintf(_formatted_hint_buffer, sizeof(_formatted_hint_buffer),
                       "[%s] %s",
                       best_intent_entry->flag_type ? best_intent_entry->flag_type : best_flag->type_string(),
                       best_intent_entry->technical_doc);
        }
      } else if (best_typo_hint != nullptr) {
        jio_snprintf(_formatted_hint_buffer, sizeof(_formatted_hint_buffer),
                     "[%s] %s", best_flag->type_string(), best_typo_hint);
      }
      *out_hint = _formatted_hint_buffer;
    }
    return best_flag;
  }

  return nullptr;
}
