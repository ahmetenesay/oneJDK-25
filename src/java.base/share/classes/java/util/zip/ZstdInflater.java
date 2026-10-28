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
import jdk.internal.util.Preconditions;

import static java.util.zip.ZipUtils.NIO_ACCESS;

/**
 * This class provides support for decompression using the Zstd compression library.
 * Zstd (Zstandard) is a fast lossless compression algorithm.
 * <p>
 * Unless otherwise noted, passing a {@code null} argument to a method
 * in this class will cause a {@link NullPointerException} to be
 * thrown.
 * <p>
 * This class inflates Zstd compressed data back to the original byte sequence.
 * The input byte sequence is provided in either a byte array or a {@link ByteBuffer},
 * via one of the {@code setInput()} methods. The output byte sequence is
 * written to the output byte array or {@code ByteBuffer} passed to the
 * {@code inflate()} methods.
 * <p>
 * To release the resources used by a {@code ZstdInflater}, an application must close it
 * by invoking its {@link #end()} or {@link #close()} method.
 *
 * @apiNote
 * This class implements {@link AutoCloseable} to facilitate its usage with
 * {@code try}-with-resources statement. The {@linkplain ZstdInflater#close() close() method} simply
 * calls {@code end()}.
 *
 * @see         ZstdDeflater
 * @since 25
 */

public class ZstdInflater implements AutoCloseable {

    private final ZstdZStreamRef zsRef;
    private ByteBuffer input;
    private byte[] inputArray;
    private int inputPos, inputLim;
    private boolean finished;
    private long bytesRead;
    private long bytesWritten;

    static {
        ZipUtils.loadLibrary();
    }

    /**
     * Creates a new decompressor.
     */
    @SuppressWarnings("this-escape")
    public ZstdInflater() {
        this.zsRef = new ZstdZStreamRef(this, init());
    }

    /**
     * Sets input data for decompression.
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
     * Sets input data for decompression.
     * @param input the input data bytes
     */
    public void setInput(byte[] input) {
        setInput(input, 0, input.length);
    }

    /**
     * Sets input data for decompression.
     * @param input the input data bytes
     * @since 25
     */
    public void setInput(ByteBuffer input) {
        Preconditions.checkNotNull(input);
        synchronized (zsRef) {
            this.input = input;
            this.inputArray = null;
        }
    }

    /**
     * Returns true if the decompressor has reached the end of the input data.
     * @return true if the end of the input data has been reached
     */
    public boolean finished() {
        synchronized (zsRef) {
            return finished;
        }
    }

    /**
     * Returns true if the decompressor needs more input data.
     * @return true if more input data is required
     */
    public boolean needsInput() {
        synchronized (zsRef) {
            ByteBuffer input = this.input;
            return input == null ? inputLim == inputPos : !input.hasRemaining();
        }
    }

    /**
     * Returns true if the decompressor is in an error state.
     * @return true if the decompressor is in an error state
     */
    public boolean needsDictionary() {
        return false; // Zstd doesn't use dictionaries by default
    }

    /**
     * Decompresses the input data and fills specified buffer with decompressed
     * data. Returns actual number of bytes of decompressed data.
     * @param output the buffer for the decompressed data
     * @param off the start offset of the data
     * @param len the maximum number of bytes of decompressed data
     * @return the actual number of bytes of decompressed data written
     * @throws DataFormatException if a data format error occurs
     */
    public int inflate(byte[] output, int off, int len) throws DataFormatException {
        Preconditions.checkFromIndexSize(off, len, output.length, Preconditions.AIOOBE_FORMATTER);
        synchronized (zsRef) {
            ensureOpen();
            int result = inflate(zsRef.address(), inputArray, inputPos,
                inputLim - inputPos, output, off, len);
            if (result < 0) {
                throw new DataFormatException("Zstd decompression error: " + result);
            }
            this.bytesWritten += result;
            this.bytesRead += (inputLim - inputPos);
            inputPos = inputLim;
            if (result == 0 && needsInput()) {
                finished = true;
            }
            return result;
        }
    }

