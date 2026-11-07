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

#ifndef _JAVA_JVMTI_H_
#define _JAVA_JVMTI_H_

#include "jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * JVM Tool Interface (JVMTI) — Consolidated OneJDK-25 Native SDK Header
 * ========================================================================= */

enum {
  JVMTI_VERSION_1   = 0x30010000,
  JVMTI_VERSION_1_1 = 0x30010100,
  JVMTI_VERSION_1_2 = 0x30010200,
  JVMTI_VERSION_9   = 0x30090000,
  JVMTI_VERSION_11  = 0x300B0000,
  JVMTI_VERSION_19  = 0x30130000,
  JVMTI_VERSION_21  = 0x30150000,
  JVMTI_VERSION_25  = 0x30190000,
  JVMTI_VERSION     = 0x30190000
};

typedef jobject jthread;
typedef jobject jthreadGroup;
typedef jlong   jlocation;
struct _jrawMonitorID;
typedef struct _jrawMonitorID* jrawMonitorID;
typedef struct JNINativeInterface_ jniNativeInterface;

typedef enum {
  JVMTI_ERROR_NONE                                   = 0,
  JVMTI_ERROR_INVALID_THREAD                         = 10,
  JVMTI_ERROR_INVALID_THREAD_GROUP                   = 11,
  JVMTI_ERROR_INVALID_PRIORITY                       = 12,
  JVMTI_ERROR_THREAD_NOT_SUSPENDED                   = 13,
  JVMTI_ERROR_THREAD_SUSPENDED                       = 14,
  JVMTI_ERROR_THREAD_NOT_ALIVE                       = 15,
  JVMTI_ERROR_INVALID_OBJECT                         = 20,
  JVMTI_ERROR_INVALID_CLASS                          = 21,
  JVMTI_ERROR_CLASS_NOT_PREPARED                     = 22,
  JVMTI_ERROR_INVALID_METHODID                       = 23,
  JVMTI_ERROR_INVALID_LOCATION                       = 24,
  JVMTI_ERROR_INVALID_FIELDID                        = 25,
  JVMTI_ERROR_INVALID_MODULE                         = 26,
  JVMTI_ERROR_NO_MORE_FRAMES                         = 31,
  JVMTI_ERROR_OPAQUE_FRAME                           = 32,
  JVMTI_ERROR_TYPE_MISMATCH                          = 34,
  JVMTI_ERROR_INVALID_SLOT                           = 35,
  JVMTI_ERROR_DUPLICATE                              = 40,
  JVMTI_ERROR_NOT_FOUND                              = 41,
  JVMTI_ERROR_INVALID_MONITOR                        = 50,
  JVMTI_ERROR_NOT_MONITOR_OWNER                      = 51,
  JVMTI_ERROR_INTERRUPT                              = 52,
  JVMTI_ERROR_INVALID_CLASS_FORMAT                   = 60,
  JVMTI_ERROR_CIRCULAR_CLASS_DEFINITION              = 61,
  JVMTI_ERROR_FAILS_VERIFICATION                     = 62,
  JVMTI_ERROR_UNSUPPORTED_REDEFINITION_METHOD_ADDED  = 63,
  JVMTI_ERROR_INVALID_TYPESTATE                      = 65,
  JVMTI_ERROR_UNSUPPORTED_VERSION                    = 68,
  JVMTI_ERROR_NAMES_DONT_MATCH                       = 69,
  JVMTI_ERROR_UNMODIFIABLE_CLASS                     = 79,
  JVMTI_ERROR_UNMODIFIABLE_MODULE                    = 80,
  JVMTI_ERROR_NOT_AVAILABLE                          = 98,
  JVMTI_ERROR_MUST_POSSESS_CAPABILITY                = 99,
  JVMTI_ERROR_NULL_POINTER                           = 100,
  JVMTI_ERROR_ABSENT_INFORMATION                     = 101,
  JVMTI_ERROR_INVALID_EVENT_TYPE                     = 102,
  JVMTI_ERROR_ILLEGAL_ARGUMENT                       = 103,
  JVMTI_ERROR_NATIVE_METHOD                          = 104,
  JVMTI_ERROR_CLASS_LOADER_UNSUPPORTED               = 106,
  JVMTI_ERROR_OUT_OF_MEMORY                          = 110,
  JVMTI_ERROR_ACCESS_DENIED                          = 111,
  JVMTI_ERROR_WRONG_PHASE                            = 112,
  JVMTI_ERROR_INTERNAL                               = 113,
  JVMTI_ERROR_UNATTACHED_THREAD                      = 115,
  JVMTI_ERROR_INVALID_ENVIRONMENT                    = 116
} jvmtiError;

