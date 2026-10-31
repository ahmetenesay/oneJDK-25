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

#include "jni.h"
#include "prims/jni.hpp"
#include "runtime/javaThreads.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/thread.hpp"

// oneTBB headers (statically linked into libjvm)
#include <oneapi/tbb/version.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/scalable_allocator.h>
#include <oneapi/tbb/tick_count.h>

// ============================================================================
// JNI Method Signatures for jdk.internal.tbb.TBBSession
// ============================================================================

namespace TBBJNI {

// ============================================================================
// 1. Version Information (version.h)
// ============================================================================

static jint TBB_versionMajor(JNIEnv* /*env*/, jclass /*cls*/) {
    return __TBB_VERSION_MAJOR;
}

static jint TBB_versionMinor(JNIEnv* /*env*/, jclass /*cls*/) {
    return __TBB_VERSION_MINOR;
}

static jint TBB_interfaceVersion(JNIEnv* /*env*/, jclass /*cls*/) {
    return __TBB_INTERFACE_VERSION;
}

static jstring TBB_runtimeVersionString(JNIEnv* env, jclass /*cls*/) {
    const char* ver = oneapi::tbb::version::runtime();
    return env->NewStringUTF(ver ? ver : "");
}

static jint TBB_runtimeInterfaceVersion(JNIEnv* /*env*/, jclass /*cls*/) {
    return oneapi::tbb::version::runtime_interface();
}

// ============================================================================
// 2. CPU Topology (info.h)
// ============================================================================

static jint TBB_numaNodeCount(JNIEnv* /*env*/, jclass /*cls*/) {
    return (jint)oneapi::tbb::info::numa_node_count();
}

static jint TBB_numaDefaultConcurrency(JNIEnv* /*env*/, jclass /*cls*/, jint numaId) {
    return (jint)oneapi::tbb::info::default_concurrency((int)numaId);
}

static jint TBB_coreTypeCount(JNIEnv* /*env*/, jclass /*cls*/) {
    std::vector<oneapi::tbb::info::topology::info_core_type> core_types =
        oneapi::tbb::info::core_types();
    return (jint)core_types.size();
}

static jint TBB_defaultConcurrency(JNIEnv* /*env*/, jclass /*cls*/) {
    return (jint)oneapi::tbb::info::default_concurrency();
}

// ============================================================================
// 3. Global Control (global_control.h)
// ============================================================================

static void TBB_setGlobalControl(JNIEnv* /*env*/, jclass /*cls*/, jint parameter, jlong value) {
    switch (parameter) {
        case 0: // MAX_ALLOWED_PARALLELISM
            new oneapi::tbb::global_control(
                oneapi::tbb::global_control::max_allowed_parallelism, (size_t)value);
            break;
        case 1: // THREAD_STACK_SIZE
            new oneapi::tbb::global_control(
                oneapi::tbb::global_control::threading_stack_size, (size_t)value);
            break;
        case 2: // TERMINATE_ON_EXCEPTION
            new oneapi::tbb::global_control(
                oneapi::tbb::global_control::terminate_on_exception, (size_t)value);
            break;
    }
}

static jlong TBB_activeValue(JNIEnv* /*env*/, jclass /*cls*/, jint parameter) {
    switch (parameter) {
        case 0: // MAX_ALLOWED_PARALLELISM
            return (jlong)oneapi::tbb::global_control::active_value(
                oneapi::tbb::global_control::max_allowed_parallelism);
        case 1: // THREAD_STACK_SIZE
            return (jlong)oneapi::tbb::global_control::active_value(
                oneapi::tbb::global_control::threading_stack_size);
        case 2: // TERMINATE_ON_EXCEPTION
            return (jlong)oneapi::tbb::global_control::active_value(
                oneapi::tbb::global_control::terminate_on_exception);
        default:
            return 0;
    }
}

// ============================================================================
// 4. Task Arena (task_arena.h)
// ============================================================================

static jlong TBB_createArena(JNIEnv* /*env*/, jclass /*cls*/,
                              jint maxConcurrency, jint reservedForMasters, jint priority) {
    oneapi::tbb::task_arena::constraints constraints;
    constraints.max_threads_per_core = 1;
    constraints.priority = (oneapi::tbb::task_arena::priority)priority;
    
    oneapi::tbb::task_arena* arena = new oneapi::tbb::task_arena(
        (int)maxConcurrency, (int)reservedForMasters, constraints);
    arena->initialize();
    return (jlong)arena;
}

static void TBB_closeArena(JNIEnv* /*env*/, jclass /*cls*/, jlong arenaHandle) {
    oneapi::tbb::task_arena* arena = (oneapi::tbb::task_arena*)arenaHandle;
    if (arena) {
        delete arena;
    }
}

static jint TBB_arenaMaxConcurrency(JNIEnv* /*env*/, jclass /*cls*/, jlong arenaHandle) {
    oneapi::tbb::task_arena* arena = (oneapi::tbb::task_arena*)arenaHandle;
    return arena ? (jint)arena->max_concurrency() : 0;
}

static jboolean TBB_arenaIsActive(JNIEnv* /*env*/, jclass /*cls*/, jlong arenaHandle) {
    oneapi::tbb::task_arena* arena = (oneapi::tbb::task_arena*)arenaHandle;
    return arena ? JNI_TRUE : JNI_FALSE;
}

static jint TBB_currentThreadIndex(JNIEnv* /*env*/, jclass /*cls*/) {
    return oneapi::tbb::this_task_arena::current_thread_index();
}

static jint TBB_maxConcurrency(JNIEnv* /*env*/, jclass /*cls*/) {
    return oneapi::tbb::this_task_arena::max_concurrency();
}

// ============================================================================
// 5. tbbmalloc — Scalable Memory Allocation (scalable_allocator.h)
// ============================================================================

static jlong TBB_scalableMalloc(JNIEnv* /*env*/, jclass /*cls*/, jlong size) {
    void* ptr = scalable_malloc((size_t)size);
    return (jlong)ptr;
}

static void TBB_scalableFree(JNIEnv* /*env*/, jclass /*cls*/, jlong ptr) {
    scalable_free((void*)ptr);
}

static jlong TBB_scalableRealloc(JNIEnv* /*env*/, jclass /*cls*/, jlong ptr, jlong size) {
    void* result = scalable_realloc((void*)ptr, (size_t)size);
    return (jlong)result;
}

static jlong TBB_scalableCalloc(JNIEnv* /*env*/, jclass /*cls*/, jlong nobj, jlong size) {
    void* ptr = scalable_calloc((size_t)nobj, (size_t)size);
    return (jlong)ptr;
}

static jlong TBB_scalableAlignedMalloc(JNIEnv* /*env*/, jclass /*cls*/, jlong size, jlong alignment) {
    void* ptr = scalable_aligned_malloc((size_t)size, (size_t)alignment);
    return (jlong)ptr;
}

static jlong TBB_scalableAlignedRealloc(JNIEnv* /*env*/, jclass /*cls*/,
                                         jlong ptr, jlong size, jlong alignment) {
    void* result = scalable_aligned_realloc((void*)ptr, (size_t)size, (size_t)alignment);
    return (jlong)result;
}

static void TBB_scalableAlignedFree(JNIEnv* /*env*/, jclass /*cls*/, jlong ptr) {
    scalable_aligned_free((void*)ptr);
}

static jlong TBB_scalableMsize(JNIEnv* /*env*/, jclass /*cls*/, jlong ptr) {
    return (jlong)scalable_msize((void*)ptr);
}

static jlong TBB_scalablePosixMemAlign(JNIEnv* /*env*/, jclass /*cls*/, jlong alignment, jlong size) {
    void* ptr = NULL;
    int ret = scalable_posix_memalign(&ptr, (size_t)alignment, (size_t)size);
    return (ret == 0) ? (jlong)ptr : 0;
}

// ============================================================================
// 6. tbbmalloc — Allocation Modes (scalable_allocator.h)
// ============================================================================

static jint TBB_scalableAllocationMode(JNIEnv* /*env*/, jclass /*cls*/, jint param, jlong value) {
    return scalable_allocation_mode((int)param, (size_t)value);
}

static jint TBB_scalableAllocationCommand(JNIEnv* /*env*/, jclass /*cls*/, jint cmd, jlong /*param*/) {
    return scalable_allocation_command((int)cmd, NULL);
}

// ============================================================================
// 7. tbbmalloc — Memory Pools (rml::pool_* from scalable_allocator.h)
// ============================================================================

static jlong TBB_poolCreate(JNIEnv* /*env*/, jclass /*cls*/,
                             jlong granularity, jboolean fixedPool, jboolean keepAllMemory) {
    rml::MemoryPool* pool = nullptr;
    rml::MemPoolPolicy policy(nullptr, nullptr,
                              (std::size_t)granularity,
                              fixedPool, keepAllMemory);
    rml::MemPoolError err = rml::pool_create_v1(0, &policy, &pool);
    return (err == rml::POOL_OK) ? (jlong)pool : 0;
}

static void TBB_poolDestroy(JNIEnv* /*env*/, jclass /*cls*/, jlong poolHandle) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    if (pool) {
        rml::pool_destroy(pool);
    }
}

