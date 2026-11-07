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

import com.sun.tools.javac.bootstrap.PJCF;
import com.sun.tools.javac.bootstrap.PJCF.MethodStatement;
import com.sun.tools.javac.bootstrap.PJCF.PreVacSorter;
import com.sun.tools.javac.bootstrap.PJCF.StatementKind;
import com.sun.tools.javac.bootstrap.PJCF.TemporalBranchTable;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Deque;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;
import java.util.jar.JarOutputStream;

/**
 * ============================================================================
 * JLCDecompilerFrontend — Legacy Bytecode Decompiler & 0xCAFEF00D Synthesizer
 * ============================================================================
 * Bridges legacy OpenJDK / third-party classfiles (0xCAFEBABE, Java 1.1..25)
 * into the OneJDK-25 FCS canonical classfile format:
 *   - Magic:         0xCAFEF00D
 *   - Major Version: 74
 *   - Minor Version: 1
 *   - Modern Tag:    0x20262027 (FCS_ModernTag attribute)
 *
 * Pipeline:
 * 1. Checks for optional '_JLCFrontend.java' companion rules in package or
 *    META-INF/jlc/ (allowing library authors to declare semantic barriers,
 *    strict IEEE-754 methods, or @RJGC policies).
 * 2. Parses all 17 JVMS Constant Pool tags, fields, methods, and Code blocks.
 * 3. Lifts stack opcodes into symbolic PJCF.MethodStatement blocks, orders
 *    them via PJCF.PreVacSorter, synthesizes 8-WIM FCS_TBT branch tables,
 *    detects System.gc() preflight clean-hint sites, and seals a modern
 *    0xCAFEF00D binary ready for zero-CompileBroker VAC ingestion!
 * ============================================================================
 */
public final class JLCDecompilerFrontend {

    public static final int LEGACY_JAVA_MAGIC    = 0xCAFEBABE;
    public static final int ONEJDK_CLASS_MAGIC   = 0xCAFEF00D;
    public static final int ONEJDK_MAJOR_VERSION = 74;
    public static final int ONEJDK_MINOR_VERSION = 1;
    public static final int HAS_MODERN_JLC_TAG   = 0x20262027;
    public static final String FRONTEND_HOOK_SUFFIX = "_JLCFrontend.java";

    /**
     * Parsed directives from an optional *_JLCFrontend.java companion file.
     */
    public static final class FrontendDirectiveProfile {
        public final boolean preserveStrictOrder;
        public final boolean forceIeeeExact;
        public final String defaultRjgcMark;
        public final Set<String> semanticBarrierMethods;

        public FrontendDirectiveProfile(boolean preserveStrictOrder,
                                        boolean forceIeeeExact,
                                        String defaultRjgcMark,
                                        Set<String> semanticBarrierMethods) {
            this.preserveStrictOrder = preserveStrictOrder;
            this.forceIeeeExact = forceIeeeExact;
            this.defaultRjgcMark = (defaultRjgcMark != null) ? defaultRjgcMark : "ImmediateEviction";
            this.semanticBarrierMethods = Collections.unmodifiableSet(new LinkedHashSet<>(semanticBarrierMethods));
        }

        public static FrontendDirectiveProfile defaults() {
            return new FrontendDirectiveProfile(false, false, "ImmediateEviction", Collections.emptySet());
        }
    }

    /**
     * Summary returned after re-synthesizing a legacy classfile or JAR archive.
     */
    public static final class ResynthesisResult {
        public final byte[] modernClassBytes;
        public final String internalClassName;
        public final int originalMajorVersion;
        public final int methodsProcessed;
        public final int tbtBranchesSynthesized;
        public final int explicitGcPreflightsFound;
        public final boolean usedFrontendHook;

        public ResynthesisResult(byte[] modernClassBytes,
                                 String internalClassName,
                                 int originalMajorVersion,
                                 int methodsProcessed,
                                 int tbtBranchesSynthesized,
                                 int explicitGcPreflightsFound,
                                 boolean usedFrontendHook) {
            this.modernClassBytes = modernClassBytes;
            this.internalClassName = internalClassName;
            this.originalMajorVersion = originalMajorVersion;
            this.methodsProcessed = methodsProcessed;
            this.tbtBranchesSynthesized = tbtBranchesSynthesized;
            this.explicitGcPreflightsFound = explicitGcPreflightsFound;
            this.usedFrontendHook = usedFrontendHook;
        }
    }

