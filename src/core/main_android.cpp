/**
 * @file        rex/core/main_android.cpp
 * @brief       Android application bootstrap and device API level query
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <rex/main_android.h>

#include <android/api-level.h>
#include <pthread.h>

#include <rex/assert.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>

namespace rex {

namespace {

int android_api_level_ = -1;
JavaVM* android_java_vm_ = nullptr;
jobject android_application_context_ = nullptr;

// Threads that GetAndroidThreadJniEnv attached, so they can be detached again.
// A thread that dies before shutdown detaches itself via the key destructor.
pthread_key_t android_jni_detach_key_;
bool android_jni_detach_key_created_ = false;

void DetachCurrentThreadFromJavaVM(void* value) {
  // Only ever registered with a non-null value, and only for threads this
  // translation unit attached itself.
  (void)value;
  if (android_java_vm_) {
    android_java_vm_->DetachCurrentThread();
  }
}

}  // namespace

bool InitializeAndroidAppFromMainThread(JavaVM* vm, jobject application_context) {
  assert_null(android_java_vm_);
  if (!vm) {
    return false;
  }

  // Latch the API level before anything else: the AndroidInitialize functions
  // below gate their dlsym lookups on GetAndroidApiLevel().
  android_api_level_ = android_get_device_api_level();
  if (android_api_level_ < 0) {
    return false;
  }

  android_java_vm_ = vm;

  JNIEnv* env = GetAndroidThreadJniEnv();
  if (!env) {
    android_java_vm_ = nullptr;
    android_api_level_ = -1;
    return false;
  }
  // The caller's local reference dies when it returns to Java, so keep our own.
  android_application_context_ =
      application_context ? env->NewGlobalRef(application_context) : nullptr;

  if (pthread_key_create(&android_jni_detach_key_, DetachCurrentThreadFromJavaVM) == 0) {
    android_jni_detach_key_created_ = true;
  }

  rex::memory::AndroidInitialize();
  rex::thread::AndroidInitialize();
  return true;
}

void ShutdownAndroidAppFromMainThread() {
  if (!android_java_vm_) {
    return;
  }

  rex::thread::AndroidShutdown();
  rex::memory::AndroidShutdown();

  if (android_application_context_) {
    JNIEnv* env = GetAndroidThreadJniEnv();
    if (env) {
      env->DeleteGlobalRef(android_application_context_);
    }
    android_application_context_ = nullptr;
  }

  if (android_jni_detach_key_created_) {
    pthread_key_delete(android_jni_detach_key_);
    android_jni_detach_key_created_ = false;
  }

  android_java_vm_->DetachCurrentThread();
  android_java_vm_ = nullptr;
  android_api_level_ = -1;
}

int GetAndroidApiLevel() { return android_api_level_; }

JavaVM* GetAndroidJavaVM() { return android_java_vm_; }

jobject GetAndroidApplicationContext() { return android_application_context_; }

JNIEnv* GetAndroidThreadJniEnv() {
  if (!android_java_vm_) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  jint status = android_java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (status == JNI_OK) {
    return env;
  }
  if (status != JNI_EDETACHED) {
    return nullptr;
  }
  if (android_java_vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    return nullptr;
  }
  // Detach on thread exit. Skipping this leaks a Java thread object per guest
  // thread that ever touches JNI.
  if (android_jni_detach_key_created_) {
    pthread_setspecific(android_jni_detach_key_, env);
  }
  return env;
}

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
