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

import java.lang.ref.Cleaner.Cleanable;
import java.nio.ByteBuffer;
import java.nio.ReadOnlyBufferException;

import jdk.internal.ref.CleanerFactory;
import java.util.Objects;

import jdk.internal.util.Preconditions;

import static java.util.zip.ZipUtils.NIO_ACCESS;

/**
 * This class provides support for compression using the Zstd compression library.
 * Zstd (Zstandard) is a fast lossless compression algorithm, targeting
 * compression speeds >300 MB/s per core and a real-time decompression speed on
 * single CPU core, while featuring a very high compression ratio.
 * <p>
 * Unless otherwise noted, passing a {@code null} argument to a method
 * in this class will cause a {@link NullPointerException} to be
 * thrown.
 * <p>
 * This class deflates sequences of bytes into Zstd compressed data format.
 * The input byte sequence is provided in either a byte array or a {@link ByteBuffer},
 * via one of the {@code setInput()} methods. The output byte sequence is
 * written to the output byte array or {@code ByteBuffer} passed to the
 * {@code deflate()} methods.
 * <p>
 * To release the resources used by a {@code ZstdDeflater}, an application must close it
 * by invoking its {@link #end()} or {@link #close()} method.
 *
 * @apiNote
 * This class implements {@link AutoCloseable} to facilitate its usage with
 * {@code try}-with-resources statement. The {@linkplain ZstdDeflater#close() close() method} simply
 * calls {@code end()}.
 *
 * @see         ZstdInflater
 * @since 25
 */

public class ZstdDeflater implements AutoCloseable {

    private final ZstdZStreamRef zsRef;
    private ByteBuffer input;
    private byte[] inputArray;
    private int inputPos, inputLim;
    private int level;
    private boolean finish, finished;
    private long bytesRead;
    private long bytesWritten;

    /**
     * Compression level for no compression.
     */
    public static final int NO_COMPRESSION = 0;

    /**
     * Compression level for fastest compression.
     */
    public static final int BEST_SPEED = 1;

    /**
     * Compression level for best compression.
     * Zstd supports levels up to 22, but 19 is the practical maximum.
     */
    public static final int BEST_COMPRESSION = 19;

    /**
     * Default compression level.
     */
    public static final int DEFAULT_COMPRESSION = 3;

    /**
     * Compression flush mode used to achieve best compression result.
     */
    public static final int NO_FLUSH = 0;

    /**
     * Compression flush mode used to flush out all pending output.
     */
    public static final int SYNC_FLUSH = 1;

    /**
     * Compression flush mode used to flush out all pending output and
     * reset the deflater.
     */
    public static final int FULL_FLUSH = 2;

    /**
     * Flush mode to use at the end of output.
     */
    private static final int FINISH = 4;

    static {
        ZipUtils.loadLibrary();
    }

    /**
     * Creates a new compressor using the specified compression level.
     * @param level the compression level (0-19, or DEFAULT_COMPRESSION)
     */
    @SuppressWarnings("this-escape")
    public ZstdDeflater(int level) {
        if (level != DEFAULT_COMPRESSION && (level < 0 || level > BEST_COMPRESSION)) {
            throw new IllegalArgumentException("invalid compression level: " + level);
        }
        this.level = (level == DEFAULT_COMPRESSION) ? DEFAULT_COMPRESSION : level;
        this.zsRef = new ZstdZStreamRef(this, init(level));
    }

    /**
     * Creates a new compressor with the default compression level.
     */
    public ZstdDeflater() {
        this(DEFAULT_COMPRESSION);
    }

    /**
     * Sets input data for compression.
     * @param input the input data bytes
     * @param off the start offset of the data
     * @param len the length of the data
     */
    public void setInput(byte[] input, int off, int len) {
        Preconditions.checkFromIndexSize(off, len, input.length, Preconditions.AIOOBE_FORMATTER);
        synchronized (zsRef) {
            this.input = null;
            this.inputArray = input;
            this.inputPos = off;
            this.inputLim = off + len;
        }
    }

