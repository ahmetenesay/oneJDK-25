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
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Representation of the RedjetGC VFS Hierarchical Memory Tree for the Serviceability Agent.
 *
 * Root partitions:
 * 1. On-Heap:
 *    - /heap/on_heap/runway (motherline egg/fledgling, pitstop, falline)
 *    - /heap/on_heap/puzzle_arena (stagnant, eval)
 *    - /heap/on_heap/landfill (matured, rehab, header_pane)
 *    - /heap/on_heap/rpss (TailTraces reference harbingers)
 * 2. Off-Heap:
 *    - /heap/off_heap/code_boot (rawcode_cell, vac_processed_code_chunk, mirc_execution_ready_page lvl1/lvl2)
 *    - /heap/off_heap/log_boot (hotlog, coldlog)
 *    - /heap/off_heap/reserved_boot
 */
public class RedjetVFSTree {

  public static class BranchDesc {
    private final String path;
    private final String purpose;
    private final boolean isOffHeap;

    public BranchDesc(String path, String purpose, boolean isOffHeap) {
      this.path = path;
      this.purpose = purpose;
      this.isOffHeap = isOffHeap;
    }

    public String path() { return path; }
    public String purpose() { return purpose; }
    public boolean isOffHeap() { return isOffHeap; }
  }

  private static final List<BranchDesc> BRANCHES;

  static {
    List<BranchDesc> b = new ArrayList<>();
    // Runway - Motherline - Egg (Pre-Puzzler newborn allocations)
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/young/tank", "Newborn young objects bulk allocation tank", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/young/demand", "On-demand young objects allocation lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/young/middle", "Medium-sized young objects staging lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/young/hotfield", "High-frequency accessed young objects ordered by footprint (OOH)", false));

    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/adolescent/tank", "Mid-generation pre-puzzler bulk tank", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/adolescent/demand", "Mid-generation pre-puzzler demand lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/adolescent/middle", "Mid-generation pre-puzzler medium lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/adolescent/hotfield", "High-frequency mid-gen objects sorted by memory footprint", false));

    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/senior/tank", "Pre-puzzler tenured survival tank", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/senior/demand", "Pre-puzzler tenured survival demand lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/senior/middle", "Pre-puzzler tenured medium lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/egg/senior/hotfield", "Pre-puzzler tenured hotfield footprint-ordered objects", false));

    // Runway - Motherline - Fledgling (Post-Puzzler optimized objects)
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/young/tank", "Post-puzzler optimized young tank", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/young/demand", "Post-puzzler optimized young demand lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/young/middle", "Post-puzzler optimized young medium lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/young/hotfield", "Post-puzzler optimized young hotfield", false));

    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/adolescent/tank", "Post-puzzler adolescent tank", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/adolescent/demand", "Post-puzzler adolescent demand lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/adolescent/middle", "Post-puzzler adolescent medium lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/adolescent/hotfield", "Post-puzzler adolescent hotfield", false));

    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/senior/tank", "Post-puzzler senior tenured tank", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/senior/demand", "Post-puzzler senior tenured demand lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/senior/middle", "Post-puzzler senior tenured medium lane", false));
    b.add(new BranchDesc("/heap/on_heap/runway/motherline/fledgling/senior/hotfield", "Post-puzzler senior tenured hotfield", false));

    // Runway - Pitstop & Falline
    b.add(new BranchDesc("/heap/on_heap/runway/pitstop", "Puzzler dynamic duel and code replacement lobby", false));
    b.add(new BranchDesc("/heap/on_heap/runway/falline", "Pre-trashing hearse lane for condemned objects", false));

    // Puzzle Arena & Park Arena
    b.add(new BranchDesc("/heap/on_heap/puzzle_arena/stagnant", "Objects pending profile evaluation in puzzler arena", false));
    b.add(new BranchDesc("/heap/on_heap/puzzle_arena/eval", "Active duel evaluation and benchmarking arena", false));
    b.add(new BranchDesc("/heap/on_heap/park_arena", "Java to Native pinned transition buffer (is_pinned = 1)", false));

    // Landfill
    b.add(new BranchDesc("/heap/on_heap/landfill/trash_pit/matured", "Totally dead objects ready for irreversible sweep", false));
    b.add(new BranchDesc("/heap/on_heap/landfill/trash_pit/rehab", "Rehabilitation buffer for resurrected objects", false));
    b.add(new BranchDesc("/heap/on_heap/landfill/header_pane", "Isolated metaspace headers for deceased objects", false));

    // RPSS (TailTraces)
    b.add(new BranchDesc("/heap/on_heap/rpss", "Reference Predicting Signal Storage (TailTraces clasp queue)", false));

    // Off-Heap: Code Boot
    b.add(new BranchDesc("/heap/off_heap/code_boot/rawcode_cell", "Read-only bytecode cell sealed with VAC_READ key", true));
    b.add(new BranchDesc("/heap/off_heap/code_boot/vac_processed_code_chunk", "W^X protected VAC processed chunk sealed with MIRC_READ key", true));
    b.add(new BranchDesc("/heap/off_heap/code_boot/mirc_execution_ready_page/lvl1", "MIRC Level 1 fast JIT execution page", true));
    b.add(new BranchDesc("/heap/off_heap/code_boot/mirc_execution_ready_page/lvl2", "MIRC Level 2 optimizing JIT execution page", true));

    // Off-Heap: Log Boot & Reserved
    b.add(new BranchDesc("/heap/off_heap/log_boot/hotlog", "High-frequency lockless ring log", true));
    b.add(new BranchDesc("/heap/off_heap/log_boot/coldlog", "Compressed cold telemetry storage", true));
    b.add(new BranchDesc("/heap/off_heap/reserved_boot", "Reserved off-heap memory safety buffer", true));

    BRANCHES = Collections.unmodifiableList(b);
  }

  public static List<BranchDesc> getBranches() {
    return BRANCHES;
  }

  public static void printSummary(PrintStream tty) {
    tty.println("RedjetGC VFS Memory Tree (40 Canonical Branches):");
    for (BranchDesc b : BRANCHES) {
      tty.printf("  %-58s [%s] %s%n", b.path(), b.isOffHeap() ? "OFF-HEAP" : "ON-HEAP", b.purpose());
    }
  }
}
