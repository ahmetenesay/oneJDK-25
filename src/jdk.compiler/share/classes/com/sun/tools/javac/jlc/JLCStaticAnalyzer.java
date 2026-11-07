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

package com.sun.tools.javac.jlc;

import com.sun.tools.javac.jlc.JLCDiagnostics.DiagnosticRecord;
import com.sun.tools.javac.jlc.JLCDiagnostics.QuickFixSuggestion;
import com.sun.tools.javac.jlc.JLCDiagnostics.Severity;
import com.sun.tools.javac.jlc.JLCDiagnostics.SourceFileBuffer;
import com.sun.tools.javac.jlc.JLCDiagnostics.SourceSpan;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * ============================================================================
 * JLCStaticAnalyzer — FCS Semantic, Liveness & Safety Static Code Analyzer
 * ============================================================================
 * Performs five compile-time static analyses for JLC / PJCF:
 * 1. Resource & Scope Leak Detection:
 *    Detects unclosed AutoCloseable, BatchScope, PrecisionScope, Foreign Arena,
 *    and RedjetTube instances outside try-with-resources or explicit close().
 * 2. IEEE-754 Bad-Coder Financial Shield:
 *    Detects @PrecisionTarget(FAST_RELAXED) combined with BigDecimal, Currency,
 *    or financial/ledger state and triggers mandatory IEEE_EXACT lock.
 * 3. Loop Ephemeral Allocation & Autoboxing Analysis:
 *    Flags 'new Integer(...)', 'Double.valueOf(...)', and non-escaping object
 *    churn inside loops for scalar replacement or BatchScope wrapping.
 * 4. Backward Dataflow Liveness & TailTraces Clasp Synthesis:
 *    Computes exact last-use statement coordinates for object references and
 *    @RJGC(Mark = "ImmediateEviction") off-heap handles so VAC/RedjetGC can
 *    emit dead-reference clasp signals immediately upon last use.
 * 5. Static System.gc() / Runtime.gc() Lookahead Detection:
 *    Identifies explicit GC calls and computes preflight clean-hint injection
 *    points ahead of the callsite (-XX:+RedjetGCInvokesExplicitCleanHints).
 * ============================================================================
 */
public final class JLCStaticAnalyzer {

    private static final int DEFAULT_PREFLIGHT_LOOKAHEAD_LINES = 4;

    private static final Set<String> SCOPED_RESOURCE_TYPES = new HashSet<>(Arrays.asList(
            "BatchScope", "PrecisionScope", "Arena", "RedjetTube",
            "FileInputStream", "FileOutputStream", "BufferedReader", "BufferedWriter",
            "Socket", "ServerSocket", "FileChannel", "SocketChannel", "Stream"
    ));

    private static final Set<String> BOXED_WRAPPER_TYPES = new HashSet<>(Arrays.asList(
            "Integer", "Long", "Double", "Float", "Short", "Byte", "Boolean", "Character"
    ));

    private static final Map<String, String> BOX_TO_PRIMITIVE = new HashMap<>();
    static {
        BOX_TO_PRIMITIVE.put("Integer", "int");
        BOX_TO_PRIMITIVE.put("Long", "long");
        BOX_TO_PRIMITIVE.put("Double", "double");
        BOX_TO_PRIMITIVE.put("Float", "float");
        BOX_TO_PRIMITIVE.put("Short", "short");
        BOX_TO_PRIMITIVE.put("Byte", "byte");
        BOX_TO_PRIMITIVE.put("Boolean", "boolean");
        BOX_TO_PRIMITIVE.put("Character", "char");
    }

    private static final String[] FINANCIAL_KEYWORDS = {
            "bigdecimal", "currency", "monetary", "balance", "ledger",
            "invoice", "interestrate", "accountbalance", "satoshi", "taxamount", "payment"
    };

    private static final Pattern METHOD_DECL_PATTERN = Pattern.compile(
            "(?:public|protected|private|static|final|synchronized|native|strictfp|\\s)+[\\w<>\\[\\],.?\\s]+\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s*\\([^;]*\\)\\s*(?:throws[^{]+)?\\{"
    );

    private static final Pattern LOCAL_OBJECT_DEF_PATTERN = Pattern.compile(
            "^\\s*(?:final\\s+)?(?:@RJGC\\s*\\([^)]*\\)\\s*)?([A-Z][A-Za-z0-9_$<>\\[\\]]*)\\s+([a-zA-Z_$][a-zA-Z0-9_$]*)\\s*=\\s*(.+);\\s*$"
    );

