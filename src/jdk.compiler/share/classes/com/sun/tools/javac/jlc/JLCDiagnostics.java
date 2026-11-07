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

import java.io.PrintStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * ============================================================================
 * JLCDiagnostics — Rust/Cargo-Style Multi-Span Diagnostic & QuickFix Renderer
 * ============================================================================
 * Replaces legacy terse javac messages with rich, human-centric terminal
 * diagnostics featuring:
 * - Structured error/warning/note/shield taxonomy (e.g. error[JLC-E0201])
 * - O(log N) binary-searched line/column span indexing over source buffers
 * - Primary (^^^) and secondary (---) ASCII source underlines with labels
 * - Actionable '= help:' architectural explanations
 * - Interactive-ready '= quickfix:' unified diff previews
 * ============================================================================
 */
public final class JLCDiagnostics {

    public enum Severity {
        ERROR("error", "\u001B[1;31m"),
        SHIELD("shield", "\u001B[1;35m"),
        WARNING("warning", "\u001B[1;33m"),
        NOTE("note", "\u001B[1;36m"),
        HELP("help", "\u001B[1;32m");

        public final String label;
        public final String ansiCode;

        Severity(String label, String ansiCode) {
            this.label = label;
            this.ansiCode = ansiCode;
        }
    }

    private static final String ANSI_RESET = "\u001B[0m";
    private static final String ANSI_BOLD  = "\u001B[1m";
    private static final String ANSI_BLUE  = "\u001B[1;34m";
    private static final String ANSI_GREEN = "\u001B[1;32m";
    private static final String ANSI_RED   = "\u001B[1;31m";

    /**
     * Indexed source file buffer supporting O(log N) offset-to-line/col lookup.
     */
    public static final class SourceFileBuffer {
        public final String filePath;
        public final String content;
        private final String[] lines;
        private final int[] lineStartOffsets;

        public SourceFileBuffer(String filePath, String content) {
            this.filePath = (filePath != null && !filePath.isEmpty()) ? filePath : "<memory>.java";
            this.content = (content != null) ? content : "";
            List<String> splitLines = new ArrayList<>();
            List<Integer> starts = new ArrayList<>();
            int len = this.content.length();
            int lineStart = 0;
            starts.add(0);
            for (int i = 0; i < len; i++) {
                char c = this.content.charAt(i);
                if (c == '\n') {
                    int end = (i > lineStart && this.content.charAt(i - 1) == '\r') ? (i - 1) : i;
                    splitLines.add(this.content.substring(lineStart, end));
                    lineStart = i + 1;
                    if (lineStart <= len) {
                        starts.add(lineStart);
                    }
                }
            }
            if (lineStart <= len) {
                splitLines.add(this.content.substring(lineStart));
            }
            this.lines = splitLines.toArray(new String[0]);
            this.lineStartOffsets = new int[starts.size()];
            for (int i = 0; i < starts.size(); i++) {
                this.lineStartOffsets[i] = starts.get(i);
            }
        }

        public int lineCount() {
            return lines.length;
        }

        public String lineAt(int oneBasedLine) {
            if (oneBasedLine < 1 || oneBasedLine > lines.length) {
                return "";
            }
            return lines[oneBasedLine - 1];
        }

        /**
         * Resolves a 0-based character offset to 1-based [line, column].
         */
        public int[] resolveLineAndColumn(int charOffset) {
            if (lineStartOffsets.length == 0 || charOffset <= 0) {
                return new int[]{1, 1};
            }
            int clamped = Math.min(charOffset, content.length());
            int idx = Arrays.binarySearch(lineStartOffsets, clamped);
            int lineIdx = (idx >= 0) ? idx : (-idx - 2);
            if (lineIdx < 0) {
                lineIdx = 0;
            }
            if (lineIdx >= lines.length) {
                lineIdx = lines.length - 1;
            }
            int col = (clamped - lineStartOffsets[lineIdx]) + 1;
            return new int[]{lineIdx + 1, Math.max(1, col)};
        }
    }

    /**
     * Source location span (1-based line and column coordinates).
     */
    public static final class SourceSpan {
        public final String filePath;
        public final int startLine;
        public final int startColumn;
        public final int endLine;
        public final int endColumn;
        public final boolean primary;
        public final String inlineLabel;

