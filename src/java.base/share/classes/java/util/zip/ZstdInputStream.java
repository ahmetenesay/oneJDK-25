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
import java.io.InputStream;
import java.io.FilterInputStream;

/**
 * This class provides support for decompression using the Zstd compression library.
 * It wraps an {@link InputStream} and decompresses Zstd compressed data on the fly.
 * <p>
 * To release the resources used by a {@code ZstdInputStream}, an application must close it
 * by invoking its {@link #close()} method.
 *
 * @apiNote
 * This class implements {@link AutoCloseable} to facilitate its usage with
 * {@code try}-with-resources statement.
 *
 * @see         ZstdOutputStream
 * @since 25
 */

public class ZstdInputStream extends FilterInputStream {

    private final ZstdInflater inflater;
    private final byte[] buf;
    private int bufOffset;
    private int bufLen;
    private boolean eos;

    /**
     * Creates a new Zstd input stream with the default buffer size.
     * @param in the underlying input stream
     */
    public ZstdInputStream(InputStream in) {
        this(in, 8192);
    }

    /**
     * Creates a new Zstd input stream with the specified buffer size.
     * @param in the underlying input stream
     * @param size the buffer size
     */
    public ZstdInputStream(InputStream in, int size) {
        super(in);
        this.inflater = new ZstdInflater();
        this.buf = new byte[size];
        this.bufOffset = 0;
        this.bufLen = 0;
    }

    /**
     * Reads the next byte of decompressed data from this input stream.
     * @return the next byte of decompressed data, or -1 if end of stream
     * @throws IOException if an I/O error occurs
     */
    @Override
    public int read() throws IOException {
        if (bufOffset >= bufLen && !eos) {
            if (fill() == -1) {
                return -1;
            }
        }
        if (bufOffset >= bufLen) {
            return -1;
        }
        return buf[bufOffset++] & 0xFF;
    }

    /**
     * Reads up to {@code len} bytes of decompressed data into an array of bytes.
     * @param b the buffer into which the data is read
     * @param off the start offset in the destination array
     * @param len the maximum number of bytes to read
     * @return the total number of bytes read into the buffer, or -1 if end of stream
     * @throws IOException if an I/O error occurs
     */
    @Override
    public int read(byte[] b, int off, int len) throws IOException {
        if (b == null) {
            throw new NullPointerException();
        } else if (off < 0 || len < 0 || len > b.length - off) {
            throw new IndexOutOfBoundsException();
        } else if (len == 0) {
            return 0;
        }

        if (bufOffset >= bufLen && !eos) {
            if (fill() == -1) {
                return -1;
            }
        }
        if (bufOffset >= bufLen) {
            return -1;
        }

        int count = Math.min(len, bufLen - bufOffset);
        System.arraycopy(buf, bufOffset, b, off, count);
        bufOffset += count;
        return count;
    }

    /**
     * Reads the remaining decompressed data into a byte array.
     * @return a byte array containing the remaining decompressed data
     * @throws IOException if an I/O error occurs
     */
    public byte[] readAllBytes() throws IOException {
        java.util.List<Byte> bytes = new java.util.ArrayList<>();
        int b;
        while ((b = read()) != -1) {
            bytes.add((byte) b);
        }
        byte[] result = new byte[bytes.size()];
        for (int i = 0; i < result.length; i++) {
            result[i] = bytes.get(i);
        }
        return result;
    }

    /**
     * Skips {@code n} bytes of decompressed data.
     * @param n the number of bytes to skip
     * @return the actual number of bytes skipped
     * @throws IOException if an I/O error occurs
     */
    @Override
    public long skip(long n) throws IOException {
        if (n <= 0) {
            return 0;
        }
        long remaining = n;
        while (remaining > 0) {
            if (bufOffset >= bufLen && !eos) {
                if (fill() == -1) {
                    break;
                }
            }
            if (bufOffset >= bufLen) {
                break;
            }
            int skip = (int) Math.min(remaining, bufLen - bufOffset);
            bufOffset += skip;
            remaining -= skip;
        }
        return n - remaining;
    }

    /**
     * Returns the number of bytes that can be read without blocking.
     * @return the number of bytes available
     * @throws IOException if an I/O error occurs
     */
    @Override
    public int available() throws IOException {
        return bufLen - bufOffset;
    }

    /**
     * Closes this input stream and releases associated resources.
     * @throws IOException if an I/O error occurs
     */
    @Override
    public void close() throws IOException {
        try {
            inflater.end();
        } finally {
            super.close();
        }
    }

    /**
     * Returns the number of input bytes read so far.
     * @return the number of input bytes read
     */
    public long bytesRead() {
        return inflater.bytesRead();
    }

    /**
     * Returns the number of output bytes written so far.
     * @return the number of output bytes written
     */
    public long bytesWritten() {
        return inflater.bytesWritten();
    }

    private int fill() throws IOException {
        bufOffset = 0;
        bufLen = 0;
        if (inflater.finished()) {
            eos = true;
            return -1;
        }
        try {
            int n = in.read(buf, 0, buf.length);
            if (n == -1) {
                eos = true;
                return -1;
            }
            inflater.setInput(buf, 0, n);
            bufLen = inflater.inflate(buf);
            return bufLen;
        } catch (DataFormatException e) {
            throw new IOException(e);
        }
    }
}