    private static final Pattern BOXING_CTOR_PATTERN = Pattern.compile(
            "\\bnew\\s+(Integer|Long|Double|Float|Short|Byte|Boolean|Character)\\s*\\(([^()]+)\\)"
    );

    private static final Pattern BOXING_VALUEOF_PATTERN = Pattern.compile(
            "\\b(Integer|Long|Double|Float|Short|Byte|Boolean|Character)\\.valueOf\\s*\\(([^()]+)\\)"
    );

    private static final Pattern IDENTIFIER_TOKEN_PATTERN = Pattern.compile(
            "\\b([a-zA-Z_$][a-zA-Z0-9_$]*)\\b"
    );

    /**
     * TailTraces / @RJGC dead-reference clasp descriptor synthesized per local reference.
     */
    public static final class DeadReferenceClasp {
        public final String methodName;
        public final String variableName;
        public final String declaredType;
        public final int localSlotIndex;
        public final int declaredLine;
        public final int lastUseLine;
        public final boolean offHeapImmediateEviction;
        public final boolean continuousFlowWatch;

        public DeadReferenceClasp(String methodName,
                                  String variableName,
                                  String declaredType,
                                  int localSlotIndex,
                                  int declaredLine,
                                  int lastUseLine,
                                  boolean offHeapImmediateEviction,
                                  boolean continuousFlowWatch) {
            this.methodName = methodName;
            this.variableName = variableName;
            this.declaredType = declaredType;
            this.localSlotIndex = localSlotIndex;
            this.declaredLine = declaredLine;
            this.lastUseLine = lastUseLine;
            this.offHeapImmediateEviction = offHeapImmediateEviction;
            this.continuousFlowWatch = continuousFlowWatch;
        }
    }

    /**
     * Static preflight clean-hint descriptor emitted ahead of System.gc() / Runtime.gc().
     */
    public static final class PreflightCleanHintSite {
        public final String methodName;
        public final int explicitGcLine;
        public final int preflightInsertLine;
        public final int estimatedLiveObjectPressure;

        public PreflightCleanHintSite(String methodName,
                                      int explicitGcLine,
                                      int preflightInsertLine,
                                      int estimatedLiveObjectPressure) {
            this.methodName = methodName;
            this.explicitGcLine = explicitGcLine;
            this.preflightInsertLine = preflightInsertLine;
            this.estimatedLiveObjectPressure = estimatedLiveObjectPressure;
        }
    }

    /**
     * Unclosed resource leak candidate for JLCBugfixer Auto-Scope wrapping.
     */
    public static final class UnclosedResourceSite {
        public final String methodName;
        public final String resourceType;
        public final String variableName;
        public final int declarationLine;
        public final int scopeEndLine;

        public UnclosedResourceSite(String methodName,
                                    String resourceType,
                                    String variableName,
                                    int declarationLine,
                                    int scopeEndLine) {
            this.methodName = methodName;
            this.resourceType = resourceType;
            this.variableName = variableName;
            this.declarationLine = declarationLine;
            this.scopeEndLine = scopeEndLine;
        }
    }

    /**
     * IEEE-754 precision violation site where FAST_RELAXED must be locked to IEEE_EXACT.
     */
    public static final class PrecisionShieldSite {
        public final String methodName;
        public final int annotationLine;
        public final int financialEvidenceLine;
        public final String financialToken;

        public PrecisionShieldSite(String methodName,
                                   int annotationLine,
                                   int financialEvidenceLine,
                                   String financialToken) {
            this.methodName = methodName;
            this.annotationLine = annotationLine;
            this.financialEvidenceLine = financialEvidenceLine;
            this.financialToken = financialToken;
        }
    }

    /**
     * Boxing-to-scalar replacement site inside hot/loop code.
     */
    public static final class BoxingSite {
        public final String methodName;
        public final int line;
        public final String wrapperType;
        public final String primitiveType;
        public final String innerExpression;
        public final String matchedConstruct;

        public BoxingSite(String methodName,
                          int line,
                          String wrapperType,
                          String primitiveType,
                          String innerExpression,
                          String matchedConstruct) {
            this.methodName = methodName;
            this.line = line;
            this.wrapperType = wrapperType;
            this.primitiveType = primitiveType;
            this.innerExpression = innerExpression;
            this.matchedConstruct = matchedConstruct;
        }
    }