        public SourceSpan(String filePath,
                          int startLine,
                          int startColumn,
                          int endLine,
                          int endColumn,
                          boolean primary,
                          String inlineLabel) {
            this.filePath = (filePath != null) ? filePath : "<unknown>";
            this.startLine = Math.max(1, startLine);
            this.startColumn = Math.max(1, startColumn);
            this.endLine = Math.max(this.startLine, endLine);
            this.endColumn = (this.endLine == this.startLine)
                    ? Math.max(this.startColumn + 1, endColumn)
                    : Math.max(1, endColumn);
            this.primary = primary;
            this.inlineLabel = (inlineLabel != null) ? inlineLabel : "";
        }

        public static SourceSpan primary(String filePath, int line, int startCol, int endCol, String label) {
            return new SourceSpan(filePath, line, startCol, line, endCol, true, label);
        }

        public static SourceSpan secondary(String filePath, int line, int startCol, int endCol, String label) {
            return new SourceSpan(filePath, line, startCol, line, endCol, false, label);
        }
    }

    /**
     * Concrete QuickFix rewrite suggestion with before/after replacement preview.
     */
    public static final class QuickFixSuggestion {
        public final String title;
        public final int targetLine;
        public final String originalCode;
        public final String replacementCode;
        public final boolean autoApplied;

        public QuickFixSuggestion(String title,
                                  int targetLine,
                                  String originalCode,
                                  String replacementCode,
                                  boolean autoApplied) {
            this.title = (title != null) ? title : "Suggested QuickFix";
            this.targetLine = Math.max(1, targetLine);
            this.originalCode = (originalCode != null) ? originalCode : "";
            this.replacementCode = (replacementCode != null) ? replacementCode : "";
            this.autoApplied = autoApplied;
        }
    }

    /**
     * Complete structured diagnostic record emitted by JLC passes.
     */
    public static final class DiagnosticRecord {
        public final Severity severity;
        public final String code;
        public final String headline;
        public final List<SourceSpan> spans;
        public final List<String> helpNotes;
        public final QuickFixSuggestion quickFix;

        public DiagnosticRecord(Severity severity,
                                String code,
                                String headline,
                                List<SourceSpan> spans,
                                List<String> helpNotes,
                                QuickFixSuggestion quickFix) {
            this.severity = (severity != null) ? severity : Severity.WARNING;
            this.code = (code != null) ? code : "JLC-W0000";
            this.headline = (headline != null) ? headline : "";
            this.spans = (spans != null) ? Collections.unmodifiableList(new ArrayList<>(spans)) : Collections.emptyList();
            this.helpNotes = (helpNotes != null) ? Collections.unmodifiableList(new ArrayList<>(helpNotes)) : Collections.emptyList();
            this.quickFix = quickFix;
        }
    }

    private final List<DiagnosticRecord> records = new ArrayList<>();
    private boolean ansiColorEnabled;

    public JLCDiagnostics(boolean ansiColorEnabled) {
        this.ansiColorEnabled = ansiColorEnabled;
    }

    public void setAnsiColorEnabled(boolean enabled) {
        this.ansiColorEnabled = enabled;
    }

    public void report(DiagnosticRecord record) {
        if (record != null) {
            records.add(record);
        }
    }

    public List<DiagnosticRecord> getRecords() {
        return Collections.unmodifiableList(records);
    }

    public int countBySeverity(Severity sev) {
        int count = 0;
        for (DiagnosticRecord r : records) {
            if (r.severity == sev) {
                count++;
            }
        }
        return count;
    }

    public boolean hasErrors() {
        return countBySeverity(Severity.ERROR) > 0;
    }

