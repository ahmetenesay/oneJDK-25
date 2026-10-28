/*
 * Copyright (c) 2025 JetBrains and/or its affiliates. All rights reserved.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package java.util.zip;

import java.io.IOException;
import java.io.FilterOutputStream;
import java.io.OutputStream;

/**
 * This class provides support for compression using the Zstd compression library.
 * It wraps an {@link OutputStream} and compresses data using Zstd on the fly.
 * <p>
 * To release the resources used by a {@code ZstdOutputStream}, an application must close it
 * by invoking its {@link #close()} method.
 *
 * @apiNote
 * This class implements {@link AutoCloseable} to facilitate its usage with
 * {@code try}-with-resources statement.
 *
 * @see         ZstdInputStream
 * @since 25
 */

public class ZstdOutputStream extends FilterOutputStream {

    private final ZstdDeflater deflater;
    private final byte[] buf;
    private boolean closed;
    private boolean finished;

    /**
     * Creates a new Zstd output stream with the default buffer size and compression level.
     * @param out the underlying output stream
     */
    public ZstdOutputStream(OutputStream out) {
        this(out, 8192);
    }

    /**
     * Creates a new Zstd output stream with the specified buffer size and default compression level.
     * @param out the underlying output stream
     * @param size the buffer size
     */
    public ZstdOutputStream(OutputStream out, int size) {
        this(out, size, ZstdDeflater.DEFAULT_COMPRESSION);
    }

    /**
     * Creates a new Zstd output stream with the specified buffer size and compression level.
     * @param out the underlying output stream
     * @param size the buffer size
     * @param level the compression level (0-19)
     */
    public ZstdOutputStream(OutputStream out, int size, int level) {
        super(out);
        this.deflater = new ZstdDeflater(level);
        this.buf = new byte[size];
    }

    /**
     * Writes the specified byte to this output stream.
     * @param b the byte to write
     * @throws IOException if an I/O error occurs
     */
    @Override
    public void write(int b) throws IOException {
        byte[] bytes = new byte[]{(byte) b};
        write(bytes, 0, 1);
    }

    /**
     * Writes {@code len} bytes from the specified byte array starting at offset {@code off}
     * to this output stream.
     * @param b the data to write
     * @param off the start offset in the data
     * @param len the number of bytes to write
     * @throws IOException if an I/O error occurs
     */
    @Override
    public void write(byte[] b, int off, int len) throws IOException {
        if (closed) {
            throw new IOException("Stream closed");
        }
        if (b == null) {
            throw new NullPointerException();
        } else if (off < 0 || len < 0 || len > b.length - off) {
            throw new IndexOutOfBoundsException();
        } else if (len == 0) {
            return;
        }

        deflater.setInput(b, off, len);
        while (!deflater.needsInput() && !deflater.finished()) {
            int n = deflater.deflate(buf);
            if (n > 0) {
                out.write(buf, 0, n);
            }
        }
    }

    /**
     * Flushes this output stream and forces any buffered output bytes to be written.
     * @throws IOException if an I/O error occurs
     */
    @Override
    public void flush() throws IOException {
        if (!closed && !finished) {
            deflater.finish();
            while (!deflater.finished()) {
                int n = deflater.deflate(buf);
                if (n > 0) {
                    out.write(buf, 0, n);
                }
            }
            finished = true;
        }
        out.flush();
    }

    /**
     * Closes this output stream and releases associated resources.
     * @throws IOException if an I/O error occurs
     */
    @Override
    public void close() throws IOException {
        if (!closed) {
            closed = true;
            try {
                finish();
            } finally {
                deflater.end();
                super.close();
            }
        }
    }

    /**
     * Finishes writing compressed data without closing the underlying stream.
     * @throws IOException if an I/O error occurs
     */
    public void finish() throws IOException {
        if (!finished) {
            deflater.finish();
            while (!deflater.finished()) {
                int n = deflater.deflate(buf);
                if (n > 0) {
                    out.write(buf, 0, n);
                }
            }
            finished = true;
        }
    }

    /**
     * Returns the number of input bytes written so far.
     * @return the number of input bytes written
     */
    public long bytesRead() {
        return deflater.bytesRead();
    }

    /**
     * Returns the number of output bytes written so far.
     * @return the number of output bytes written
     */
    public long bytesWritten() {
        return deflater.bytesWritten();
    }

    /**
     * Returns the current compression level.
     * @return the compression level
     */
    public int getLevel() {
        return deflater.getLevel();
    }
}
