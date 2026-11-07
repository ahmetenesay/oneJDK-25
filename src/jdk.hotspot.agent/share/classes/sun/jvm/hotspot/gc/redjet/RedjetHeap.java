/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation. Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
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

import java.io.*;
import java.util.*;

import sun.jvm.hotspot.gc.shared.*;
import sun.jvm.hotspot.debugger.*;
import sun.jvm.hotspot.runtime.*;
import sun.jvm.hotspot.types.*;
import sun.jvm.hotspot.memory.*;
import sun.jvm.hotspot.utilities.Observable;
import sun.jvm.hotspot.utilities.Observer;

public class RedjetHeap extends CollectedHeap {

  private static AddressField spaceField;
  private static Field virtualSpaceField;
  private ContiguousSpace space;
  private VirtualSpace virtualSpace;

  // EmptyBox live telemetry record
  public static class EmptyBoxTelemetry {
    public long reservationsCreated;
    public long reservationsClaimed;
    public long timeoutsDetected;
    public long theftAttemptsPrevented;
    public long activeReservedBytes;

    public void printOn(PrintStream tty) {
      tty.println("  EmptyBox Phantom Reservation Subsystem (0xEB0C5107):");
      tty.println("    Created reservations:  " + reservationsCreated);
      tty.println("    Claimed reservations:  " + reservationsClaimed);
      tty.println("    Timeouts detected:     " + timeoutsDetected);
      tty.println("    Theft prevented:       " + theftAttemptsPrevented);
      tty.println("    Active reserved bytes: " + activeReservedBytes + " bytes");
    }
  }

  // MLTC live telemetry record
  public static class MLTCTelemetry {
    public float  bandwidthMbps;
    public float  congestionScore;
    public String congestionLevel = "CONGESTION_FREE";
    public long   pacedAllocations;
    public long   stallDelayNs;
    public String leakRiskTier = "LEAK_CLEAN";

    public void printOn(PrintStream tty) {
      tty.println("  MLTC Multi-Level Traffic Controller & Leak Detector:");
      tty.printf("    Bus bandwidth:         %.2f MB/s%n", bandwidthMbps);
      tty.printf("    Congestion score:      %.2f [%s]%n", congestionScore, congestionLevel);
      tty.println("    Paced allocations:     " + pacedAllocations);
      tty.println("    Total stall delay:     " + (stallDelayNs / 1000000) + " ms");
      tty.println("    Leak risk tier:        " + leakRiskTier);
    }
  }

  // RMRP live telemetry record
  public static class RMRPTelemetry {
    public long   plumpReclaimedBytes;
    public long   sequentialReclaimedBytes;
    public long   vfsReclaimedBytes;
    public long   zeroDeduplicatedBytes;
    public long   reclamationCycles;
    public long   vfsCompactionTriggers;

    public void printOn(PrintStream tty) {
      tty.println("  RMRP (Reduce Memory Resident Set Propagation):");
      tty.println("    Plump reclaimed:       " + (plumpReclaimedBytes / 1024) + " KB");
      tty.println("    Sequential reclaimed:  " + (sequentialReclaimedBytes / 1024) + " KB");
      tty.println("    VFS reclaimed:         " + (vfsReclaimedBytes / 1024) + " KB");
      tty.println("    Zero-page dedup:       " + (zeroDeduplicatedBytes / 1024) + " KB");
      tty.println("    Total cycles:          " + reclamationCycles);
      tty.println("    VFS auto-compactions:  " + vfsCompactionTriggers);
    }
  }

  private EmptyBoxTelemetry emptyBoxTelemetry = new EmptyBoxTelemetry();
  private MLTCTelemetry     mltcTelemetry     = new MLTCTelemetry();
  private RMRPTelemetry     rmrpTelemetry     = new RMRPTelemetry();

  static {
    VM.registerVMInitializedObserver(new Observer() {
      public void update(Observable o, Object data) {
        initialize(VM.getVM().getTypeDataBase());
      }
    });
  }

  private static void initialize(TypeDataBase db) {
    Type type = db.lookupType("RedjetHeap");
    spaceField = type.getAddressField("_space");
    virtualSpaceField = type.getField("_virtual_space");
  }

  public RedjetHeap(Address addr) {
    super(addr);
    Address spaceAddr = spaceField.getValue(addr);
    if (spaceAddr != null) {
      space = new ContiguousSpace(spaceAddr);
    }
    virtualSpace = new VirtualSpace(addr.addOffsetTo(virtualSpaceField.getOffset()));
  }

  @Override
  public CollectedHeapName kind() {
    return CollectedHeapName.REDJET;
  }

  @Override
  public long capacity() {
    return space != null ? space.capacity() : virtualSpace.capacity();
  }

  @Override
  public long used() {
    return space != null ? space.used() : 0;
  }

  public ContiguousSpace space() {
    return space;
  }

  public EmptyBoxTelemetry getEmptyBoxTelemetry() {
    return emptyBoxTelemetry;
  }

  public MLTCTelemetry getMLTCTelemetry() {
    return mltcTelemetry;
  }

  public RMRPTelemetry getRMRPTelemetry() {
    return rmrpTelemetry;
  }

  @Override
  public void liveRegionsIterate(LiveRegionsClosure closure) {
    if (space != null) {
      closure.doLiveRegions(space());
    }
  }

  @Override
  public void printOn(PrintStream tty) {
    MemRegion mr = reservedRegion();
    tty.println("RedjetGC Big GC Engine Heap (VFS on-heap & off-heap)");
    tty.println(" reserved:  [" + mr.start() + ", " + mr.end() + "]");
    tty.println(" committed: [" + virtualSpace.low() + ", " + virtualSpace.high() + "]");
    if (space != null) {
      tty.println(" used:      [" + space.bottom() + ", " + space.top() + "]");
    }
    tty.println();
    emptyBoxTelemetry.printOn(tty);
    tty.println();
    mltcTelemetry.printOn(tty);
    tty.println();
    rmrpTelemetry.printOn(tty);
    tty.println();
    RedjetVFSTree.printSummary(tty);
  }
}