    /**
     * Formats a single DiagnosticRecord in Rust/Cargo multi-span style.
     */
    public String formatRecord(DiagnosticRecord rec, SourceFileBuffer source) {
        StringBuilder sb = new StringBuilder(512);
        String sevColor = ansiColorEnabled ? rec.severity.ansiCode : "";
        String bold = ansiColorEnabled ? ANSI_BOLD : "";
        String blue = ansiColorEnabled ? ANSI_BLUE : "";
        String green = ansiColorEnabled ? ANSI_GREEN : "";
        String red = ansiColorEnabled ? ANSI_RED : "";
        String reset = ansiColorEnabled ? ANSI_RESET : "";

        // Header: error[JLC-E0201]: headline
        sb.append(sevColor).append(rec.severity.label)
          .append('[').append(rec.code).append(']').append(reset)
          .append(bold).append(": ").append(rec.headline).append(reset).append('\n');

        int maxLineNum = 1;
        for (SourceSpan sp : rec.spans) {
            if (sp.endLine > maxLineNum) {
                maxLineNum = sp.endLine;
            }
        }
        if (rec.quickFix != null && rec.quickFix.targetLine > maxLineNum) {
            maxLineNum = rec.quickFix.targetLine;
        }
        int gutterWidth = Math.max(2, Integer.toString(maxLineNum).length());
        String emptyGutter = repeat(' ', gutterWidth);

        if (!rec.spans.isEmpty()) {
            SourceSpan lead = rec.spans.get(0);
            sb.append(emptyGutter).append(blue).append("--> ").append(reset)
              .append(lead.filePath).append(':')
              .append(lead.startLine).append(':').append(lead.startColumn).append('\n');
            sb.append(emptyGutter).append(blue).append(" |").append(reset).append('\n');

            for (SourceSpan sp : rec.spans) {
                String srcLine = (source != null) ? source.lineAt(sp.startLine) : "";
                String expandedSrcLine = expandTabs(srcLine);
                String lineNumStr = padLeft(Integer.toString(sp.startLine), gutterWidth);
                sb.append(blue).append(lineNumStr).append(" | ").append(reset)
                  .append(expandedSrcLine).append('\n');

                int startCol = Math.max(1, sp.startColumn);
                int maxCol = Math.max(startCol + 1, expandedSrcLine.length() + 1);
                int endCol = Math.min(Math.max(startCol + 1, sp.endColumn), maxCol);
                int caretLen = Math.max(1, endCol - startCol);
                char underlineChar = sp.primary ? '^' : '-';
                String markerColor = sp.primary ? sevColor : blue;

                sb.append(emptyGutter).append(blue).append(" | ").append(reset)
                  .append(repeat(' ', startCol - 1))
                  .append(markerColor).append(repeat(underlineChar, caretLen));
                if (!sp.inlineLabel.isEmpty()) {
                    sb.append(' ').append(sp.inlineLabel);
                }
                sb.append(reset).append('\n');
            }
        }

        for (String help : rec.helpNotes) {
            sb.append(emptyGutter).append(blue).append(" = ").append(reset)
              .append(bold).append("help: ").append(reset)
              .append(help).append('\n');
        }

        if (rec.quickFix != null) {
            QuickFixSuggestion qf = rec.quickFix;
            String modeTag = qf.autoApplied ? " [AUTO-APPLIED BY JLC]" : " [Bunu soyle duzeltmek ister misiniz? (QuickFix)]";
            sb.append(emptyGutter).append(blue).append(" = ").append(reset)
              .append(green).append("quickfix: ").append(reset)
              .append(qf.title).append(modeTag).append('\n');

            String lineNumStr = padLeft(Integer.toString(qf.targetLine), gutterWidth);
            if (!qf.originalCode.isEmpty()) {
                sb.append(red).append(lineNumStr).append(" - ").append(qf.originalCode.trim()).append(reset).append('\n');
            }
            for (String repLine : qf.replacementCode.split("\n")) {
                sb.append(green).append(lineNumStr).append(" + ").append(repLine.trim()).append(reset).append('\n');
            }
        }

        return sb.toString();
    }

    /**
     * Renders all accumulated diagnostics and summary footer to the target stream.
     */
    public void flushTo(PrintStream out, SourceFileBuffer source) {
        if (out == null || records.isEmpty()) {
            return;
        }
        for (DiagnosticRecord rec : records) {
            out.print(formatRecord(rec, source));
            out.println();
        }
        int errors = countBySeverity(Severity.ERROR);
        int shields = countBySeverity(Severity.SHIELD);
        int warnings = countBySeverity(Severity.WARNING);
        if (errors > 0 || shields > 0 || warnings > 0) {
            String bold = ansiColorEnabled ? ANSI_BOLD : "";
            String reset = ansiColorEnabled ? ANSI_RESET : "";
            out.println(bold + "[jlc diagnostics] summary: " + errors + " error(s), "
                    + shields + " safety-shield lock(s), "
                    + warnings + " warning(s) emitted." + reset);
        }
    }

    private static String expandTabs(String s) {
        if (s == null || s.indexOf('\t') < 0) {
            return (s != null) ? s : "";
        }
        return s.replace("\t", "    ");
    }

    private static String padLeft(String s, int width) {
        if (s.length() >= width) {
            return s;
        }
        return repeat(' ', width - s.length()) + s;
    }

    private static String repeat(char c, int count) {
        if (count <= 0) {
            return "";
        }
        char[] buf = new char[count];
        Arrays.fill(buf, c);
        return new String(buf);
    }
}