    private static final class RawCpEntry {
        final int tag;
        final byte[] rawBytes;
        final String utf8Value;
        final int ref1;
        final int ref2;

        RawCpEntry(int tag, byte[] rawBytes, String utf8Value, int ref1, int ref2) {
            this.tag = tag;
            this.rawBytes = rawBytes;
            this.utf8Value = utf8Value;
            this.ref1 = ref1;
            this.ref2 = ref2;
        }
    }

    /**
     * Parses optional *_JLCFrontend.java source text for package/archive rules.
     */
    public FrontendDirectiveProfile parseFrontendHook(String hookSource) {
        if (hookSource == null || hookSource.isEmpty()) {
            return FrontendDirectiveProfile.defaults();
        }
        boolean strictOrder = hookSource.contains("@JLCPreserveOrder") || hookSource.contains("preserveOrder = true");
        boolean forceExact = hookSource.contains("IEEE_EXACT") || hookSource.contains("forceIeeeExact = true");
        String rjgc = hookSource.contains("ContinuousFlowWatch") ? "ContinuousFlowWatch" : "ImmediateEviction";
        Set<String> barrierMethods = new LinkedHashSet<>();
        for (String rawLine : hookSource.split("\n")) {
            String line = rawLine.trim();
            int idx = line.indexOf("semanticBarrier(\"");
            if (idx >= 0) {
                int start = idx + "semanticBarrier(\"".length();
                int end = line.indexOf('"', start);
                if (end > start) {
                    barrierMethods.add(line.substring(start, end));
                }
            }
        }
        return new FrontendDirectiveProfile(strictOrder, forceExact, rjgc, barrierMethods);
    }

    /**
     * Resolves a companion <ClassName>_JLCFrontend.java or package _JLCFrontend.java if present.
     */
    public FrontendDirectiveProfile discoverFrontendHook(Path classOrSourcePath) {
        if (classOrSourcePath == null) {
            return FrontendDirectiveProfile.defaults();
        }
        Path parent = classOrSourcePath.getParent();
        if (parent == null) {
            return FrontendDirectiveProfile.defaults();
        }
        String fileName = classOrSourcePath.getFileName().toString();
        int dot = fileName.lastIndexOf('.');
        String stem = (dot > 0) ? fileName.substring(0, dot) : fileName;

        Path specificHook = parent.resolve(stem + FRONTEND_HOOK_SUFFIX);
        Path packageHook = parent.resolve(FRONTEND_HOOK_SUFFIX);
        try {
            if (Files.isRegularFile(specificHook)) {
                return parseFrontendHook(Files.readString(specificHook, StandardCharsets.UTF_8));
            }
            if (Files.isRegularFile(packageHook)) {
                return parseFrontendHook(Files.readString(packageHook, StandardCharsets.UTF_8));
            }
        } catch (IOException ignored) {
            // Fallback cleanly to default profile if filesystem read fails
        }
        return FrontendDirectiveProfile.defaults();
    }