static jlong TBB_poolMalloc(JNIEnv* /*env*/, jclass /*cls*/, jlong poolHandle, jlong size) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    void* ptr = pool ? rml::pool_malloc(pool, (std::size_t)size) : nullptr;
    return (jlong)ptr;
}

static jlong TBB_poolRealloc(JNIEnv* /*env*/, jclass /*cls*/,
                              jlong poolHandle, jlong ptr, jlong size) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    void* result = pool ? rml::pool_realloc(pool, (void*)ptr, (std::size_t)size) : nullptr;
    return (jlong)result;
}

static jlong TBB_poolAlignedMalloc(JNIEnv* /*env*/, jclass /*cls*/,
                                    jlong poolHandle, jlong size, jlong alignment) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    void* ptr = pool ? rml::pool_aligned_malloc(pool, (std::size_t)size, (std::size_t)alignment) : nullptr;
    return (jlong)ptr;
}

static jlong TBB_poolAlignedRealloc(JNIEnv* /*env*/, jclass /*cls*/,
                                     jlong poolHandle, jlong ptr, jlong size, jlong alignment) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    void* result = pool ? rml::pool_aligned_realloc(pool, (void*)ptr,
                                                     (std::size_t)size, (std::size_t)alignment) : nullptr;
    return (jlong)result;
}

