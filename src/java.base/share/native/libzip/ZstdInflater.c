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
 * Native method support for java.util.zip.ZstdInflater
 */

#include <stdio.h>
#include <stdlib.h>
#include "jlong.h"
#include "jni.h"
#include "jni_util.h"
#include <zstd.h>

JNIEXPORT jlong JNICALL
Java_java_util_zip_ZstdInflater_init(JNIEnv *env, jclass cls)
{
    ZSTD_DStream *stream = ZSTD_createDStream();
    if (stream == NULL) {
        JNU_ThrowOutOfMemoryError(env, 0);
        return jlong_zero;
    }

    size_t ret = ZSTD_initDStream(stream);
    if (ZSTD_isError(ret)) {
        ZSTD_freeDStream(stream);
        JNU_ThrowInternalError(env, "Failed to initialize Zstd decompressor");
        return jlong_zero;
    }

    return ptr_to_jlong(stream);
}

JNIEXPORT jint JNICALL
Java_java_util_zip_ZstdInflater_inflate(JNIEnv *env, jclass cls, jlong addr,
                                         jbyteArray input, jint inputOff, jint inputLen,
                                         jbyteArray output, jint outputOff, jint outputLen)
{
    ZSTD_DStream *stream = (ZSTD_DStream *) jlong_to_ptr(addr);
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

    size_t ret = ZSTD_decompressStream(stream, &outputBufStruct, &inputBufStruct);
    if (ZSTD_isError(ret)) {
        (*env)->ReleasePrimitiveArrayCritical(env, input, inputBuf, 0);
        (*env)->ReleasePrimitiveArrayCritical(env, output, outputBuf, 0);
        JNU_ThrowRuntimeException(env, (const char *)ZSTD_getErrorName(ret));
        return (jint)(-ret);
    }

    (*env)->ReleasePrimitiveArrayCritical(env, input, inputBuf, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, output, outputBuf, 0);

    return (jint)outputBufStruct.pos;
}

JNIEXPORT jint JNICALL
Java_java_util_zip_ZstdInflater_inflateBuffer(JNIEnv *env, jclass cls, jlong addr,
                                               jlong inputAddr, jint inputLen,
                                               jlong outputAddr, jint outputLen)
{
    ZSTD_DStream *stream = (ZSTD_DStream *) jlong_to_ptr(addr);
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

    size_t ret = ZSTD_decompressStream(stream, &outputBufStruct, &inputBufStruct);
    if (ZSTD_isError(ret)) {
        JNU_ThrowRuntimeException(env, (const char *)ZSTD_getErrorName(ret));
        return (jint)(-ret);
    }

    return (jint)outputBufStruct.pos;
}

JNIEXPORT void JNICALL
Java_java_util_zip_ZstdInflater_end(JNIEnv *env, jclass cls, jlong addr)
{
    ZSTD_DStream *stream = (ZSTD_DStream *) jlong_to_ptr(addr);
    if (stream != NULL) {
        ZSTD_freeDStream(stream);
    }
}