    /**
     * Ingests a legacy 0xCAFEBABE (or already-modern 0xCAFEF00D) class binary,
     * decompiles its method bodies for Pre-VAC canonical analysis & FCS_TBT
     * synthesis, and re-emits a sealed OneJDK 0xCAFEF00D (v74.1) classfile.
     */
    public ResynthesisResult resynthesizeClassBytes(byte[] inputBytes,
                                                    FrontendDirectiveProfile profile,
                                                     boolean usedHook) throws IOException {
        if (inputBytes == null || inputBytes.length < 24) {
            throw new IOException("Invalid classfile buffer: length < 24 bytes");
        }
        FrontendDirectiveProfile activeProfile = (profile != null) ? profile : FrontendDirectiveProfile.defaults();
        DataInputStream dis = new DataInputStream(new ByteArrayInputStream(inputBytes));

        int magic = dis.readInt();
        if (magic != LEGACY_JAVA_MAGIC && magic != ONEJDK_CLASS_MAGIC) {
            throw new IOException(String.format("Unsupported classfile magic: 0x%08X", magic));
        }
        int origMinor = dis.readUnsignedShort();
        int origMajor = dis.readUnsignedShort();

        // 1. Parse Constant Pool (preserving exact original indices + appending FCS attributes)
        int cpCount = dis.readUnsignedShort();
        RawCpEntry[] cp = new RawCpEntry[cpCount + 16];
        List<byte[]> serializedCpEntries = new ArrayList<>(cpCount + 8);

        for (int i = 1; i < cpCount; i++) {
            int tag = dis.readUnsignedByte();
            RawCpEntry entry = readConstantPoolEntry(tag, dis);
            cp[i] = entry;
            serializedCpEntries.add(entry.rawBytes);
            if (tag == 5 || tag == 6) {
                // JVMS §4.4.5: Long and Double take two constant pool slots
                i++;
            }
        }

        int accessFlags = dis.readUnsignedShort();
        int thisClassIdx = dis.readUnsignedShort();
        int superClassIdx = dis.readUnsignedShort();
        String internalClassName = resolveClassName(cp, thisClassIdx);

        int interfacesCount = dis.readUnsignedShort();
        int[] interfaces = new int[interfacesCount];
        for (int i = 0; i < interfacesCount; i++) {
            interfaces[i] = dis.readUnsignedShort();
        }

        // 2. Read Fields verbatim
        int fieldsCount = dis.readUnsignedShort();
        byte[] fieldsBlob = readMemberTableVerbatim(dis, fieldsCount);

        // 3. Read & Decompile Methods to extract FCS_TBT & System.gc() preflight metadata
        int methodsCount = dis.readUnsignedShort();
        ByteArrayOutputStream methodsBos = new ByteArrayOutputStream();
        DataOutputStream methodsDos = new DataOutputStream(methodsBos);
        TemporalBranchTable classTbt = new TemporalBranchTable();
        List<JLCStaticAnalyzer.PreflightCleanHintSite> preflightHints = new ArrayList<>();

        for (int m = 0; m < methodsCount; m++) {
            int mFlags = dis.readUnsignedShort();
            int mNameIdx = dis.readUnsignedShort();
            int mDescIdx = dis.readUnsignedShort();
            int mAttrCount = dis.readUnsignedShort();
            String methodName = resolveUtf8(cp, mNameIdx);

            methodsDos.writeShort(mFlags);
            methodsDos.writeShort(mNameIdx);
            methodsDos.writeShort(mDescIdx);
            methodsDos.writeShort(mAttrCount);

            for (int a = 0; a < mAttrCount; a++) {
                int aNameIdx = dis.readUnsignedShort();
                int aLen = dis.readInt();
                byte[] aInfo = new byte[aLen];
                dis.readFully(aInfo);

                String attrName = resolveUtf8(cp, aNameIdx);
                if ("Code".equals(attrName) && aInfo.length >= 12) {
                    analyzeAndLiftCodeAttribute(
                            methodName,
                            aInfo,
                            cp,
                            activeProfile,
                            classTbt,
                            preflightHints);
                }

                methodsDos.writeShort(aNameIdx);
                methodsDos.writeInt(aLen);
                methodsDos.write(aInfo);
            }
        }
        methodsDos.flush();

        // 4. Read existing Class-level Attributes (filtering old FCS_ModernTag/FCS_TBT if re-sealing)
        int classAttrCount = dis.readUnsignedShort();
        List<byte[]> retainedClassAttrs = new ArrayList<>(classAttrCount + 4);
        for (int a = 0; a < classAttrCount; a++) {
            int aNameIdx = dis.readUnsignedShort();
            int aLen = dis.readInt();
            byte[] aInfo = new byte[aLen];
            dis.readFully(aInfo);
            String attrName = resolveUtf8(cp, aNameIdx);
            if (!JLCDebugSymbolEmitter.ATTR_FCS_MODERN_TAG.equals(attrName)
                    && !PJCF.ATTR_TEMPORAL_BRANCH_TABLE.equals(attrName)
                    && !JLCDebugSymbolEmitter.ATTR_FCS_PREFLIGHT_GC_HINT.equals(attrName)) {
                retainedClassAttrs.add(JLCDebugSymbolEmitter.wrapAttribute(aNameIdx, aInfo));
            }
        }

        // 5. Append FCS Constant Pool UTF8 entries & synthesize OneJDK v74.1 attributes
        int nextCpSlot = cpCount;
        Map<String, Integer> appendedUtf8 = new HashMap<>();

        int modernTagNameIdx = appendUtf8(serializedCpEntries, appendedUtf8, nextCpSlot, JLCDebugSymbolEmitter.ATTR_FCS_MODERN_TAG);
        nextCpSlot = cpCount + appendedUtf8.size();
        ByteArrayOutputStream tagPayload = new ByteArrayOutputStream(8);
        DataOutputStream tagDos = new DataOutputStream(tagPayload);
        tagDos.writeInt(HAS_MODERN_JLC_TAG);
        tagDos.writeShort(origMajor);
        tagDos.writeShort(origMinor);
        tagDos.flush();
        retainedClassAttrs.add(JLCDebugSymbolEmitter.wrapAttribute(modernTagNameIdx, tagPayload.toByteArray()));

        if (!classTbt.entries.isEmpty()) {
            int tbtNameIdx = appendUtf8(serializedCpEntries, appendedUtf8, nextCpSlot, PJCF.ATTR_TEMPORAL_BRANCH_TABLE);
            nextCpSlot = cpCount + appendedUtf8.size();
            retainedClassAttrs.add(JLCDebugSymbolEmitter.wrapAttribute(tbtNameIdx, classTbt.toAttributeBytes()));
        }

        if (!preflightHints.isEmpty()) {
            int pfNameIdx = appendUtf8(serializedCpEntries, appendedUtf8, nextCpSlot, JLCDebugSymbolEmitter.ATTR_FCS_PREFLIGHT_GC_HINT);
            nextCpSlot = cpCount + appendedUtf8.size();
            ByteArrayOutputStream pfPayload = new ByteArrayOutputStream(2 + preflightHints.size() * 6);
            DataOutputStream pfDos = new DataOutputStream(pfPayload);
            pfDos.writeShort(preflightHints.size());
            for (JLCStaticAnalyzer.PreflightCleanHintSite h : preflightHints) {
                int mUtfIdx = appendUtf8(serializedCpEntries, appendedUtf8, nextCpSlot, h.methodName);
                nextCpSlot = cpCount + appendedUtf8.size();
                pfDos.writeShort(mUtfIdx);
                pfDos.writeShort(h.preflightInsertLine & 0xFFFF);
                pfDos.writeShort(h.explicitGcLine & 0xFFFF);
            }
            pfDos.flush();
            retainedClassAttrs.add(JLCDebugSymbolEmitter.wrapAttribute(pfNameIdx, pfPayload.toByteArray()));
        }

        // 6. Emit sealed 0xCAFEF00D v74.1 ClassFile binary
        int finalCpCount = cpCount + appendedUtf8.size();
        ByteArrayOutputStream outBos = new ByteArrayOutputStream(inputBytes.length + 256);
        DataOutputStream outDos = new DataOutputStream(outBos);

        outDos.writeInt(ONEJDK_CLASS_MAGIC);
        outDos.writeShort(ONEJDK_MINOR_VERSION);
        outDos.writeShort(ONEJDK_MAJOR_VERSION);
        outDos.writeShort(finalCpCount);
        for (byte[] cpBytes : serializedCpEntries) {
            outDos.write(cpBytes);
        }

        outDos.writeShort(accessFlags);
        outDos.writeShort(thisClassIdx);
        outDos.writeShort(superClassIdx);
        outDos.writeShort(interfacesCount);
        for (int iface : interfaces) {
            outDos.writeShort(iface);
        }

        outDos.writeShort(fieldsCount);
        outDos.write(fieldsBlob);

        outDos.writeShort(methodsCount);
        outDos.write(methodsBos.toByteArray());

        outDos.writeShort(retainedClassAttrs.size());
        for (byte[] attr : retainedClassAttrs) {
            outDos.write(attr);
        }
        outDos.flush();

        return new ResynthesisResult(
                outBos.toByteArray(),
                internalClassName,
                origMajor,
                methodsCount,
                classTbt.entries.size(),
                preflightHints.size(),
                usedHook);
    }