    /**
     * Complete immutable output report of JLCStaticAnalyzer.
     */
    public static final class AnalysisReport {
        public final SourceFileBuffer sourceBuffer;
        public final List<DeadReferenceClasp> deadReferenceClasps;
        public final List<PreflightCleanHintSite> preflightCleanHints;
        public final List<UnclosedResourceSite> unclosedResources;
        public final List<PrecisionShieldSite> precisionShieldSites;
        public final List<BoxingSite> boxingSites;
        public final List<DiagnosticRecord> diagnostics;

        public AnalysisReport(SourceFileBuffer sourceBuffer,
                              List<DeadReferenceClasp> deadReferenceClasps,
                              List<PreflightCleanHintSite> preflightCleanHints,
                              List<UnclosedResourceSite> unclosedResources,
                              List<PrecisionShieldSite> precisionShieldSites,
                              List<BoxingSite> boxingSites,
                              List<DiagnosticRecord> diagnostics) {
            this.sourceBuffer = sourceBuffer;
            this.deadReferenceClasps = Collections.unmodifiableList(deadReferenceClasps);
            this.preflightCleanHints = Collections.unmodifiableList(preflightCleanHints);
            this.unclosedResources = Collections.unmodifiableList(unclosedResources);
            this.precisionShieldSites = Collections.unmodifiableList(precisionShieldSites);
            this.boxingSites = Collections.unmodifiableList(boxingSites);
            this.diagnostics = Collections.unmodifiableList(diagnostics);
        }
    }

    private static final class MethodSlice {
        final String name;
        final int annotationStartLine;
        final int bodyStartLine;
        final int bodyEndLine;

        MethodSlice(String name, int annotationStartLine, int bodyStartLine, int bodyEndLine) {
            this.name = name;
            this.annotationStartLine = annotationStartLine;
            this.bodyStartLine = bodyStartLine;
            this.bodyEndLine = bodyEndLine;
        }
    }

    private static final class TrackedLocal {
        final String varName;
        final String typeName;
        final int slotIndex;
        final int declLine;
        final boolean immediateEviction;
        final boolean continuousFlowWatch;
        boolean escaped;
        int lastUseLine;

        TrackedLocal(String varName,
                     String typeName,
                     int slotIndex,
                     int declLine,
                     boolean immediateEviction,
                     boolean continuousFlowWatch) {
            this.varName = varName;
            this.typeName = typeName;
            this.slotIndex = slotIndex;
            this.declLine = declLine;
            this.immediateEviction = immediateEviction;
            this.continuousFlowWatch = continuousFlowWatch;
            this.escaped = false;
            this.lastUseLine = declLine;
        }
    }

    /**
     * Runs all 5 static analysis passes over a Java compilation unit.
     */
    public AnalysisReport analyzeSource(String filePath, String sourceCode, JLCDiagnostics sink) {
        SourceFileBuffer buffer = new SourceFileBuffer(filePath, sourceCode);
        List<DeadReferenceClasp> clasps = new ArrayList<>();
        List<PreflightCleanHintSite> preflightHints = new ArrayList<>();
        List<UnclosedResourceSite> unclosedSites = new ArrayList<>();
        List<PrecisionShieldSite> shieldSites = new ArrayList<>();
        List<BoxingSite> boxingSites = new ArrayList<>();
        List<DiagnosticRecord> diagnostics = new ArrayList<>();

        List<MethodSlice> methods = discoverMethodSlices(buffer);
        if (methods.isEmpty() && buffer.lineCount() > 0) {
            methods.add(new MethodSlice("<unit>", 1, 1, buffer.lineCount()));
        }

        for (MethodSlice method : methods) {
            analyzeMethodSlice(
                    buffer,
                    method,
                    clasps,
                    preflightHints,
                    unclosedSites,
                    shieldSites,
                    boxingSites,
                    diagnostics);
        }

        if (sink != null) {
            for (DiagnosticRecord d : diagnostics) {
                sink.report(d);
            }
        }

        return new AnalysisReport(
                buffer,
                clasps,
                preflightHints,
                unclosedSites,
                shieldSites,
                boxingSites,
                diagnostics);
    }

