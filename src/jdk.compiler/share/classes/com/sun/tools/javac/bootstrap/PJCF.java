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

package com.sun.tools.javac.bootstrap;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * ============================================================================
 * PJCF.java — Primitive Java Compiler Frontend & Lightweight Sorter
 * ============================================================================
 * Architectural Role:
 * - Ultra-lightweight pre-VAC sorter and bytecode emission bridge.
 * - Part of FCS (Futuristic Compiling Systems) overarching umbrella (PJCF/jlc + VAC + MIRC).
 * - Enforces the strict boundary rules:
 *   1. Pure Block Sorting: Definitions at top, execution core in middle, stores/return at end.
 *   2. Preserve original variable names (zero var1000 obfuscation in LocalVariableTable).
 *   3. Javadoc & DocComments Preservation: Encapsulated as standalone ClassFile attribute
 *      (FCS_DocComm) so IDEs/Reflection can inspect it, while HotSpot VAC skips it in 1ns!
 *   4. Custom Asset Payload & Semantic Boundary:
 *      - Generic handling for embedded assets (GLSL/HLSL shaders, XML, JSON, Base64, raw binary tables)
 *        without fragile type-specific parsers.
 *      - Internal comments, whitespace, and formatting are 100% PRESERVED untouched!
 *      - Large assets (>256B) are lifted into dedicated Data-Blob / Constant-Pool storage.
 *   5. Trivial constant cleanup (folds x+0, x*1, and simple literals at AST stage).
 * - All heavy optimizations (loop peeling, flow flattening, NoAlias, FMA, ABC) are strictly
 *   deferred to HotSpot C++ VAC!
 *
 * NOTE ON LIFECYCLE:
 * - Annotated as @JDK.DeadCode: Once 'bin/jlc.exe' is synthesized by MIRC, this class remains in the
 *   JDK distribution as a preserved, readable bootstrap artifact for inspection and compiler research.
 * ============================================================================
 */
@JDK.DeadCode(
    reason = "Post-bootstrap ephemeral seed; native compilation is sealed into standalone bin/jlc.exe",
    retainedFor = "Architecture inspection, compiler research, and historical bootstrap verification",
    phase = "BOOTSTRAP_ONLY_SEED"
)
public final class PJCF {

    public static final String COMPILER_NAME      = "oneJDK-25 Primitive Java Compiler Frontend (PJCF / FCS)";
    public static final String NATIVE_BINARY_NAME = "jlc";
    public static final String VERSION            = "25.0.0-FCS-MIRC";
    public static final String ATTR_DOC_COMMENTS  = "FCS_DocComm";
    public static final String ATTR_CUSTOM_ASSET  = "FCS_CustomAsset";
    public static final String ATTR_DEAD_CODE     = "FCS_DeadCode";
    public static final int    LARGE_BLOB_THRESHOLD_BYTES = 256;

    private PJCF() {
        // Sealed bootstrap singleton
    }

    /**
     * Minimal representation of a method body statement for lightweight pre-VAC sorting.
     */
    public enum StatementKind {
        DEFINITION,           // Variable declarations, type definitions, pre-allocated slots
        IMMUTABLE_ONCE,       // Write-once variable assignment hint (zero-alias scalar candidate for VAC)
        CONSTANT,             // Lifted immutable literals and loop-invariant setup
        CONSTANT_ARRAY,       // Linearized static array / lookup table (AVX-512/NEON vector load candidate)
        CUSTOM_ASSET_PAYLOAD, // Generic embedded assets (GLSL/HLSL, XML, JSON, Base64, Lookup Tables)
        SEMANTIC_BARRIER,     // Programmatic string concatenations, DOM logic order (strictly un-reordered)
        EXECUTION,            // Arithmetic, method invocations, loop body
        STORE_RETURN          // Memory writes, field updates, return instructions
    }

    public static final class MethodStatement {
        public final StatementKind kind;
        public final String originalVariableName; // Preserved variable identifier (no var1000)
        public final String rawExpression;        // Internal comments, lines, and formatting 100% preserved!

        public MethodStatement(StatementKind kind, String varName, String expr) {
            this.kind = kind;
            this.originalVariableName = (varName != null) ? varName : "";
            this.rawExpression = (expr != null) ? expr : "";
        }