    /**
     * Transforms an entire legacy .jar archive into a modern OneJDK 0xCAFEF00D .jar archive.
     */
    public int resynthesizeJarArchive(Path inputJar, Path outputJar) throws IOException {
        int transformedClasses = 0;
        FrontendDirectiveProfile archiveProfile = FrontendDirectiveProfile.defaults();

        try (JarFile jar = new JarFile(inputJar.toFile())) {
            JarEntry hookEntry = jar.getJarEntry("META-INF/jlc/" + FRONTEND_HOOK_SUFFIX);
            if (hookEntry != null) {
                try (InputStream is = jar.getInputStream(hookEntry)) {
                    String hookText = new String(is.readAllBytes(), StandardCharsets.UTF_8);
                    archiveProfile = parseFrontendHook(hookText);
                }
            }

            try (JarOutputStream jos = new JarOutputStream(Files.newOutputStream(outputJar))) {
                Enumeration<JarEntry> entries = jar.entries();
                while (entries.hasMoreElements()) {
                    JarEntry entry = entries.nextElement();
                    byte[] raw;
                    try (InputStream is = jar.getInputStream(entry)) {
                        raw = is.readAllBytes();
                    }

                    JarEntry outEntry = new JarEntry(entry.getName());
                    outEntry.setTime(entry.getTime());
                    jos.putNextEntry(outEntry);

                    if (!entry.isDirectory() && entry.getName().endsWith(".class") && raw.length >= 24) {
                        ResynthesisResult res = resynthesizeClassBytes(raw, archiveProfile, hookEntry != null);
                        jos.write(res.modernClassBytes);
                        transformedClasses++;
                    } else {
                        jos.write(raw);
                    }
                    jos.closeEntry();
                }
            }
        }
        return transformedClasses;
    }