    private void analyzeMethodSlice(SourceFileBuffer buffer,
                                    MethodSlice method,
                                    List<DeadReferenceClasp> clasps,
                                    List<PreflightCleanHintSite> preflightHints,
                                    List<UnclosedResourceSite> unclosedSites,
                                    List<PrecisionShieldSite> shieldSites,
                                    List<BoxingSite> boxingSites,
                                    List<DiagnosticRecord> diagnostics) {

        int relaxedAnnotationLine = -1;
        for (int line = method.annotationStartLine; line <= method.bodyStartLine; line++) {
            String text = buffer.lineAt(line);
            if (text.contains("@PrecisionTarget") && text.contains("FAST_RELAXED")) {
                relaxedAnnotationLine = line;
                break;
            }
        }

        int financialLine = -1;
        String financialToken = null;
        int loopDepth = 0;
        int nextLocalSlot = 1; // slot 0 reserved for 'this' / first arg frame base
        Map<String, TrackedLocal> trackedLocals = new LinkedHashMap<>();
        Map<String, Integer> unclosedCandidates = new LinkedHashMap<>();
        Map<String, String> unclosedTypes = new HashMap<>();
        String pendingRjgcMark = null;

        for (int line = method.bodyStartLine; line <= method.bodyEndLine; line++) {
            String rawLine = buffer.lineAt(line);
            String trimmed = stripLineComment(rawLine).trim();
            if (trimmed.isEmpty()) {
                continue;
            }

            // Track standalone @RJGC annotation preceding local declaration
            if (trimmed.startsWith("@RJGC")) {
                pendingRjgcMark = extractRjgcMark(trimmed);
                if (!trimmed.contains(";")) {
                    continue;
                }
            }

            // Track loop nesting depth for hot-loop boxing & allocation diagnostics
            if (isLoopHeader(trimmed)) {
                loopDepth++;
            }

            // 1. IEEE-754 Financial / Monetary evidence check
            if (financialLine < 0) {
                String lower = trimmed.toLowerCase(Locale.ROOT);
                for (String kw : FINANCIAL_KEYWORDS) {
                    if (lower.contains(kw)) {
                        financialLine = line;
                        financialToken = kw;
                        break;
                    }
                }
            }

            // 2. Boxing-to-Scalar detection (especially inside loops or arithmetic expressions)
            detectBoxingConstructs(buffer.filePath, method.name, line, rawLine, trimmed, loopDepth > 0, boxingSites, diagnostics);

            // 3. Static System.gc() / Runtime.getRuntime().gc() lookahead detection
            if (trimmed.contains("System.gc()") || trimmed.contains("Runtime.getRuntime().gc()")) {
                int insertLine = Math.max(method.bodyStartLine + 1, line - DEFAULT_PREFLIGHT_LOOKAHEAD_LINES);
                int pressure = Math.max(1, trackedLocals.size());
                preflightHints.add(new PreflightCleanHintSite(method.name, line, insertLine, pressure));

                int col = Math.max(1, rawLine.indexOf("gc()") - 6);
                SourceSpan gcSpan = SourceSpan.primary(
                        buffer.filePath, line, col, col + 11,
                        "explicit GC callsite intercepted for 0-STW Redjet preflight clean hint");
                SourceSpan preflightSpan = SourceSpan.secondary(
                        buffer.filePath, insertLine, 1, Math.max(2, buffer.lineAt(insertLine).length()),
                        "preflight clean-hint window armed here");
                QuickFixSuggestion qf = new QuickFixSuggestion(
                        "Pre-arm RedjetGC asynchronous micro-cleanse ahead of System.gc()",
                        insertLine,
                        "",
                        "jdk.gc.redjet.GCHints.ThrowGCHint(); /* [JLC Lookahead Clean Hint] */",
                        false);
                diagnostics.add(new DiagnosticRecord(
                        Severity.NOTE,
                        "JLC-N0501",
                        "explicit System.gc() converted to lookahead RedjetGC clean hint in '" + method.name + "'",
                        Arrays.asList(preflightSpan, gcSpan),
                        Collections.singletonList("With -XX:+RedjetGCInvokesExplicitCleanHints, VAC/JLC pre-arms RedjetSeasonalCoordinator "
                                + (line - insertLine) + " statements before System.gc() for 0ms STW cleansing."),
                        qf));
            }

            // 4. Local object / off-heap resource definitions
            Matcher defMatcher = LOCAL_OBJECT_DEF_PATTERN.matcher(trimmed);
            if (defMatcher.matches() && !trimmed.startsWith("try") && !trimmed.startsWith("return ")) {
                String typeName = simpleTypeName(defMatcher.group(1));
                String varName = defMatcher.group(2);
                String rhs = defMatcher.group(3);

                String inlineRjgc = trimmed.contains("@RJGC") ? extractRjgcMark(trimmed) : pendingRjgcMark;
                pendingRjgcMark = null;

                boolean immEvict = "ImmediateEviction".equalsIgnoreCase(inlineRjgc)
                        || ("MemorySegment".equals(typeName) && inlineRjgc == null && rhs.contains("allocate"));
                boolean flowWatch = "ContinuousFlowWatch".equalsIgnoreCase(inlineRjgc);

                TrackedLocal tl = new TrackedLocal(varName, typeName, nextLocalSlot++, line, immEvict, flowWatch);
                trackedLocals.put(varName, tl);

                if (SCOPED_RESOURCE_TYPES.contains(typeName) && !isInsideTryHeader(rawLine)) {
                    unclosedCandidates.put(varName, line);
                    unclosedTypes.put(varName, typeName);
                }
            } else {
                pendingRjgcMark = null;
            }

            // 5. Update variable uses, escapes, and explicit .close() calls
            Matcher idMatcher = IDENTIFIER_TOKEN_PATTERN.matcher(trimmed);
            boolean isReturnOrThrow = trimmed.startsWith("return ") || trimmed.startsWith("throw ");
            while (idMatcher.find()) {
                String tok = idMatcher.group(1);
                TrackedLocal tracked = trackedLocals.get(tok);
                if (tracked != null && line > tracked.declLine) {
                    tracked.lastUseLine = line;
                    if (isReturnOrThrow || isFieldEscapeAssignment(trimmed, tok)) {
                        tracked.escaped = true;
                    }
                }
                if (unclosedCandidates.containsKey(tok) && trimmed.contains(tok + ".close(")) {
                    unclosedCandidates.remove(tok);
                }
            }

            if (loopDepth > 0 && trimmed.endsWith("}") && countChar(trimmed, '}') > countChar(trimmed, '{')) {
                loopDepth = Math.max(0, loopDepth - 1);
            }
        }

        // Emit IEEE-754 Bad-Coder Shield Lock if FAST_RELAXED collided with financial logic
        if (relaxedAnnotationLine > 0 && financialLine > 0) {
            shieldSites.add(new PrecisionShieldSite(method.name, relaxedAnnotationLine, financialLine, financialToken));
            String annoText = buffer.lineAt(relaxedAnnotationLine);
            int startCol = Math.max(1, annoText.indexOf("@PrecisionTarget") + 1);
            SourceSpan annoSpan = SourceSpan.primary(
                    buffer.filePath,
                    relaxedAnnotationLine,
                    startCol,
                    startCol + "@PrecisionTarget".length(),
                    "unsafe FAST_RELAXED mode requested on monetary/financial method");
            SourceSpan finSpan = SourceSpan.secondary(
                    buffer.filePath,
                    financialLine,
                    1,
                    Math.max(2, buffer.lineAt(financialLine).length()),
                    "financial symbol '" + financialToken + "' detected here");
            String fixedAnno = annoText.replace("FAST_RELAXED", "IEEE_EXACT");
            QuickFixSuggestion qf = new QuickFixSuggestion(
                    "Lock @PrecisionTarget to strict IEEE_EXACT mode (IEEE-754 Bad-Coder Shield)",
                    relaxedAnnotationLine,
                    annoText,
                    fixedAnno,
                    true);
            diagnostics.add(new DiagnosticRecord(
                    Severity.SHIELD,
                    "JLC-E0201",
                    "IEEE-754 Bad-Coder Shield triggered in '" + method.name + "': FAST_RELAXED forbidden on financial arithmetic",
                    Arrays.asList(annoSpan, finSpan),
                    Collections.singletonList("Relaxed FMA/reassociation introduces non-associative rounding drift in ledger math; JLC locked this method to IEEE_EXACT (MIRC Slot 15)."),
                    qf));
        }

        // Emit Unclosed Resource sites & QuickFix diagnostics
        for (Map.Entry<String, Integer> entry : unclosedCandidates.entrySet()) {
            String varName = entry.getKey();
            int declLine = entry.getValue();
            String resType = unclosedTypes.getOrDefault(varName, "AutoCloseable");
            int scopeEnd = Math.max(declLine, method.bodyEndLine - 1);
            unclosedSites.add(new UnclosedResourceSite(method.name, resType, varName, declLine, scopeEnd));

            String origLine = buffer.lineAt(declLine);
            int col = Math.max(1, origLine.indexOf(varName) + 1);
            SourceSpan span = SourceSpan.primary(
                    buffer.filePath,
                    declLine,
                    col,
                    col + varName.length(),
                    "scoped resource '" + varName + "' (" + resType + ") is never closed on method exit");
            QuickFixSuggestion qf = new QuickFixSuggestion(
                    "Wrap '" + varName + "' in deterministic exception-safe try-with-resources scope",
                    declLine,
                    origLine,
                    "try (" + stripTrailingSemicolon(origLine.trim()) + ") { /* ... scope body ... */ }",
                    true);
            diagnostics.add(new DiagnosticRecord(
                    Severity.WARNING,
                    "JLC-W0101",
                    "unclosed resource '" + varName + "' of type '" + resType + "' in method '" + method.name + "'",
                    Collections.singletonList(span),
                    Collections.singletonList("JLC Auto-Scope Fixer can wrap this resource in try-with-resources to guarantee zero native/arena memory leaks."),
                    qf));
        }

        // Emit TailTraces & @RJGC ImmediateEviction Dead-Reference Clasps
        for (TrackedLocal tl : trackedLocals.values()) {
            if (!tl.escaped && !BOXED_WRAPPER_TYPES.contains(tl.typeName)) {
                clasps.add(new DeadReferenceClasp(
                        method.name,
                        tl.varName,
                        tl.typeName,
                        tl.slotIndex,
                        tl.declLine,
                        tl.lastUseLine,
                        tl.immediateEviction,
                        tl.continuousFlowWatch));
            }
        }
    }