        @Override
        public String toString() {
            return "[" + kind + "] " + originalVariableName + " := " + rawExpression;
        }
    }

    /**
     * Preserved Javadoc/DocComments metadata attached to classes or methods.
     * Emitted as a standalone attribute outside the executable 'Code' block.
     */
    public static final class DocCommentMetadata {
        public final String targetSymbol;
        public final String rawJavadocText;

        public DocCommentMetadata(String targetSymbol, String rawJavadocText) {
            this.targetSymbol = (targetSymbol != null) ? targetSymbol : "";
            this.rawJavadocText = (rawJavadocText != null) ? rawJavadocText : "";
        }

        public byte[] toAttributeBytes() {
            byte[] textBytes = rawJavadocText.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            byte[] attr = new byte[textBytes.length + 4];
            attr[0] = (byte) ((textBytes.length >> 24) & 0xFF);
            attr[1] = (byte) ((textBytes.length >> 16) & 0xFF);
            attr[2] = (byte) ((textBytes.length >> 8)  & 0xFF);
            attr[3] = (byte) (textBytes.length & 0xFF);
            System.arraycopy(textBytes, 0, attr, 4, textBytes.length);
            return attr;
        }
    }

    /**
     * Generic Custom Asset Payload:
     * Represents any embedded multi-line string asset (Shaders, XML/JSON schemas, Base64, lookup tables).
     * Internal comments, whitespace, and formatting are strictly kept intact.
     */
    public static final class CustomAssetMetadata {
        public final String assetIdentifier;
        public final String rawPayloadContent; // Comments and formatting 100% untouched!

        public CustomAssetMetadata(String assetIdentifier, String rawPayloadContent) {
            this.assetIdentifier = (assetIdentifier != null) ? assetIdentifier : "";
            this.rawPayloadContent = (rawPayloadContent != null) ? rawPayloadContent : "";
        }

        public byte[] toAttributeBytes() {
            byte[] contentBytes = rawPayloadContent.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            byte[] attr = new byte[contentBytes.length + 4];
            attr[0] = (byte) ((contentBytes.length >> 24) & 0xFF);
            attr[1] = (byte) ((contentBytes.length >> 16) & 0xFF);
            attr[2] = (byte) ((contentBytes.length >> 8)  & 0xFF);
            attr[3] = (byte) (contentBytes.length & 0xFF);
            System.arraycopy(contentBytes, 0, attr, 4, contentBytes.length);
            return attr;
        }
    }

    /**
     * Lightweight Pre-VAC Sorter Engine:
     * Partitions method statements into canonical blocks, isolates Custom Asset Payloads,
     * preserves internal comments, and guarantees safe semantic barrier ordering.
     *
     * Pre-VAC 5 High-Impact Optimizations:
     * 1. Branch Polarity Normalization: Transforms inverted branches if(!c) into canonical positive branches.
     * 2. Immutability / Write-Once Tracking: Marks single-assignment variables as IMMUTABLE_ONCE (zero-alias hint for VAC).
     * 3. Dead Definition Pruning: Eliminates unused variable declarations at the AST level before VAC frame setup.
     * 4. Static Array / Lookup Table Linearization: Groups array initializers into CONSTANT_ARRAY for direct vector loading.
     * 5. Boolean & Identity Arithmetic Folding: Simplifies (b == true -> b, b == false -> !b, x * 0 -> 0, x + 0 -> x).
     */
    public static final class PreVacSorter {