typedef enum {
  JVMTI_PHASE_ONLOAD   = 1,
  JVMTI_PHASE_PRIMORDIAL = 2,
  JVMTI_PHASE_START    = 6,
  JVMTI_PHASE_LIVE     = 4,
  JVMTI_PHASE_DEAD     = 8
} jvmtiPhase;

typedef enum {
  JVMTI_ENABLE  = 1,
  JVMTI_DISABLE = 0
} jvmtiEventMode;

typedef enum {
  JVMTI_EVENT_VM_INIT                   = 50,
  JVMTI_EVENT_VM_DEATH                  = 51,
  JVMTI_EVENT_THREAD_START              = 52,
  JVMTI_EVENT_THREAD_END                = 53,
  JVMTI_EVENT_CLASS_FILE_LOAD_HOOK      = 54,
  JVMTI_EVENT_CLASS_LOAD                = 55,
  JVMTI_EVENT_CLASS_PREPARE             = 56,
  JVMTI_EVENT_EXCEPTION                 = 58,
  JVMTI_EVENT_SINGLE_STEP               = 60,
  JVMTI_EVENT_BREAKPOINT                = 62,
  JVMTI_EVENT_METHOD_ENTRY              = 65,
  JVMTI_EVENT_METHOD_EXIT               = 66,
  JVMTI_EVENT_COMPILED_METHOD_LOAD      = 68,
  JVMTI_EVENT_COMPILED_METHOD_UNLOAD    = 69,
  JVMTI_EVENT_DYNAMIC_CODE_GENERATED    = 70,
  JVMTI_EVENT_GARBAGE_COLLECTION_START  = 81,
  JVMTI_EVENT_GARBAGE_COLLECTION_FINISH = 82,
  JVMTI_EVENT_OBJECT_FREE               = 83,
  JVMTI_EVENT_VM_OBJECT_ALLOC           = 84,
  JVMTI_EVENT_SAMPLED_OBJECT_ALLOC      = 86,
  JVMTI_EVENT_VIRTUAL_THREAD_START      = 87,
  JVMTI_EVENT_VIRTUAL_THREAD_END        = 88
} jvmtiEvent;

typedef struct {
  unsigned int can_tag_objects : 1;
  unsigned int can_generate_field_modification_events : 1;
  unsigned int can_generate_field_access_events : 1;
  unsigned int can_get_bytecodes : 1;
  unsigned int can_get_synthetic_attribute : 1;
  unsigned int can_get_owned_monitor_info : 1;
  unsigned int can_get_current_contended_monitor : 1;
  unsigned int can_get_monitor_info : 1;
  unsigned int can_pop_frame : 1;
  unsigned int can_redefine_classes : 1;
  unsigned int can_signal_thread : 1;
  unsigned int can_get_source_file_name : 1;
  unsigned int can_get_line_numbers : 1;
  unsigned int can_get_source_debug_extension : 1;
  unsigned int can_access_local_variables : 1;
  unsigned int can_maintain_original_method_order : 1;
  unsigned int can_generate_single_step_events : 1;
  unsigned int can_generate_exception_events : 1;
  unsigned int can_generate_frame_pop_events : 1;
  unsigned int can_generate_breakpoint_events : 1;
  unsigned int can_suspend : 1;
  unsigned int can_redefine_any_class : 1;
  unsigned int can_get_current_thread_cpu_time : 1;
  unsigned int can_get_thread_cpu_time : 1;
  unsigned int can_generate_method_entry_events : 1;
  unsigned int can_generate_method_exit_events : 1;
  unsigned int can_generate_all_class_hook_events : 1;
  unsigned int can_generate_compiled_method_load_events : 1;
  unsigned int can_generate_monitor_events : 1;
  unsigned int can_generate_vm_object_alloc_events : 1;
  unsigned int can_generate_native_method_bind_events : 1;
  unsigned int can_generate_garbage_collection_events : 1;
  unsigned int can_generate_object_free_events : 1;
  unsigned int can_force_early_return : 1;
  unsigned int can_get_owned_monitor_stack_depth_info : 1;
  unsigned int can_get_constant_pool : 1;
  unsigned int can_set_native_method_prefix : 1;
  unsigned int can_retransform_classes : 1;
  unsigned int can_retransform_any_class : 1;
  unsigned int can_generate_resource_exhaustion_heap_events : 1;
  unsigned int can_generate_resource_exhaustion_threads_events : 1;
  unsigned int can_generate_early_vmstart : 1;
  unsigned int can_generate_early_class_hook_events : 1;
  unsigned int can_generate_sampled_object_alloc_events : 1;
  unsigned int can_support_virtual_threads : 1;
  unsigned int : 19;
  unsigned int : 32;
  unsigned int : 32;
} jvmtiCapabilities;