    /**
     * Lifts raw JVM stack opcodes into symbolic PJCF.MethodStatement blocks,
     * orders them via PreVacSorter, extracts 8-WIM TBT branch entries, and
     * detects System.gc() / Runtime.gc() preflight clean-hint PCs.
     */
    private void analyzeAndLiftCodeAttribute(String methodName,
                                             byte[] codeAttrInfo,
                                             RawCpEntry[] cp,
                                             FrontendDirectiveProfile profile,
                                             TemporalBranchTable classTbt,
                                             List<JLCStaticAnalyzer.PreflightCleanHintSite> preflightHints) throws IOException {
        DataInputStream dis = new DataInputStream(new ByteArrayInputStream(codeAttrInfo));
        dis.readUnsignedShort(); // max_stack
        dis.readUnsignedShort(); // max_locals
        int codeLen = dis.readInt();
        if (codeLen <= 0 || codeLen > codeAttrInfo.length - 8) {
            return;
        }
        byte[] code = new byte[codeLen];
        dis.readFully(code);

        Deque<String> symStack = new ArrayDeque<>();
        List<MethodStatement> lifted = new ArrayList<>();
        boolean isBarrierMethod = profile.preserveStrictOrder || profile.semanticBarrierMethods.contains(methodName);

        int pc = 0;
        while (pc < codeLen) {
            int opcode = code[pc] & 0xFF;
            int insnPc = pc;
            pc++;

            switch (opcode) {
                case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: // iconst_0..5
                    symStack.push(Integer.toString(opcode - 0x03));
                    break;
                case 0x10: // bipush
                    if (pc < codeLen) {
                        symStack.push(Integer.toString(code[pc++]));
                    }
                    break;
                case 0x11: // sipush
                    if (pc + 1 < codeLen) {
                        short val = (short) (((code[pc] & 0xFF) << 8) | (code[pc + 1] & 0xFF));
                        pc += 2;
                        symStack.push(Short.toString(val));
                    }
                    break;
                case 0x12: // ldc
                    if (pc < codeLen) {
                        int idx = code[pc++] & 0xFF;
                        symStack.push("const#" + idx);
                    }
                    break;
                case 0x13: // ldc_w
                case 0x14: // ldc2_w
                    if (pc + 1 < codeLen) {
                        int idx = ((code[pc] & 0xFF) << 8) | (code[pc + 1] & 0xFF);
                        pc += 2;
                        symStack.push("const#" + idx);
                    }
                    break;
                case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: // iload..aload
                    if (pc < codeLen) {
                        int slot = code[pc++] & 0xFF;
                        symStack.push("local_" + slot);
                    }
                    break;
                case 0x1A: case 0x1B: case 0x1C: case 0x1D: // iload_0..3
                    symStack.push("local_" + (opcode - 0x1A));
                    break;
                case 0x2A: case 0x2B: case 0x2C: case 0x2D: // aload_0..3
                    symStack.push("local_" + (opcode - 0x2A));
                    break;
                case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: // istore..astore
                    if (pc < codeLen) {
                        int slot = code[pc++] & 0xFF;
                        String rhs = symStack.isEmpty() ? "stackTop" : symStack.pop();
                        String varName = "local_" + slot;
                        lifted.add(new MethodStatement(
                                isBarrierMethod ? StatementKind.SEMANTIC_BARRIER : StatementKind.DEFINITION,
                                varName,
                                varName + " = " + rhs));
                    }
                    break;
                case 0x3B: case 0x3C: case 0x3D: case 0x3E: // istore_0..3
                case 0x4B: case 0x4C: case 0x4D: case 0x4E: { // astore_0..3
                    int slot = (opcode >= 0x4B) ? (opcode - 0x4B) : (opcode - 0x3B);
                    String rhs = symStack.isEmpty() ? "stackTop" : symStack.pop();
                    String varName = "local_" + slot;
                    lifted.add(new MethodStatement(
                            isBarrierMethod ? StatementKind.SEMANTIC_BARRIER : StatementKind.DEFINITION,
                            varName,
                            varName + " = " + rhs));
                    break;
                }
                case 0x60: case 0x61: case 0x62: case 0x63: // add
                case 0x68: case 0x69: case 0x6A: case 0x6B: { // mul
                    String b = symStack.isEmpty() ? "r1" : symStack.pop();
                    String a = symStack.isEmpty() ? "r0" : symStack.pop();
                    String op = (opcode <= 0x63) ? " + " : " * ";
                    symStack.push("(" + a + op + b + ")");
                    break;
                }
                case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: // if<cond>
                case 0x9F: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: // if_icmp<cond>
                case 0xC6: case 0xC7: { // ifnull / ifnonnull
                    if (pc + 1 < codeLen) {
                        short rel = (short) (((code[pc] & 0xFF) << 8) | (code[pc + 1] & 0xFF));
                        pc += 2;
                        int targetPc = insnPc + rel;
                        String cond = symStack.isEmpty() ? ("pc_" + insnPc) : symStack.pop();
                        boolean backwardLoopEdge = targetPc < insnPc;
                        int bpuHint = backwardLoopEdge ? 1 : 2;
                        classTbt.register8WayMatrix(
                                cond + " => pc_" + targetPc,
                                cond + " => fallthrough_" + pc,
                                "!(" + cond + ") => pc_" + targetPc,
                                "!(" + cond + ") => fallthrough_" + pc,
                                "pc_" + targetPc + " => " + cond,
                                "pc_" + targetPc + " => !(" + cond + ")",
                                "fallthrough_" + pc + " => " + cond,
                                "fallthrough_" + pc + " => !(" + cond + ")",
                                "/* bytecode_rollback_pc=" + insnPc + " */",
                                bpuHint);
                        lifted.add(new MethodStatement(
                                backwardLoopEdge ? StatementKind.PILF_LOOP : StatementKind.EXECUTION,
                                "",
                                backwardLoopEdge ? ("while (" + cond + ") { /* loop */ }") : ("if (" + cond + ")")));
                    }
                    break;
                }
                case 0xA7: // goto
                case 0xA8: // jsr
                case 0xB2: // getstatic
                case 0xB3: // putstatic
                case 0xB4: // getfield
                case 0xB5: // putfield
                case 0xBB: // new
                case 0xBD: // anewarray
                case 0xC0: // checkcast
                case 0xC1: // instanceof
                    pc = Math.min(codeLen, pc + 2);
                    break;
                case 0xB6: // invokevirtual
                case 0xB7: // invokespecial
                case 0xB8: { // invokestatic
                    if (pc + 1 < codeLen) {
                        int refIdx = ((code[pc] & 0xFF) << 8) | (code[pc + 1] & 0xFF);
                        pc += 2;
                        String callee = resolveMethodRef(cp, refIdx);
                        if ("java/lang/System.gc:()V".equals(callee)
                                || "java/lang/Runtime.gc:()V".equals(callee)) {
                            int preflightPc = Math.max(0, insnPc - 12);
                            preflightHints.add(new JLCStaticAnalyzer.PreflightCleanHintSite(
                                    methodName, insnPc, preflightPc, 4));
                        }
                        lifted.add(new MethodStatement(StatementKind.EXECUTION, "", "invoke " + callee));
                    }
                    break;
                }
                case 0xB9: // invokeinterface
                case 0xBA: // invokedynamic
                case 0xC8: // goto_w
                case 0xC9: // jsr_w
                    pc = Math.min(codeLen, pc + 4);
                    break;
                case 0x84: // iinc
                    pc = Math.min(codeLen, pc + 2);
                    break;
                case 0xBC: // newarray
                case 0x19 + 0xF0: // unused guard
                    pc = Math.min(codeLen, pc + 1);
                    break;
                case 0xC5: // multianewarray
                    pc = Math.min(codeLen, pc + 3);
                    break;
                case 0xAA: { // tableswitch
                    pc = (pc + 3) & ~3;
                    if (pc + 12 <= codeLen) {
                        int low = readInt(code, pc + 4);
                        int high = readInt(code, pc + 8);
                        int jumpCount = Math.max(0, high - low + 1);
                        pc = Math.min(codeLen, pc + 12 + jumpCount * 4);
                    } else {
                        pc = codeLen;
                    }
                    break;
                }
                case 0xAB: { // lookupswitch
                    pc = (pc + 3) & ~3;
                    if (pc + 8 <= codeLen) {
                        int npairs = Math.max(0, readInt(code, pc + 4));
                        pc = Math.min(codeLen, pc + 8 + npairs * 8);
                    } else {
                        pc = codeLen;
                    }
                    break;
                }
                case 0xC4: { // wide
                    if (pc < codeLen) {
                        int wideOp = code[pc++] & 0xFF;
                        pc = Math.min(codeLen, pc + ((wideOp == 0x84) ? 4 : 2));
                    }
                    break;
                }
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: case 0xB0: case 0xB1: { // *return
                    String retVal = symStack.isEmpty() ? "" : symStack.pop();
                    lifted.add(new MethodStatement(StatementKind.STORE_RETURN, "", "return " + retVal + ";"));
                    break;
                }
                default:
                    // Single-byte zero-immediate instruction
                    break;
            }
        }

        if (!isBarrierMethod && !lifted.isEmpty()) {
            PreVacSorter.sortAndClean(lifted);
        }
    }