    /**
     * Sets input data for compression.
     * @param input the input data bytes
     */
    public void setInput(byte[] input) {
        setInput(input, 0, input.length);
    }

    /**
     * Sets input data for compression.
     * @param input the input data bytes
     * @since 25
     */
    public void setInput(ByteBuffer input) {
        Objects.requireNonNull(input);
        synchronized (zsRef) {
            this.input = input;
            this.inputArray = null;
        }
    }

    /**
     * Sets the compression level to the specified value.
     * @param level the new compression level (0-19)
     */
    public void setLevel(int level) {
        if (level != DEFAULT_COMPRESSION && (level < 0 || level > BEST_COMPRESSION)) {
            throw new IllegalArgumentException("invalid compression level: " + level);
        }
        synchronized (zsRef) {
            this.level = (level == DEFAULT_COMPRESSION) ? DEFAULT_COMPRESSION : level;
        }
    }

    /**
     * Returns true if no data remains in the input buffer.
     * @return true if the input data buffer is empty
     */
    public boolean needsInput() {
        synchronized (zsRef) {
            ByteBuffer input = this.input;
            return input == null ? inputLim == inputPos : !input.hasRemaining();
        }
    }

    /**
     * When called, indicates that compression should end with the current
     * contents of the input buffer.
     */
    public void finish() {
        synchronized (zsRef) {
            this.finish = true;
        }
    }

    /**
     * Returns true if the end of the compressed data output stream has
     * been reached.
     * @return true if the end of the compressed data output stream has been reached
     */
    public boolean finished() {
        synchronized (zsRef) {
            return finished;
        }
    }

    /**
     * Compresses the input data and fills specified buffer with compressed
     * data. Returns actual number of bytes of compressed data.
     * @param output the buffer for the compressed data
     * @param off the start offset of the data
     * @param len the maximum number of bytes of compressed data
     * @return the actual number of bytes of compressed data written
     */
    public int deflate(byte[] output, int off, int len) {
        return deflate(output, off, len, NO_FLUSH);
    }

    /**
     * Compresses the input data and fills specified buffer with compressed
     * data. Returns actual number of bytes of compressed data.
     * @param output the buffer for the compressed data
     * @return the actual number of bytes of compressed data written
     */
    public int deflate(byte[] output) {
        return deflate(output, 0, output.length, NO_FLUSH);
    }

    /**
     * Compresses the input data and fills the specified buffer with compressed
     * data. Returns actual number of bytes of data compressed.
     * @param output the buffer for the compressed data
     * @param off the start offset of the data
     * @param len the maximum number of bytes of compressed data
     * @param flush the flush mode
     * @return the actual number of bytes of compressed data written
     */
    public int deflate(byte[] output, int off, int len, int flush) {
        Preconditions.checkFromIndexSize(off, len, output.length, Preconditions.AIOOBE_FORMATTER);
        synchronized (zsRef) {
            ensureOpen();
            int bytesWritten = deflate(zsRef.address(), inputArray, inputPos,
                inputLim - inputPos, output, off, len, flush);
            this.bytesWritten += bytesWritten;
            inputPos += (inputLim - inputPos) - bytesWritten;
            if (finish || bytesWritten == 0) {
                finished = true;
            }
            return bytesWritten;
        }
    }

    /**
     * Compresses the input data and fills the specified buffer with compressed
     * data. Returns actual number of bytes of data compressed.
     * @param output the buffer for the compressed data
     * @return the actual number of bytes of compressed data written
     * @throws ReadOnlyBufferException if the given output buffer is read-only
     * @since 25
     */
    public int deflate(ByteBuffer output) {
        return deflate(output, NO_FLUSH);
    }

