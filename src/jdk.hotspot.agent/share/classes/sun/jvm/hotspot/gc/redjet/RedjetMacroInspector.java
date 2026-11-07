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

package sun.jvm.hotspot.gc.redjet;

import java.io.PrintStream;
import sun.jvm.hotspot.debugger.Address;
import sun.jvm.hotspot.runtime.VM;
import sun.jvm.hotspot.types.AddressField;
import sun.jvm.hotspot.types.CIntegerField;
import sun.jvm.hotspot.types.Type;
import sun.jvm.hotspot.types.TypeDataBase;

/**
 * ============================================================================
 * RedjetMacroInspector — Serviceability Agent Macro Subsystem Inspector
 * ============================================================================
 * Exposes RedjetGC's entire macro subsystem constellation to jhsdb / clhsdb:
 *   1. Active VM Flags & Scheduler Mode (Domestic DSP vs. TBB Mutual Exclusion)
 *   2. 4-Season Coordinator (Spring / Summer / Autumn / Winter + Accordion Depth)
 *   3. VFS Logical Memory Partitioning (JAVA_MEMORY vs. HOTSPOT_MEMORY)
 *   4. Isolated Compressed Spaces (HeaderPane narrowKlass & isolated code_boot)
 *   5. Dynamic SIMD Contention Governor & BPU TAGE/Perceptron Director
 *   6. 2-Points Hardware Coordination (Dockpoint & Parkpoint)
 *   7. TailTraces RPSS, @RJGC Off-Heap Reclaimer, and Fast JNI/FFM Bridge
 * ============================================================================
 */
public final class RedjetMacroInspector {

  private static AddressField globalsInstanceField;
  private static AddressField schedulerFlagPtrField;
  private static AddressField strictVfsPtrField;
  private static AddressField rmrpPtrField;
  private static AddressField deadRefSignalsPtrField;
  private static AddressField explicitCleanHintsPtrField;
  private static AddressField autoScalarContentionPtrField;
  private static AddressField criticalSimdOnlyPtrField;
  private static CIntegerField schedulerActiveCoresField;

  static {
    VM.registerVMInitializedObserver((o, data) -> initialize(VM.getVM().getTypeDataBase()));
  }

  private static synchronized void initialize(TypeDataBase db) {
    try {
      Type globalsType = db.lookupType("RedjetGlobalsForVMStructs");
      globalsInstanceField = globalsType.getAddressField("_instance_p");
      schedulerFlagPtrField = globalsType.getAddressField("_RedjetGCScheduler");
      strictVfsPtrField = globalsType.getAddressField("_RedjetGCStrictVFS");
      rmrpPtrField = globalsType.getAddressField("_RedjetGCRMRP");
      deadRefSignalsPtrField = globalsType.getAddressField("_RedjetGCUseDeadReferenceSignals");
      explicitCleanHintsPtrField = globalsType.getAddressField("_RedjetGCInvokesExplicitCleanHints");
      autoScalarContentionPtrField = globalsType.getAddressField("_RedjetGCAutoScalarOnSIMDContention");
      criticalSimdOnlyPtrField = globalsType.getAddressField("_RedjetGCCriticalSIMDOnly");

      Type schedType = db.lookupType("RedjetBareMetalScheduler");
      schedulerActiveCoresField = schedType.getCIntegerField("_active_cores");
    } catch (RuntimeException ignored) {
      // Target VM may be running with a non-Redjet collector
    }
  }

  public static final class MacroFlagSnapshot {
    public String  schedulerMode = "Domestic";
    public boolean strictVfs = true;
    public boolean rmrpEnabled = true;
    public boolean tailTracesSignals = true;
    public boolean explicitCleanHints = true;
    public boolean autoScalarOnSimdContention = true;
    public boolean criticalSimdOnly = false;
  }

  public static MacroFlagSnapshot readLiveFlags() {
    MacroFlagSnapshot snap = new MacroFlagSnapshot();
    if (globalsInstanceField == null) {
      return snap;
    }
    try {
      Address inst = globalsInstanceField.getValue();
      if (inst == null) {
        return snap;
      }
      snap.strictVfs = readBoolPtr(inst, strictVfsPtrField, true);
      snap.rmrpEnabled = readBoolPtr(inst, rmrpPtrField, true);
      snap.tailTracesSignals = readBoolPtr(inst, deadRefSignalsPtrField, true);
      snap.explicitCleanHints = readBoolPtr(inst, explicitCleanHintsPtrField, true);
      snap.autoScalarOnSimdContention = readBoolPtr(inst, autoScalarContentionPtrField, true);
      snap.criticalSimdOnly = readBoolPtr(inst, criticalSimdOnlyPtrField, false);

      if (schedulerFlagPtrField != null) {
        Address strPtrAddr = schedulerFlagPtrField.getValue(inst);
        if (strPtrAddr != null) {
          Address cstr = strPtrAddr.getAddressAt(0);
          if (cstr != null) {
            snap.schedulerMode = readCString(cstr, 32);
          }
        }
      }
    } catch (RuntimeException ignored) {
      // Best-effort debugger memory read
    }
    return snap;
  }

  public static void printMacroSummaryOn(PrintStream tty) {
    MacroFlagSnapshot flags = readLiveFlags();
    tty.println("  RedjetGC Macro Subsystem Constellation (SA Live View):");
    tty.println("    Scheduler Engine:           " + flags.schedulerMode
        + ("TBB".equalsIgnoreCase(flags.schedulerMode)
            ? " [Domestic DSP Silenced]"
            : " [100% In-Order DSP | TBB Silenced]"));
    tty.println("    2-Points Sync Architecture: Dockpoint (Slot 16/TLAB) + Parkpoint (L1-D Loop/JNI)");
    tty.println("    TailTraces Dead Signals:    " + (flags.tailTracesSignals ? "ACTIVE (/heap/on_heap/rpss + BPU)" : "DISABLED"));
    tty.println("    Explicit GC Clean Hints:    " + (flags.explicitCleanHints ? "ACTIVE (JLC/VAC Lookahead 0-STW)" : "DISABLED"));
    tty.println("    SIMD Contention Governor:   " + (flags.autoScalarOnSimdContention ? "AUTO STEP-DOWN (512->256->128->Scalar)" : "FIXED")
        + (flags.criticalSimdOnly ? " [Critical-Only]" : " [Full-Pipeline]"));
    tty.println("    Compressed Spaces Layout:   narrowKlass -> /heap/on_heap/landfill/header_pane | JIT -> /heap/off_heap/code_boot");
    tty.println("    Native Interop Split:       JNI = On-Heap<=>Native (park_arena) | FFM = Off-Heap<=>Native (@RJGC)");
    tty.println("    Strict VFS & RMRP Coalesce: VFS=" + flags.strictVfs + " | RMRP=" + flags.rmrpEnabled);
  }

  private static boolean readBoolPtr(Address inst, AddressField field, boolean fallback) {
    if (field == null) {
      return fallback;
    }
    Address ptr = field.getValue(inst);
    if (ptr == null) {
      return fallback;
    }
    return ptr.getJBooleanAt(0);
  }

  private static String readCString(Address addr, int maxLen) {
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < maxLen; i++) {
      byte b = (byte) addr.getCIntegerAt(i, 1, true);
      if (b == 0) {
        break;
      }
      sb.append((char) (b & 0xFF));
    }
    return sb.length() > 0 ? sb.toString() : "Domestic";
  }
}