typedef struct {
  jlocation start_location;
  const void* address;
} jvmtiAddrLocationMap;

struct _jvmtiEnv;
struct jvmtiInterface_1_;

#ifdef __cplusplus
typedef _jvmtiEnv jvmtiEnv;
#else
typedef const struct jvmtiInterface_1_* jvmtiEnv;
#endif

typedef struct jvmtiEventCallbacks {
  void (JNICALL *VMInit)(jvmtiEnv *jvmti_env, JNIEnv* jni_env, jthread thread);
  void (JNICALL *VMDeath)(jvmtiEnv *jvmti_env, JNIEnv* jni_env);
  void (JNICALL *ThreadStart)(jvmtiEnv *jvmti_env, JNIEnv* jni_env, jthread thread);
  void (JNICALL *ThreadEnd)(jvmtiEnv *jvmti_env, JNIEnv* jni_env, jthread thread);
  void (JNICALL *CompiledMethodLoad)(jvmtiEnv *jvmti_env, jmethodID method, jint code_size,
                                     const void* code_addr, jint map_length,
                                     const jvmtiAddrLocationMap* map, const void* compile_info);
  void (JNICALL *CompiledMethodUnload)(jvmtiEnv *jvmti_env, jmethodID method, const void* code_addr);
  void (JNICALL *DynamicCodeGenerated)(jvmtiEnv *jvmti_env, const char* name, const void* address, jint length);
  void (JNICALL *GarbageCollectionStart)(jvmtiEnv *jvmti_env);
  void (JNICALL *GarbageCollectionFinish)(jvmtiEnv *jvmti_env);
} jvmtiEventCallbacks;

typedef struct jvmtiInterface_1_ {
  void* reserved1;
  jvmtiError (JNICALL *SetEventNotificationMode)(jvmtiEnv* env, jvmtiEventMode mode,
                                                 jvmtiEvent event_type, jthread event_thread, ...);
  jvmtiError (JNICALL *Allocate)(jvmtiEnv* env, jlong size, unsigned char** mem_ptr);
  jvmtiError (JNICALL *Deallocate)(jvmtiEnv* env, unsigned char* mem);
  jvmtiError (JNICALL *AddCapabilities)(jvmtiEnv* env, const jvmtiCapabilities* capabilities_ptr);
  jvmtiError (JNICALL *GetCapabilities)(jvmtiEnv* env, jvmtiCapabilities* capabilities_ptr);
  jvmtiError (JNICALL *SetEventCallbacks)(jvmtiEnv* env, const jvmtiEventCallbacks* callbacks, jint size_of_callbacks);
  jvmtiError (JNICALL *GetPhase)(jvmtiEnv* env, jvmtiPhase* phase_ptr);
  jvmtiError (JNICALL *GetVersionNumber)(jvmtiEnv* env, jint* version_ptr);
} jvmtiInterface_1;

struct _jvmtiEnv {
  const struct jvmtiInterface_1_* functions;
#ifdef __cplusplus
  jvmtiError Allocate(jlong size, unsigned char** mem_ptr) {
    return functions->Allocate(this, size, mem_ptr);
  }
  jvmtiError Deallocate(unsigned char* mem) {
    return functions->Deallocate(this, mem);
  }
  jvmtiError AddCapabilities(const jvmtiCapabilities* capabilities_ptr) {
    return functions->AddCapabilities(this, capabilities_ptr);
  }
  jvmtiError GetCapabilities(jvmtiCapabilities* capabilities_ptr) {
    return functions->GetCapabilities(this, capabilities_ptr);
  }
  jvmtiError SetEventCallbacks(const jvmtiEventCallbacks* callbacks, jint size_of_callbacks) {
    return functions->SetEventCallbacks(this, callbacks, size_of_callbacks);
  }
  jvmtiError SetEventNotificationMode(jvmtiEventMode mode, jvmtiEvent event_type, jthread event_thread, ...) {
    return functions->SetEventNotificationMode(this, mode, event_type, event_thread);
  }
  jvmtiError GetPhase(jvmtiPhase* phase_ptr) {
    return functions->GetPhase(this, phase_ptr);
  }
  jvmtiError GetVersionNumber(jint* version_ptr) {
    return functions->GetVersionNumber(this, version_ptr);
  }
#endif
};

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved);
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options, void *reserved);
JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* !_JAVA_JVMTI_H_ */