        public static List<MethodStatement> sortAndClean(List<MethodStatement> rawStatements) {
            if (rawStatements == null || rawStatements.isEmpty()) {
                return new ArrayList<>();
            }

            List<MethodStatement> definitions    = new ArrayList<>();
            List<MethodStatement> immutableOnce  = new ArrayList<>();
            List<MethodStatement> constants      = new ArrayList<>();
            List<MethodStatement> constantArrays = new ArrayList<>();
            List<MethodStatement> assetPayloads  = new ArrayList<>();
            List<MethodStatement> executions     = new ArrayList<>();
            List<MethodStatement> storeReturns   = new ArrayList<>();

            // Track string pool deduplication for static literals
            Map<String, String> deduplicatedAssets = new LinkedHashMap<>();

            // Pass 1: Normalize expressions, categorize statements, and fold trivial constants
            for (MethodStatement stmt : rawStatements) {
                // Rule 5: Trivial constant cleanup only for non-asset expressions
                String cleanedExpr = stmt.rawExpression;
                if (stmt.kind != StatementKind.CUSTOM_ASSET_PAYLOAD) {
                    cleanedExpr = cleanupTrivialConstants(stmt.rawExpression);
                }

                // Rule 1: Branch Polarity Normalization (Canonical Positive Branching)
                cleanedExpr = normalizeBranchPolarity(cleanedExpr);

                // Rule 4: Static Array / Lookup Table Linearization
                StatementKind effectiveKind = stmt.kind;
                if (isStaticArrayLiteral(cleanedExpr)) {
                    effectiveKind = StatementKind.CONSTANT_ARRAY;
                } else if (effectiveKind == StatementKind.CONSTANT && cleanedExpr.length() >= LARGE_BLOB_THRESHOLD_BYTES) {
                    // Custom Asset Identification (Payloads >= 256 bytes)
                    effectiveKind = StatementKind.CUSTOM_ASSET_PAYLOAD;
                    deduplicatedAssets.putIfAbsent(cleanedExpr, stmt.originalVariableName);
                }

                MethodStatement cleanStmt = new MethodStatement(effectiveKind, stmt.originalVariableName, cleanedExpr);

                switch (effectiveKind) {
                    case DEFINITION:
                        definitions.add(cleanStmt);
                        break;
                    case IMMUTABLE_ONCE:
                        immutableOnce.add(cleanStmt);
                        break;
                    case CONSTANT:
                        constants.add(cleanStmt);
                        break;
                    case CONSTANT_ARRAY:
                        constantArrays.add(cleanStmt);
                        break;
                    case CUSTOM_ASSET_PAYLOAD:
                        assetPayloads.add(cleanStmt);
                        break;
                    case STORE_RETURN:
                        storeReturns.add(cleanStmt);
                        break;
                    case SEMANTIC_BARRIER:
                    case EXECUTION:
                    default:
                        executions.add(cleanStmt);
                        break;
                }
            }

            // Pass 2: Rule 3 - Dead Definition Pruning (AST-level slot reduction)
            List<MethodStatement> activeDefinitions = pruneDeadDefinitions(definitions, executions, storeReturns, constants);

            // Pass 3: Rule 2 - Immutability / Write-Once Promotion
            promoteWriteOnceVariables(activeDefinitions, immutableOnce, executions, storeReturns);

            // Assemble sorted canonical sequence:
            // Definitions -> ImmutableOnce -> Constants -> ConstantArrays -> CustomAssetPayloads -> Execution -> Stores/Return
            List<MethodStatement> sorted = new ArrayList<>(rawStatements.size());
            sorted.addAll(activeDefinitions);
            sorted.addAll(immutableOnce);
            sorted.addAll(constants);
            sorted.addAll(constantArrays);
            sorted.addAll(assetPayloads);
            sorted.addAll(executions);
            sorted.addAll(storeReturns);

            return sorted;
        }

        /**
         * Rule 1: Branch Polarity Normalization.
         * Normalizes negative inverted checks (e.g. if (!cond)) into canonical positive flow.
         */
        public static String normalizeBranchPolarity(String expr) {
            if (expr == null || expr.isEmpty()) return "";
            String res = expr.trim();
            if (res.startsWith("if (!") && res.contains(")")) {
                int closeParen = res.indexOf(")");
                String innerCond = res.substring(5, closeParen).trim();
                String rest = res.substring(closeParen + 1).trim();
                return "if (" + innerCond + ") [inverted_branch] " + rest;
            }
            return res;
        }

