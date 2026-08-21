/**
 * @file        rex/main_android.h
 * @brief       Android application bootstrap and device API level query
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for full license text.
 */

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <jni.h>

namespace rex {

// Initializes the process-wide Android state (API level, JavaVM, application
// context) from the main thread, then brings up the subsystems that need to
// resolve API-level-gated libc/libandroid symbols:
//   rex::memory::AndroidInitialize  - ASharedMemory_create (API 26+)
//   rex::thread::AndroidInitialize  - pthread_getname_np   (API 26+)
// Must be called before any other rex subsystem is used.
bool InitializeAndroidAppFromMainThread(JavaVM* vm, jobject application_context);
void ShutdownAndroidAppFromMainThread();

// Device API level (android_get_device_api_level), or -1 before
// InitializeAndroidAppFromMainThread has run.
//
// Callers rely on this being valid inside their own AndroidInitialize, so the
// API level is latched first, before those are invoked.
int GetAndroidApiLevel();

// The JavaVM and the application Context passed to
// InitializeAndroidAppFromMainThread, or null if not initialized.
JavaVM* GetAndroidJavaVM();
jobject GetAndroidApplicationContext();

// JNIEnv for the calling thread, attaching it to the VM on first use. Threads
// attached this way are detached by ShutdownAndroidAppFromMainThread.
JNIEnv* GetAndroidThreadJniEnv();

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