static jboolean TBB_poolFree(JNIEnv* /*env*/, jclass /*cls*/, jlong poolHandle, jlong ptr) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    bool result = pool ? rml::pool_free(pool, (void*)ptr) : false;
    return result ? JNI_TRUE : JNI_FALSE;
}

static jboolean TBB_poolReset(JNIEnv* /*env*/, jclass /*cls*/, jlong poolHandle) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    bool result = pool ? rml::pool_reset(pool) : false;
    return result ? JNI_TRUE : JNI_FALSE;
}

static jlong TBB_poolMsize(JNIEnv* /*env*/, jclass /*cls*/, jlong poolHandle, jlong ptr) {
    rml::MemoryPool* pool = (rml::MemoryPool*)poolHandle;
    return pool ? (jlong)rml::pool_msize(pool, (void*)ptr) : 0;
}

static jlong TBB_poolIdentify(JNIEnv* /*env*/, jclass /*cls*/, jlong ptr) {
    rml::MemoryPool* pool = rml::pool_identify((void*)ptr);
    return (jlong)pool;
}

// ============================================================================
// 8. Timing (tick_count.h)
// ============================================================================

static jdouble TBB_tickCount(JNIEnv* /*env*/, jclass /*cls*/) {
    return (jdouble)oneapi::tbb::tick_count::now().seconds();
}

static jdouble TBB_ticksPerSecond(JNIEnv* /*env*/, jclass /*cls*/) {
    return (jdouble)oneapi::tbb::tick_count::ticks_per_second();
}

// ============================================================================
// 9. Task Scheduler Handle (global_control.h)
// ============================================================================

static jlong TBB_getTaskSchedulerHandle(JNIEnv* /*env*/, jclass /*cls*/) {
    // TBB's task_scheduler_handle keeps the scheduler alive
    // Using a simple wrapper around the concept
    return (jlong)new int(1);  // Opaque handle
}

static void TBB_releaseTaskSchedulerHandle(JNIEnv* /*env*/, jclass /*cls*/, jlong handle) {
    int* h = (int*)handle;
    delete h;
}

// ============================================================================
// 10. Availability
// ============================================================================

static jboolean TBB_isAvailable(JNIEnv* /*env*/, jclass /*cls*/) {
    // If we reached here, TBB is available
    return JNI_TRUE;
}

// ============================================================================
// JNI Method Registration Table
// ============================================================================

