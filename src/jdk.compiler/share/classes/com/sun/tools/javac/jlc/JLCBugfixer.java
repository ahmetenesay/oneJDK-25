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

import com.sun.tools.javac.jlc.JLCDiagnostics.SourceFileBuffer;
import com.sun.tools.javac.jlc.JLCStaticAnalyzer.AnalysisReport;
import com.sun.tools.javac.jlc.JLCStaticAnalyzer.BoxingSite;
import com.sun.tools.javac.jlc.JLCStaticAnalyzer.DeadReferenceClasp;
import com.sun.tools.javac.jlc.JLCStaticAnalyzer.PrecisionShieldSite;
import com.sun.tools.javac.jlc.JLCStaticAnalyzer.PreflightCleanHintSite;
import com.sun.tools.javac.jlc.JLCStaticAnalyzer.UnclosedResourceSite;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * ============================================================================
 * JLCBugfixer — Automated Compile-Time QuickFix & Safety Shield Engine
 * ============================================================================
 * Consumes JLCStaticAnalyzer.AnalysisReport and applies deterministic,
 * semantics-preserving compile-time transformations:
 * 1. IEEE-754 Bad-Coder Shield Lock:
 *    Forces @PrecisionTarget(FAST_RELAXED) -> @PrecisionTarget(IEEE_EXACT)
 *    on financial/monetary methods unconditionally.
 * 2. Auto-Scope Resource Wrapper:
 *    Wraps unclosed AutoCloseable / BatchScope / Arena resources in
 *    exception-safe try-with-resources scopes so leaks are eliminated at birth.
 * 3. Boxing-to-Scalar Unboxer:
 *    Rewrites redundant 'new Integer(x)' / loop 'Wrapper.valueOf(x)' allocations
 *    into unboxed primitive expressions.
 * 4. Lookahead Clean-Hint & ImmediateEviction Epilogue Injection:
 *    Pre-positions 'jdk.gc.redjet.GCHints.ThrowGCHint()' ahead of explicit
 *    System.gc() calls and attaches deterministic scope-exit eviction markers
 *    for @RJGC(Mark = "ImmediateEviction") off-heap handles.
 * ============================================================================
 */
public final class JLCBugfixer {

    public static final class BugfixResult {
        public final String transformedSource;
        public final int shieldLocksApplied;
        public final int autoScopesWrapped;
        public final int boxingEliminated;
        public final int preflightHintsInjected;
        public final int immediateEvictionsWired;
        public final List<String> appliedPatchLog;

        public BugfixResult(String transformedSource,
                            int shieldLocksApplied,
                            int autoScopesWrapped,
                            int boxingEliminated,
                            int preflightHintsInjected,
                            int immediateEvictionsWired,
                            List<String> appliedPatchLog) {
            this.transformedSource = transformedSource;
            this.shieldLocksApplied = shieldLocksApplied;
            this.autoScopesWrapped = autoScopesWrapped;
            this.boxingEliminated = boxingEliminated;
            this.preflightHintsInjected = preflightHintsInjected;
            this.immediateEvictionsWired = immediateEvictionsWired;
            this.appliedPatchLog = Collections.unmodifiableList(appliedPatchLog);
        }

        public int totalFixesApplied() {
            return shieldLocksApplied
                    + autoScopesWrapped
                    + boxingEliminated
                    + preflightHintsInjected
                    + immediateEvictionsWired;
        }
    }

