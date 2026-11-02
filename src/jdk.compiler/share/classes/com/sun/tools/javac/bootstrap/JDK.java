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

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * ============================================================================
 * JDK — Root Namespace & Metadata Containers for oneJDK-25
 * ============================================================================
 * Hosts system-level meta-annotations including '@JDK.DeadCode' to explicitly
 * identify bootstrap seeds, post-bootstrap inert classes, and preserved
 * research/reference artifacts across the JDK source repository.
 * ============================================================================
 */
public final class JDK {

    private JDK() {
        // Namespace holder
    }

    /**
     * Identifies code that is post-bootstrap dead code or inert reference material,
     * retained intentionally in the JDK repository for inspection, debugging,
     * architecture study, and compiler research.
     */
    @Retention(RetentionPolicy.RUNTIME)
    @Target({ElementType.TYPE, ElementType.METHOD, ElementType.FIELD, ElementType.CONSTRUCTOR})
    public @interface DeadCode {
        String reason() default "Post-bootstrap ephemeral seed; native compilation is handled by bin/jlc.exe";
        String retainedFor() default "Architecture inspection, compiler research, and bootstrap verification";
        String phase() default "BOOTSTRAP_ONLY_SEED";
    }
}