static JNINativeMethod TBBSession_methods[] = {
    // registerNatives
    {"registerNatives", "()V", (void*)TBB_isAvailable},  // No-op placeholder

    // 1. Version Information
    {"nativeVersionMajor", "()I", (void*)TBB_versionMajor},
    {"nativeVersionMinor", "()I", (void*)TBB_versionMinor},
    {"nativeInterfaceVersion", "()I", (void*)TBB_interfaceVersion},
    {"nativeRuntimeVersionString", "()Ljava/lang/String;", (void*)TBB_runtimeVersionString},
    {"nativeRuntimeInterfaceVersion", "()I", (void*)TBB_runtimeInterfaceVersion},

    // 2. CPU Topology (info.h)
    {"nativeNumaNodeCount", "()I", (void*)TBB_numaNodeCount},
    {"nativeNumaDefaultConcurrency", "(I)I", (void*)TBB_numaDefaultConcurrency},
    {"nativeCoreTypeCount", "()I", (void*)TBB_coreTypeCount},
    {"nativeDefaultConcurrency", "()I", (void*)TBB_defaultConcurrency},

    // 3. Global Control (global_control.h)
    {"nativeSetGlobalControl", "(IJ)V", (void*)TBB_setGlobalControl},
    {"nativeActiveValue", "(I)J", (void*)TBB_activeValue},

    // 4. Task Arena (task_arena.h)
    {"nativeCreateArena", "(III)J", (void*)TBB_createArena},
    {"nativeCloseArena", "(J)V", (void*)TBB_closeArena},
    {"nativeArenaMaxConcurrency", "(J)I", (void*)TBB_arenaMaxConcurrency},
    {"nativeArenaIsActive", "(J)Z", (void*)TBB_arenaIsActive},
    {"nativeCurrentThreadIndex", "()I", (void*)TBB_currentThreadIndex},
    {"nativeMaxConcurrency", "()I", (void*)TBB_maxConcurrency},

    // 5. tbbmalloc — Scalable Memory (scalable_allocator.h)
    {"nativeScalableMalloc", "(J)J", (void*)TBB_scalableMalloc},
    {"nativeScalableFree", "(J)V", (void*)TBB_scalableFree},
    {"nativeScalableRealloc", "(JJ)J", (void*)TBB_scalableRealloc},
    {"nativeScalableCalloc", "(JJ)J", (void*)TBB_scalableCalloc},
    {"nativeScalableAlignedMalloc", "(JJ)J", (void*)TBB_scalableAlignedMalloc},
    {"nativeScalableAlignedRealloc", "(JJJ)J", (void*)TBB_scalableAlignedRealloc},
    {"nativeScalableAlignedFree", "(J)V", (void*)TBB_scalableAlignedFree},
    {"nativeScalableMsize", "(J)J", (void*)TBB_scalableMsize},
    {"nativeScalablePosixMemAlign", "(JJ)J", (void*)TBB_scalablePosixMemAlign},

    // 6. tbbmalloc — Allocation Modes
    {"nativeScalableAllocationMode", "(IJ)I", (void*)TBB_scalableAllocationMode},
    {"nativeScalableAllocationCommand", "(II)I", (void*)TBB_scalableAllocationCommand},

    // 7. tbbmalloc — Memory Pools
    {"nativePoolCreate", "(JZZ)J", (void*)TBB_poolCreate},
    {"nativePoolDestroy", "(J)V", (void*)TBB_poolDestroy},
    {"nativePoolMalloc", "(JJ)J", (void*)TBB_poolMalloc},
    {"nativePoolRealloc", "(JJJ)J", (void*)TBB_poolRealloc},
    {"nativePoolAlignedMalloc", "(JJJ)J", (void*)TBB_poolAlignedMalloc},
    {"nativePoolAlignedRealloc", "(JJJJ)J", (void*)TBB_poolAlignedRealloc},
    {"nativePoolFree", "(JJ)Z", (void*)TBB_poolFree},
    {"nativePoolReset", "(J)Z", (void*)TBB_poolReset},
    {"nativePoolMsize", "(JJ)J", (void*)TBB_poolMsize},
    {"nativePoolIdentify", "(J)J", (void*)TBB_poolIdentify},

    // 8. Timing (tick_count.h)
    {"nativeTickCount", "()D", (void*)TBB_tickCount},
    {"nativeTicksPerSecond", "()D", (void*)TBB_ticksPerSecond},

    // 9. Task Scheduler Handle
    {"nativeGetTaskSchedulerHandle", "()J", (void*)TBB_getTaskSchedulerHandle},
    {"nativeReleaseTaskSchedulerHandle", "(J)V", (void*)TBB_releaseTaskSchedulerHandle},

    // 10. Availability
    {"nativeIsAvailable", "()Z", (void*)TBB_isAvailable},
};

// ============================================================================
// Registration Entry Point
// ============================================================================

// Called by JVM during class initialization
int register_jdk_internal_tbb_TBBSession(JNIEnv* env) {
    jclass cls;
    cls = JNIHandles::make_local(env,
        JNIHandles::make_global(env,
            env->FindClass("jdk/internal/tbb/TBBSession")));
    
    if (cls == NULL) {
        return JNI_ERR;
    }
    
    return JNI_RegisterNatives(cls, TBBSession_methods,
        sizeof(TBBSession_methods) / sizeof(TBBSession_methods[0]));
}

} // namespace TBBJNI