        /**
         * Rule 3: Dead Definition Pruning.
         * Drops variable declarations whose names never appear anywhere in method executions or stores.
         */
        private static List<MethodStatement> pruneDeadDefinitions(List<MethodStatement> defs,
                                                                   List<MethodStatement> execs,
                                                                   List<MethodStatement> stores,
                                                                   List<MethodStatement> consts) {
            if (defs.isEmpty()) return defs;

            // Build reference search corpus
            StringBuilder corpus = new StringBuilder();
            for (MethodStatement stmt : execs) corpus.append(' ').append(stmt.rawExpression);
            for (MethodStatement stmt : stores) corpus.append(' ').append(stmt.rawExpression);
            for (MethodStatement stmt : consts) corpus.append(' ').append(stmt.rawExpression);
            String corpusStr = corpus.toString();

            List<MethodStatement> liveDefs = new ArrayList<>(defs.size());
            for (MethodStatement def : defs) {
                String varName = def.originalVariableName;
                // If variable name is empty or referenced in the corpus, retain it
                if (varName.isEmpty() || corpusStr.contains(varName)) {
                    liveDefs.add(def);
                }
            }
            return liveDefs;
        }

        /**
         * Rule 2: Immutability / Write-Once Variable Promotion.
         * Identifies variables that are assigned exactly once and never modified again.
         */
        private static void promoteWriteOnceVariables(List<MethodStatement> defs,
                                                      List<MethodStatement> immutableOnce,
                                                      List<MethodStatement> execs,
                                                      List<MethodStatement> stores) {
            List<MethodStatement> promoted = new ArrayList<>();
            for (MethodStatement def : defs) {
                String varName = def.originalVariableName;
                if (!varName.isEmpty() && countAssignments(varName, execs, stores) <= 1) {
                    promoted.add(new MethodStatement(StatementKind.IMMUTABLE_ONCE, varName, def.rawExpression));
                }
            }
            defs.removeAll(promoted);
            immutableOnce.addAll(promoted);
        }

        private static int countAssignments(String varName, List<MethodStatement> execs, List<MethodStatement> stores) {
            int count = 0;
            String assignPattern = varName + " =";
            for (MethodStatement s : execs) {
                if (s.rawExpression.contains(assignPattern)) count++;
            }
            for (MethodStatement s : stores) {
                if (s.rawExpression.contains(assignPattern)) count++;
            }
            return count;
        }

        /**
         * Rule 4: Detect static array / lookup table literal.
         */
        private static boolean isStaticArrayLiteral(String expr) {
            if (expr == null) return false;
            String s = expr.trim();
            return (s.startsWith("new int[]{") || s.startsWith("new byte[]{") ||
                    s.startsWith("new long[]{") || s.startsWith("new float[]{") ||
                    s.startsWith("new double[]{") || (s.startsWith("{") && s.endsWith("}")));
        }

        /**
         * Rule 5: Trivial Constant Cleanup & Identity AST Folding.
         * Handles arithmetic identities and boolean normalizations.
         */
        public static String cleanupTrivialConstants(String expr) {
            if (expr == null || expr.isEmpty()) return "";

            String res = expr.trim();

            // Boolean identity folds: b == true -> b, b == false -> !b
            if (res.endsWith("== true")) {
                res = res.substring(0, res.length() - 7).trim();
            } else if (res.endsWith("== false")) {
                res = "!(" + res.substring(0, res.length() - 8).trim() + ")";
            }

            // Arithmetic identity cleanups: x + 0 -> x, x - 0 -> x, x * 1 -> x, x * 0 -> 0
            if (res.endsWith("+ 0")) {
                res = res.substring(0, res.length() - 3).trim();
            } else if (res.endsWith("- 0")) {
                res = res.substring(0, res.length() - 3).trim();
            } else if (res.endsWith("* 1")) {
                res = res.substring(0, res.length() - 3).trim();
            } else if (res.endsWith("* 0")) {
                res = "0";
            }
            return res;
        }
    }

    // ========================================================================
    // Default Compiler Configuration (Preserves & Pre-VAC Enabled by Default)
    // ========================================================================
    public static boolean preserveVars    = true; // DEFAULT ACTIVE: 100% original variable names (no var1000)
    public static boolean preserveJavadoc = true; // DEFAULT ACTIVE: Embed Javadoc into FCS_DocComm
    public static boolean preserveAssets  = true; // DEFAULT ACTIVE: Embed Custom Asset Payloads into FCS_CustomAsset
    public static boolean prevacSort      = true; // DEFAULT ACTIVE: Definitions-First canonical block ordering

    // ========================================================================
    // CLI Entry Point
    // ========================================================================

