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

#ifndef _JAVASOFT_JNI_MD_H_
#define _JAVASOFT_JNI_MD_H_

/* Cross-platform unified machine-dependent JNI definitions for oneJDK Native SDK */
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifndef JNIEXPORT
    #define JNIEXPORT __declspec(dllexport)
  #endif
  #ifndef JNIIMPORT
    #define JNIIMPORT __declspec(dllimport)
  #endif
  #ifndef JNICALL
    #define JNICALL __stdcall
  #endif
  typedef int jint;
  typedef long long jlong;
  typedef signed char jbyte;
#else
  #ifndef __has_attribute
    #define __has_attribute(x) 0
  #endif
  #ifndef JNIEXPORT
    #if (defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 2)))) || __has_attribute(visibility)
      #define JNIEXPORT __attribute__((visibility("default")))
    #else
      #define JNIEXPORT
    #endif
  #endif
  #ifndef JNIIMPORT
    #if (defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 2)))) || __has_attribute(visibility)
      #define JNIIMPORT __attribute__((visibility("default")))
    #else
      #define JNIIMPORT
    #endif
  #endif
  #ifndef JNICALL
    #define JNICALL
  #endif
  typedef int jint;
  #ifdef _LP64
    typedef long jlong;
  #else
    typedef long long jlong;
  #endif
  typedef signed char jbyte;
#endif

#endif /* !_JAVASOFT_JNI_MD_H_ */