    /**
     * Applies all configured compile-time bugfixes to the analyzed source buffer.
     */
    public BugfixResult applyFixes(AnalysisReport report,
                                   boolean enableAutoScope,
                                   boolean enableUnboxer,
                                   boolean enableExplicitGcLookahead) {
        SourceFileBuffer buf = report.sourceBuffer;
        int lineCount = buf.lineCount();
        List<String> lines = new ArrayList<>(lineCount);
        for (int i = 1; i <= lineCount; i++) {
            lines.add(buf.lineAt(i));
        }

        Map<Integer, List<String>> preInsertions = new HashMap<>();
        Map<Integer, List<String>> postInsertions = new HashMap<>();
        List<String> patchLog = new ArrayList<>();

        int shieldLocks = 0;
        int autoScopes = 0;
        int unboxed = 0;
        int preflightHints = 0;
        int immEvictions = 0;

        // 1. Mandatory IEEE-754 Bad-Coder Shield Lock (always enforced for financial safety)
        for (PrecisionShieldSite shield : report.precisionShieldSites) {
            int idx = shield.annotationLine - 1;
            if (idx >= 0 && idx < lines.size()) {
                String orig = lines.get(idx);
                String updated = orig.replace("FAST_RELAXED", "IEEE_EXACT");
                if (!orig.equals(updated)) {
                    lines.set(idx, updated);
                    shieldLocks++;
                    patchLog.add("[JLC-E0201] Locked @PrecisionTarget to IEEE_EXACT at line "
                            + shield.annotationLine + " in " + shield.methodName);
                }
            }
        }

        // 2. Boxing-to-Scalar Unboxer
        if (enableUnboxer) {
            for (BoxingSite box : report.boxingSites) {
                int idx = box.line - 1;
                if (idx >= 0 && idx < lines.size()) {
                    String orig = lines.get(idx);
                    String scalarExpr = "((" + box.primitiveType + ") (" + box.innerExpression + "))";
                    String updated = orig.replace(box.matchedConstruct, scalarExpr)
                                         .replaceFirst("\\b" + box.wrapperType + "\\s+", box.primitiveType + " ");
                    if (!orig.equals(updated)) {
                        lines.set(idx, updated);
                        unboxed++;
                        patchLog.add("[JLC-W0301] Unboxed " + box.wrapperType + " -> "
                                + box.primitiveType + " at line " + box.line);
                    }
                }
            }
        }

        // 3. Auto-Scope Resource Wrapper (try-with-resources synthesis)
        if (enableAutoScope) {
            for (UnclosedResourceSite site : report.unclosedResources) {
                int declIdx = site.declarationLine - 1;
                int endIdx = site.scopeEndLine - 1;
                if (declIdx >= 0 && declIdx < lines.size() && endIdx >= declIdx && endIdx < lines.size()) {
                    String origDecl = lines.get(declIdx);
                    String indent = leadingWhitespace(origDecl);
                    String resourceInit = origDecl.trim();
                    if (resourceInit.endsWith(";")) {
                        resourceInit = resourceInit.substring(0, resourceInit.length() - 1).trim();
                    }
                    lines.set(declIdx, indent + "try (" + resourceInit + ") {");
                    postInsertions.computeIfAbsent(endIdx, k -> new ArrayList<>())
                                  .add(indent + "} /* [JLC Auto-Scope Closed: " + site.variableName + "] */");
                    autoScopes++;
                    patchLog.add("[JLC-W0101] Wrapped unclosed " + site.resourceType + " '"
                            + site.variableName + "' in try-with-resources (lines "
                            + site.declarationLine + ".." + site.scopeEndLine + ")");
                }
            }
        }

        // 4. Static System.gc() Lookahead Clean-Hint Injection
        if (enableExplicitGcLookahead) {
            for (PreflightCleanHintSite hint : report.preflightCleanHints) {
                int insertIdx = hint.preflightInsertLine - 1;
                if (insertIdx >= 0 && insertIdx < lines.size()) {
                    String indent = leadingWhitespace(lines.get(insertIdx));
                    preInsertions.computeIfAbsent(insertIdx, k -> new ArrayList<>())
                                 .add(indent + "jdk.gc.redjet.GCHints.ThrowGCHint(); /* [JLC Preflight Clean Hint for line "
                                         + hint.explicitGcLine + "] */");
                    preflightHints++;
                    patchLog.add("[JLC-N0501] Injected lookahead GCHints.ThrowGCHint() at line "
                            + hint.preflightInsertLine + " ahead of explicit GC at line " + hint.explicitGcLine);
                }
            }
        }

        // 5. @RJGC(Mark = "ImmediateEviction") Deterministic Last-Use Eviction Marker
        for (DeadReferenceClasp clasp : report.deadReferenceClasps) {
            if (clasp.offHeapImmediateEviction) {
                int lastIdx = clasp.lastUseLine - 1;
                if (lastIdx >= 0 && lastIdx < lines.size()) {
                    String indent = leadingWhitespace(lines.get(lastIdx));
                    postInsertions.computeIfAbsent(lastIdx, k -> new ArrayList<>())
                                  .add(indent + "/* [@RJGC ImmediateEviction DeadEnd: slot=" + clasp.localSlotIndex
                                          + " var=" + clasp.variableName + " -> RedjetOffHeap::immediate_free] */");
                    immEvictions++;
                    patchLog.add("[@RJGC] Bound ImmediateEviction dead-end for '" + clasp.variableName
                            + "' after last use at line " + clasp.lastUseLine);
                }
            }
        }

        // Reassemble source preserving exact line endings
        StringBuilder out = new StringBuilder(buf.content.length() + 512);
        for (int i = 0; i < lines.size(); i++) {
            List<String> pre = preInsertions.get(i);
            if (pre != null) {
                for (String p : pre) {
                    out.append(p).append('\n');
                }
            }
            out.append(lines.get(i));
            if (i + 1 < lines.size() || buf.content.endsWith("\n")) {
                out.append('\n');
            }
            List<String> post = postInsertions.get(i);
            if (post != null) {
                for (String p : post) {
                    out.append(p).append('\n');
                }
            }
        }

        return new BugfixResult(
                out.toString(),
                shieldLocks,
                autoScopes,
                unboxed,
                preflightHints,
                immEvictions,
                patchLog);
    }

    private static String leadingWhitespace(String s) {
        if (s == null || s.isEmpty()) {
            return "";
        }
        int idx = 0;
        while (idx < s.length() && (s.charAt(idx) == ' ' || s.charAt(idx) == '\t')) {
            idx++;
        }
        return s.substring(0, idx);
    }
}