    private void detectBoxingConstructs(String filePath,
                                        String methodName,
                                        int line,
                                        String rawLine,
                                        String trimmed,
                                        boolean inLoop,
                                        List<BoxingSite> boxingSites,
                                        List<DiagnosticRecord> diagnostics) {
        Matcher ctorMatcher = BOXING_CTOR_PATTERN.matcher(trimmed);
        if (ctorMatcher.find()) {
            String wrapper = ctorMatcher.group(1);
            String expr = ctorMatcher.group(2).trim();
            String prim = BOX_TO_PRIMITIVE.getOrDefault(wrapper, "int");
            String fullMatch = ctorMatcher.group(0);
            boxingSites.add(new BoxingSite(methodName, line, wrapper, prim, expr, fullMatch));

            int col = Math.max(1, rawLine.indexOf(fullMatch) + 1);
            SourceSpan span = SourceSpan.primary(
                    filePath,
                    line,
                    col,
                    col + fullMatch.length(),
                    (inLoop ? "hot-loop " : "") + "heap wrapper allocation '" + fullMatch + "' creates avoidable GC pressure");
            String fixedLine = rawLine.replace(fullMatch, "((" + prim + ") (" + expr + "))")
                                      .replaceFirst("\\b" + wrapper + "\\s+", prim + " ");
            QuickFixSuggestion qf = new QuickFixSuggestion(
                    "Unbox '" + wrapper + "' directly to primitive '" + prim + "' register scalar",
                    line,
                    rawLine,
                    fixedLine,
                    inLoop);
            diagnostics.add(new DiagnosticRecord(
                    Severity.WARNING,
                    "JLC-W0301",
                    "redundant boxed allocation 'new " + wrapper + "(...)' in '" + methodName + "'",
                    Collections.singletonList(span),
                    Collections.singletonList("Scalar primitive '" + prim + "' stays directly in CPU GPR/FPR registers with zero header overhead."),
                    qf));
            return;
        }

        if (inLoop) {
            Matcher valMatcher = BOXING_VALUEOF_PATTERN.matcher(trimmed);
            if (valMatcher.find()) {
                String wrapper = valMatcher.group(1);
                String expr = valMatcher.group(2).trim();
                String prim = BOX_TO_PRIMITIVE.getOrDefault(wrapper, "int");
                String fullMatch = valMatcher.group(0);
                boxingSites.add(new BoxingSite(methodName, line, wrapper, prim, expr, fullMatch));

                int col = Math.max(1, rawLine.indexOf(fullMatch) + 1);
                SourceSpan span = SourceSpan.primary(
                        filePath,
                        line,
                        col,
                        col + fullMatch.length(),
                        "autoboxing '" + fullMatch + "' inside loop inhibits SIMD vectorization");
                String fixedLine = rawLine.replace(fullMatch, "((" + prim + ") (" + expr + "))")
                                          .replaceFirst("\\b" + wrapper + "\\s+", prim + " ");
                QuickFixSuggestion qf = new QuickFixSuggestion(
                        "Convert loop '" + wrapper + ".valueOf' into unboxed '" + prim + "' scalar",
                        line,
                        rawLine,
                        fixedLine,
                        true);
                diagnostics.add(new DiagnosticRecord(
                        Severity.WARNING,
                        "JLC-W0302",
                        "loop autoboxing '" + wrapper + ".valueOf(...)' detected in '" + methodName + "'",
                        Collections.singletonList(span),
                        Collections.singletonList("Replacing boxed wrappers in loops unlocks MIRC AVX-512 / SVE2 / RVV vector lanes."),
                        qf));
            }
        }
    }