    /**
     * Compresses the input data and fills the specified buffer with compressed
     * data. Returns actual number of bytes of data compressed.
     * @param output the buffer for the compressed data
     * @param flush the flush mode
     * @return the actual number of bytes of compressed data written
     * @throws ReadOnlyBufferException if the given output buffer is read-only
     * @since 25
     */
    public int deflate(ByteBuffer output, int flush) {
        Objects.requireNonNull(output);
        if (output.isReadOnly()) {
            throw new ReadOnlyBufferException();
        }
        synchronized (zsRef) {
            ensureOpen();
            ByteBuffer input = this.input;
            int bytesWritten;
            if (input != null && input.hasRemaining()) {
                if (input.isDirect()) {
                    NIO_ACCESS.acquireSession(input);
                    try {
                        long inputAddr = NIO_ACCESS.getBufferAddress(input);
                        NIO_ACCESS.acquireSession(output);
                        try {
                            long outputAddr = NIO_ACCESS.getBufferAddress(output);
                            bytesWritten = deflateBuffer(zsRef.address(),
                                inputAddr + input.position(), input.remaining(),
                                outputAddr + output.position(), output.remaining(), flush);
                        } finally {
                            NIO_ACCESS.releaseSession(output);
                        }
                    } finally {
                        NIO_ACCESS.releaseSession(input);
                    }
                } else {
                    byte[] arr = ZipUtils.getBufferArray(input);
                    int offset = ZipUtils.getBufferOffset(input);
                    bytesWritten = deflate(zsRef.address(), arr, offset + input.position(),
                        input.remaining(),
                        ZipUtils.getBufferArray(output),
                        ZipUtils.getBufferOffset(output) + output.position(),
                        output.remaining(), flush);
                }
                input.position(input.position() + input.remaining());
            } else if (inputArray != null && inputLim > inputPos) {
                bytesWritten = deflate(zsRef.address(), inputArray, inputPos,
                    inputLim - inputPos,
                    ZipUtils.getBufferArray(output),
                    ZipUtils.getBufferOffset(output) + output.position(),
                    output.remaining(), flush);
                inputPos += (inputLim - inputPos);
            } else {
                bytesWritten = 0;
            }
            this.bytesWritten += bytesWritten;
            if (finish || bytesWritten == 0) {
                finished = true;
            }
            output.position(output.position() + bytesWritten);
            return bytesWritten;
        }
    }

    /**
     * Returns the number of input bytes processed so far.
     * @return the number of input bytes read
     */
    public long bytesRead() {
        synchronized (zsRef) {
            return bytesRead;
        }
    }

    /**
     * Returns the number of output bytes written so far.
     * @return the number of output bytes written
     */
    public long bytesWritten() {
        synchronized (zsRef) {
            return bytesWritten;
        }
    }

    /**
     * Returns the current compression level.
     * @return the compression level
     */
    public int getLevel() {
        synchronized (zsRef) {
            return level;
        }
    }

    /**
     * Ends compression and releases associated resources.
     */
    public void end() {
        synchronized (zsRef) {
            if (zsRef.address() != 0) {
                end(zsRef.address());
                zsRef.clear();
            }
        }
    }

    /**
     * Releases the resources associated with this compressor.
     */
    @Override
    public void close() {
        end();
    }

    private void ensureOpen() {
        if (zsRef.address() == 0) {
            throw new IllegalStateException("Deflater is closed");
        }
    }

    // Native methods
    private native static long init(int level);
    private native static int deflate(long zs, byte[] input, int inputOff, int inputLen,
                                      byte[] output, int outputOff, int outputLen, int flush);
    private native static int deflateBuffer(long zs, long inputAddr, int inputLen,
                                            long outputAddr, int outputLen, int flush);
    private native static void end(long zs);

    /**
     * Reference to the native Zstd stream, cleaned up when garbage collected.
     */
    private static class ZstdZStreamRef {
        private final Cleanable cleanable;
        private long address;

        ZstdZStreamRef(ZstdDeflater deflater, long address) {
            this.address = address;
            this.cleanable = CleanerFactory.cleaner()
                .register(this, () -> {
                    synchronized (deflater.zsRef) {
                        if (address != 0) {
                            ZstdDeflater.end(address);
                            this.address = 0;
                        }
                    }
                });
        }

        long address() {
            return address;
        }

        void clear() {
            cleanable.clean();
            address = 0;
        }
    }
}