    /**
     * Decompresses the input data and fills specified buffer with decompressed
     * data. Returns actual number of bytes of decompressed data.
     * @param output the buffer for the decompressed data
     * @return the actual number of bytes of decompressed data written
     * @throws DataFormatException if a data format error occurs
     */
    public int inflate(byte[] output) throws DataFormatException {
        return inflate(output, 0, output.length);
    }

    /**
     * Decompresses the input data and fills the specified buffer with decompressed
     * data. Returns actual number of bytes of data decompressed.
     * @param output the buffer for the decompressed data
     * @return the actual number of bytes of decompressed data written
     * @throws ReadOnlyBufferException if the given output buffer is read-only
     * @throws DataFormatException if a data format error occurs
     * @since 25
     */
    public int inflate(ByteBuffer output) throws DataFormatException {
        Preconditions.checkNotNull(output);
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
                            bytesWritten = inflateBuffer(zsRef.address(),
                                inputAddr + input.position(), input.remaining(),
                                outputAddr + output.position(), output.remaining());
                        } finally {
                            NIO_ACCESS.releaseSession(output);
                        }
                    } finally {
                        NIO_ACCESS.releaseSession(input);
                    }
                    if (bytesWritten < 0) {
                        throw new DataFormatException("Zstd decompression error: " + bytesWritten);
                    }
                    input.position(input.position() + input.remaining());
                } else {
                    byte[] arr = ZipUtils.getBufferArray(input);
                    int offset = ZipUtils.getBufferOffset(input);
                    bytesWritten = inflate(zsRef.address(), arr, offset + input.position(),
                        input.remaining(),
                        ZipUtils.getBufferArray(output),
                        ZipUtils.getBufferOffset(output) + output.position(),
                        output.remaining());
                    if (bytesWritten < 0) {
                        throw new DataFormatException("Zstd decompression error: " + bytesWritten);
                    }
                    input.position(input.limit());
                }
            } else if (inputArray != null && inputLim > inputPos) {
                bytesWritten = inflate(zsRef.address(), inputArray, inputPos,
                    inputLim - inputPos,
                    ZipUtils.getBufferArray(output),
                    ZipUtils.getBufferOffset(output) + output.position(),
                    output.remaining());
                if (bytesWritten < 0) {
                    throw new DataFormatException("Zstd decompression error: " + bytesWritten);
                }
                inputPos = inputLim;
            } else {
                bytesWritten = 0;
            }
            this.bytesWritten += bytesWritten;
            this.bytesRead += (input != null ? input.remaining() : (inputLim - inputPos));
            if (bytesWritten == 0 && needsInput()) {
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
     * Ends decompression and releases associated resources.
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
     * Releases the resources associated with this decompressor.
     */
    @Override
    public void close() {
        end();
    }

    private void ensureOpen() {
        if (zsRef.address() == 0) {
            throw new IllegalStateException("Inflater is closed");
        }
    }

    // Native methods
    private native static long init();
    private native static int inflate(long zs, byte[] input, int inputOff, int inputLen,
                                      byte[] output, int outputOff, int outputLen);
    private native static int inflateBuffer(long zs, long inputAddr, int inputLen,
                                            long outputAddr, int outputLen);
    private native static void end(long zs);

    /**
     * Reference to the native Zstd stream, cleaned up when garbage collected.
     */
    private static class ZstdZStreamRef {
        private final Cleanable cleanable;
        private long address;

        ZstdZStreamRef(ZstdInflater inflater, long address) {
            this.address = address;
            this.cleanable = CleanerFactory.cleaner()
                .register(this, () -> {
                    synchronized (inflater.zsRef) {
                        if (address != 0) {
                            ZstdInflater.end(address);
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