    private static RawCpEntry readConstantPoolEntry(int tag, DataInputStream dis) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream(16);
        DataOutputStream dos = new DataOutputStream(bos);
        dos.writeByte(tag);

        String utf8 = null;
        int ref1 = 0;
        int ref2 = 0;

        switch (tag) {
            case 1: { // CONSTANT_Utf8
                int len = dis.readUnsignedShort();
                byte[] bytes = new byte[len];
                dis.readFully(bytes);
                dos.writeShort(len);
                dos.write(bytes);
                utf8 = new String(bytes, StandardCharsets.UTF_8);
                break;
            }
            case 3: // Integer
            case 4: { // Float
                int v = dis.readInt();
                dos.writeInt(v);
                break;
            }
            case 5: // Long
            case 6: { // Double
                long v = dis.readLong();
                dos.writeLong(v);
                break;
            }
            case 7: // Class
            case 8: // String
            case 16: // MethodType
            case 19: // Module
            case 20: { // Package
                ref1 = dis.readUnsignedShort();
                dos.writeShort(ref1);
                break;
            }
            case 9: // Fieldref
            case 10: // Methodref
            case 11: // InterfaceMethodref
            case 12: // NameAndType
            case 17: // Dynamic
            case 18: { // InvokeDynamic
                ref1 = dis.readUnsignedShort();
                ref2 = dis.readUnsignedShort();
                dos.writeShort(ref1);
                dos.writeShort(ref2);
                break;
            }
            case 15: { // MethodHandle
                int kind = dis.readUnsignedByte();
                ref1 = dis.readUnsignedShort();
                dos.writeByte(kind);
                dos.writeShort(ref1);
                break;
            }
            default:
                throw new IOException("Unknown JVMS constant pool tag: " + tag);
        }
        dos.flush();
        return new RawCpEntry(tag, bos.toByteArray(), utf8, ref1, ref2);
    }

    private static byte[] readMemberTableVerbatim(DataInputStream dis, int count) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream(count * 16);
        DataOutputStream dos = new DataOutputStream(bos);
        for (int i = 0; i < count; i++) {
            dos.writeShort(dis.readUnsignedShort()); // access_flags
            dos.writeShort(dis.readUnsignedShort()); // name_index
            dos.writeShort(dis.readUnsignedShort()); // descriptor_index
            int attrCount = dis.readUnsignedShort();
            dos.writeShort(attrCount);
            for (int a = 0; a < attrCount; a++) {
                dos.writeShort(dis.readUnsignedShort());
                int len = dis.readInt();
                byte[] info = new byte[len];
                dis.readFully(info);
                dos.writeInt(len);
                dos.write(info);
            }
        }
        dos.flush();
        return bos.toByteArray();
    }

    private static int appendUtf8(List<byte[]> serializedCp,
                                  Map<String, Integer> appendedMap,
                                  int nextSlotBase,
                                  String text) throws IOException {
        Integer existing = appendedMap.get(text);
        if (existing != null) {
            return existing;
        }
        byte[] utf = text.getBytes(StandardCharsets.UTF_8);
        ByteArrayOutputStream bos = new ByteArrayOutputStream(3 + utf.length);
        DataOutputStream dos = new DataOutputStream(bos);
        dos.writeByte(1);
        dos.writeShort(utf.length);
        dos.write(utf);
        dos.flush();
        serializedCp.add(bos.toByteArray());
        int assignedIdx = nextSlotBase;
        appendedMap.put(text, assignedIdx);
        return assignedIdx;
    }

    private static String resolveUtf8(RawCpEntry[] cp, int idx) {
        if (idx > 0 && idx < cp.length && cp[idx] != null && cp[idx].utf8Value != null) {
            return cp[idx].utf8Value;
        }
        return "";
    }

    private static String resolveClassName(RawCpEntry[] cp, int classIdx) {
        if (classIdx > 0 && classIdx < cp.length && cp[classIdx] != null && cp[classIdx].tag == 7) {
            return resolveUtf8(cp, cp[classIdx].ref1);
        }
        return "<unknown>";
    }

    private static String resolveMethodRef(RawCpEntry[] cp, int refIdx) {
        if (refIdx > 0 && refIdx < cp.length && cp[refIdx] != null) {
            RawCpEntry ref = cp[refIdx];
            if (ref.tag == 10 || ref.tag == 11) {
                String owner = resolveClassName(cp, ref.ref1);
                if (ref.ref2 > 0 && ref.ref2 < cp.length && cp[ref.ref2] != null && cp[ref.ref2].tag == 12) {
                    RawCpEntry nat = cp[ref.ref2];
                    return owner + "." + resolveUtf8(cp, nat.ref1) + ":" + resolveUtf8(cp, nat.ref2);
                }
            }
        }
        return "<method#" + refIdx + ">";
    }

    private static int readInt(byte[] buf, int off) {
        return ((buf[off] & 0xFF) << 24)
             | ((buf[off + 1] & 0xFF) << 16)
             | ((buf[off + 2] & 0xFF) << 8)
             | (buf[off + 3] & 0xFF);
    }
}
