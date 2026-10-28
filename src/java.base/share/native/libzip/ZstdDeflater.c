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

/*
 * Native method support for java.util.zip.ZstdDeflater
 */

#include <stdio.h>
#include <stdlib.h>
#include "jlong.h"
#include "jni.h"
#include "jni_util.h"
#include <zstd.h>

/* Flush modes mapping */
#define ZSTD_NO_FLUSH   0
#define ZSTD_SYNC_FLUSH 1
#define ZSTD_FULL_FLUSH 2
#define ZSTD_FINISH     4

JNIEXPORT jlong JNICALL
Java_java_util_zip_ZstdDeflater_init(JNIEnv *env, jclass cls, jint level)
{
    ZSTD_CStream *stream = ZSTD_createCStream();
    if (stream == NULL) {
        JNU_ThrowOutOfMemoryError(env, 0);
        return jlong_zero;
    }

    int ret = ZSTD_initCStream(stream, level);
    if (ZSTD_isError(ret)) {
        ZSTD_freeCStream(stream);
        JNU_ThrowIllegalArgumentException(env, "Invalid compression level");
        return jlong_zero;
    }

    return ptr_to_jlong(stream);
}

JNIEXPORT jint JNICALL
Java_java_util_zip_ZstdDeflater_deflate(JNIEnv *env, jclass cls, jlong addr,
                                         jbyteArray input, jint inputOff, jint inputLen,
                                         jbyteArray output, jint outputOff, jint outputLen,
                                         jint flush)
{
    ZSTD_CStream *stream = (ZSTD_CStream *) jlong_to_ptr(addr);
    if (stream == NULL) {
        JNU_ThrowNullPointerException(env, "Null stream");
        return -1;
    }

    jbyte *inputBuf = (*env)->GetPrimitiveArrayCritical(env, input, 0);
    if (inputBuf == NULL) {
        return -1;
    }

    jbyte *outputBuf = (*env)->GetPrimitiveArrayCritical(env, output, 0);
    if (outputBuf == NULL) {
        (*env)->ReleasePrimitiveArrayCritical(env, input, inputBuf, 0);
        return -1;
    }

    ZSTD_inBuffer inputBufStruct = {
        (void *)(inputBuf + inputOff),
        inputLen,
        0
    };

    ZSTD_outBuffer outputBufStruct = {
        (void *)(outputBuf + outputOff),
        outputLen,
        0
    };

    int zstdFlush;
    switch (flush) {
        case ZSTD_FINISH:
            zstdFlush = ZSTD_e_end;
            break;
        case ZSTD_FULL_FLUSH:
            zstdFlush = ZSTD_e_flush;
            break;
        case ZSTD_SYNC_FLUSH:
            zstdFlush = ZSTD_e_flush;
            break;
        default:
            zstdFlush = ZSTD_e_continue;
            break;
    }

    size_t ret = ZSTD_compressStream2(stream, &outputBufStruct, &inputBufStruct, zstdFlush);
    if (ZSTD_isError(ret)) {
        (*env)->ReleasePrimitiveArrayCritical(env, input, inputBuf, 0);
        (*env)->ReleasePrimitiveArrayCritical(env, output, outputBuf, 0);
        JNU_ThrowRuntimeException(env, "Zstd compression error");
        return -1;
    }

    (*env)->ReleasePrimitiveArrayCritical(env, input, inputBuf, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, output, outputBuf, 0);

    return (jint)outputBufStruct.pos;
}

JNIEXPORT jint JNICALL
Java_java_util_zip_ZstdDeflater_deflateBuffer(JNIEnv *env, jclass cls, jlong addr,
                                               jlong inputAddr, jint inputLen,
                                               jlong outputAddr, jint outputLen,
                                               jint flush)
{
    ZSTD_CStream *stream = (ZSTD_CStream *) jlong_to_ptr(addr);
    if (stream == NULL) {
        JNU_ThrowNullPointerException(env, "Null stream");
        return -1;
    }

    ZSTD_inBuffer inputBufStruct = {
        (void *)(jlong_to_ptr(inputAddr)),
        inputLen,
        0
    };

    ZSTD_outBuffer outputBufStruct = {
        (void *)(jlong_to_ptr(outputAddr)),
        outputLen,
        0
    };

    int zstdFlush;
    switch (flush) {
        case ZSTD_FINISH:
            zstdFlush = ZSTD_e_end;
            break;
        case ZSTD_FULL_FLUSH:
            zstdFlush = ZSTD_e_flush;
            break;
        case ZSTD_SYNC_FLUSH:
            zstdFlush = ZSTD_e_flush;
            break;
        default:
            zstdFlush = ZSTD_e_continue;
            break;
    }

    size_t ret = ZSTD_compressStream2(stream, &outputBufStruct, &inputBufStruct, zstdFlush);
    if (ZSTD_isError(ret)) {
        JNU_ThrowRuntimeException(env, "Zstd compression error");
        return -1;
    }

    return (jint)outputBufStruct.pos;
}

JNIEXPORT void JNICALL
Java_java_util_zip_ZstdDeflater_end(JNIEnv *env, jclass cls, jlong addr)
{
    ZSTD_CStream *stream = (ZSTD_CStream *) jlong_to_ptr(addr);
    if (stream != NULL) {
        ZSTD_freeCStream(stream);
    }
}