    private List<MethodSlice> discoverMethodSlices(SourceFileBuffer buffer) {
        List<MethodSlice> slices = new ArrayList<>();
        int totalLines = buffer.lineCount();
        int pendingAnnoLine = -1;

        for (int line = 1; line <= totalLines; line++) {
            String trimmed = stripLineComment(buffer.lineAt(line)).trim();
            if (trimmed.startsWith("@")) {
                if (pendingAnnoLine < 0) {
                    pendingAnnoLine = line;
                }
                continue;
            }
            Matcher m = METHOD_DECL_PATTERN.matcher(trimmed);
            if (m.find() && !trimmed.startsWith("class ") && !trimmed.startsWith("interface ")) {
                String methodName = m.group(1);
                int annoStart = (pendingAnnoLine > 0) ? pendingAnnoLine : line;
                pendingAnnoLine = -1;
                int braceBalance = countChar(trimmed, '{') - countChar(trimmed, '}');
                int endLine = line;
                while (braceBalance > 0 && endLine < totalLines) {
                    endLine++;
                    String bodyLine = stripLineComment(buffer.lineAt(endLine));
                    braceBalance += countChar(bodyLine, '{') - countChar(bodyLine, '}');
                }
                slices.add(new MethodSlice(methodName, annoStart, line, endLine));
                line = endLine;
            } else if (!trimmed.isEmpty()) {
                pendingAnnoLine = -1;
            }
        }
        return slices;
    }