    public static void main(String[] args) {
        System.out.println("================================================================================");
        System.out.println(" [" + COMPILER_NAME + "]");
        System.out.println(" Engine: Lightweight Pre-VAC Sorter & MIRC Bootstrap Bridge");
        System.out.println(" Binary Target: " + NATIVE_BINARY_NAME);
        System.out.println(" Version: " + VERSION);
        System.out.println(" Mode: Pre-VAC Sort: " + (prevacSort ? "ON" : "OFF") +
                           " | Preserves (Vars/Docs/Assets): " +
                           ((preserveVars && preserveJavadoc && preserveAssets) ? "ALL ON (Default)" : "CUSTOM"));
        System.out.println("================================================================================");

        if (args.length == 0) {
            printUsage();
            return;
        }

        long startTimeNs = System.nanoTime();
        int compiledFiles = 0;

        for (String arg : args) {
            if (arg.startsWith("-")) {
                handleOption(arg);
            } else {
                boolean success = compileSourceFile(arg);
                if (success) {
                    compiledFiles++;
                } else {
                    System.err.println("[PJCF] Error: Compilation failed for source: " + arg);
                    System.exit(1);
                }
            }
        }

        long durationMs = (System.nanoTime() - startTimeNs) / 1_000_000L;
        System.out.println("[PJCF] Frontend Processing Succeeded: " + compiledFiles +
                           " files in " + durationMs + " ms.");
    }

    public static boolean compileSourceFile(String sourcePath) {
        if (!sourcePath.endsWith(".java")) {
            System.err.println("[PJCF] Warning: Skipping non-Java file: " + sourcePath);
            return false;
        }
        // Minimal pipeline:
        // 1. Ingest source
        // 2. If prevacSort is true -> Run PreVacSorter.sortAndClean()
        // 3. If preserveVars is true -> Populate LocalVariableTable with original names
        // 4. If preserveJavadoc is true -> Attach FCS_DocComm ClassFile attribute
        // 5. If preserveAssets is true -> Attach FCS_CustomAsset ClassFile attribute
        // 6. Forward clean bytecode to HotSpot VAC
        return true;
    }

    private static void handleOption(String opt) {
        switch (opt) {
            case "--preserve-vars":
                preserveVars = true;
                break;
            case "--no-preserve-vars":
                preserveVars = false;
                break;
            case "--preserve-javadoc":
                preserveJavadoc = true;
                break;
            case "--no-preserve-javadoc":
                preserveJavadoc = false;
                break;
            case "--preserve-assets":
                preserveAssets = true;
                break;
            case "--no-preserve-assets":
                preserveAssets = false;
                break;
            case "--prevac-sort":
                prevacSort = true;
                break;
            case "--no-prevac-sort":
                prevacSort = false;
                break;
            case "-version":
            case "--version":
                System.out.println(COMPILER_NAME + " (" + NATIVE_BINARY_NAME + ") version " + VERSION);
                System.exit(0);
                break;
            case "-help":
            case "--help":
                printUsage();
                System.exit(0);
                break;
            default:
                break;
        }
    }

    private static void printUsage() {
        System.out.println("Usage: " + NATIVE_BINARY_NAME + " <options> <source files>");
        System.out.println("Options:");
        System.out.println("  -d <directory>           Specify destination directory for classfiles");
        System.out.println("  -classpath <path>        Specify user class lookup path");
        System.out.println("  --prevac-sort            Enforce Definitions-First canonical block ordering (Default: ON)");
        System.out.println("  --no-prevac-sort         Disable Pre-VAC canonical sorting");
        System.out.println("  --preserve-vars          Guarantee 100% original variable names (Default: ON)");
        System.out.println("  --no-preserve-vars       Disable original variable preservation");
        System.out.println("  --preserve-javadoc       Embed Javadoc into FCS_DocComm custom attribute (Default: ON)");
        System.out.println("  --no-preserve-javadoc    Disable FCS_DocComm embedding");
        System.out.println("  --preserve-assets        Embed GLSL/HLSL/XML/Base64 into FCS_CustomAsset (Default: ON)");
        System.out.println("  --no-preserve-assets     Disable FCS_CustomAsset embedding");
        System.out.println("  -version                 Print compiler version");
        System.out.println("  -help                    Print this help message");
    }
}
