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

import java.io.PrintStream;
import sun.jvm.hotspot.debugger.*;
import sun.jvm.hotspot.types.*;
import sun.jvm.hotspot.runtime.*;
import sun.jvm.hotspot.memory.*;

public class RedjetHeapRegion extends VMObject {

  public enum RegionType {
    RUNWAY_EGG("Runway/Egg"),
    RUNWAY_FLEDGLING("Runway/Fledgling"),
    RUNWAY_PITSTOP("Runway/Pitstop"),
    RUNWAY_FALLINE("Runway/Falline"),
    PUZZLE_ARENA("PuzzleArena"),
    LANDFILL_MATURED("Landfill/Matured"),
    LANDFILL_REHAB("Landfill/Rehab"),
    LANDFILL_HEADER_PANE("Landfill/HeaderPane"),
    RPSS_TAILTRACES("RPSS/TailTraces"),
    OFFHEAP_CODE_BOOT("OffHeap/CodeBoot"),
    OFFHEAP_LOG_BOOT("OffHeap/LogBoot"),
    OFFHEAP_RESERVED("OffHeap/Reserved");

    private final String description;
    RegionType(String desc) { this.description = desc; }
    public String description() { return description; }
  }

  private String name;
  private Address bottom;
  private Address top;
  private Address end;
  private RegionType type;

  public RedjetHeapRegion(String name, Address bottom, Address top, Address end, RegionType type) {
    super(bottom != null ? bottom : VM.getVM().getDebugger().newAddress(0));
    this.name = name;
    this.bottom = bottom;
    this.top = top;
    this.end = end;
    this.type = type;
  }

  public String name() { return name; }
  public Address bottom() { return bottom; }
  public Address top() { return top; }
  public Address end() { return end; }
  public RegionType type() { return type; }

  public long used() {
    return (top != null && bottom != null) ? top.minus(bottom) : 0;
  }

  public long capacity() {
    return (end != null && bottom != null) ? end.minus(bottom) : 0;
  }

  public void printOn(PrintStream tty) {
    tty.println("RedjetRegion [" + name + "] type: " + type.description() +
                " [" + bottom + ", " + top + ", " + end + "] used: " + used() + " bytes");
  }
}