    private static boolean isLoopHeader(String trimmed) {
        return trimmed.startsWith("for (") || trimmed.startsWith("for(")
                || trimmed.startsWith("while (") || trimmed.startsWith("while(")
                || trimmed.startsWith("do {") || trimmed.equals("do");
    }

    private static boolean isInsideTryHeader(String rawLine) {
        String s = rawLine.trim();
        return s.startsWith("try (") || s.startsWith("try(");
    }

    private static boolean isFieldEscapeAssignment(String trimmed, String varName) {
        int eqIdx = trimmed.indexOf('=');
        if (eqIdx <= 0 || (eqIdx + 1 < trimmed.length() && trimmed.charAt(eqIdx + 1) == '=')) {
            return false;
        }
        String lhs = trimmed.substring(0, eqIdx).trim();
        String rhs = trimmed.substring(eqIdx + 1).trim();
        return (lhs.contains(".") || lhs.contains("[")) && rhs.contains(varName);
    }

    private static String extractRjgcMark(String text) {
        if (text.contains("ContinuousFlowWatch")) {
            return "ContinuousFlowWatch";
        }
        return "ImmediateEviction";
    }

    private static String simpleTypeName(String rawType) {
        String s = rawType.trim();
        int genIdx = s.indexOf('<');
        if (genIdx > 0) {
            s = s.substring(0, genIdx);
        }
        int dotIdx = s.lastIndexOf('.');
        if (dotIdx >= 0 && dotIdx + 1 < s.length()) {
            s = s.substring(dotIdx + 1);
        }
        return s;
    }

    private static String stripLineComment(String line) {
        if (line == null) {
            return "";
        }
        int idx = line.indexOf("//");
        return (idx >= 0) ? line.substring(0, idx) : line;
    }

    private static String stripTrailingSemicolon(String s) {
        return s.endsWith(";") ? s.substring(0, s.length() - 1).trim() : s;
    }

    private static int countChar(String s, char target) {
        int c = 0;
        for (int i = 0; i < s.length(); i++) {
            if (s.charAt(i) == target) {
                c++;
            }
        }
        return c;
    }
}
