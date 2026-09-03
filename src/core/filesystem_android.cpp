/**
 * @file        rex/core/filesystem_android.cpp
 * @brief       Android content:// URI support
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <rex/filesystem.h>

#include <jni.h>

#include <string>

#include <rex/logging.h>
#include <rex/main_android.h>

namespace rex {
namespace filesystem {

void AndroidInitialize() {}
void AndroidShutdown() {}

bool IsAndroidContentUri(const std::string_view source) {
  // Scheme comparison is case-insensitive per RFC 3986, and Android's own
  // ContentResolver accepts any casing.
  constexpr std::string_view kScheme = "content://";
  if (source.size() < kScheme.size()) {
    return false;
  }
  for (size_t i = 0; i < kScheme.size(); ++i) {
    char c = source[i];
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
    if (c != kScheme[i]) {
      return false;
    }
  }
  return true;
}

namespace {

// Drops a pending Java exception, logging it, and reports whether there was one.
bool ClearJavaException(JNIEnv* env, const char* what) {
  if (!env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionDescribe();
  env->ExceptionClear();
  REXFS_ERROR("OpenAndroidContentFileDescriptor: {} threw", what);
  return true;
}

// fopen-style modes ("rb", "r+", "wb") to ParcelFileDescriptor's ("r", "rw", "w").
const char* ContentResolverMode(const char* mode) {
  if (!mode) {
    return "r";
  }
  const std::string_view m(mode);
  const bool reads = m.find('r') != std::string_view::npos || m.find('+') != std::string_view::npos;
  const bool writes = m.find('w') != std::string_view::npos ||
                      m.find('a') != std::string_view::npos ||
                      m.find('+') != std::string_view::npos;
  if (reads && writes) {
    return "rw";
  }
  return writes ? "w" : "r";
}

}  // namespace

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  // A JNI round trip through ContentResolver.openFileDescriptor(Uri.parse(uri),
  // mode) followed by ParcelFileDescriptor.detachFd(), which hands ownership
  // of the descriptor to this side. The Storage Access Framework picker in the
  // activity returns exactly these URIs for a disc image or title update the
  // player chose, and a detached descriptor stays valid for the life of the
  // process - long enough to be read through /proc/self/fd/<n> by code that
  // only understands paths.
  JNIEnv* env = GetAndroidThreadJniEnv();
  jobject context = GetAndroidApplicationContext();
  if (!env || !context) {
    REXFS_ERROR("OpenAndroidContentFileDescriptor({}): no JNI environment or application context",
                uri);
    return -1;
  }

  jclass uri_class = env->FindClass("android/net/Uri");
  if (ClearJavaException(env, "FindClass(android.net.Uri)") || !uri_class) {
    return -1;
  }
  jmethodID parse = env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  jstring java_uri = env->NewStringUTF(std::string(uri).c_str());
  jobject uri_object = env->CallStaticObjectMethod(uri_class, parse, java_uri);
  env->DeleteLocalRef(java_uri);
  env->DeleteLocalRef(uri_class);
  if (ClearJavaException(env, "Uri.parse") || !uri_object) {
    return -1;
  }

  jclass context_class = env->GetObjectClass(context);
  jmethodID get_resolver = env->GetMethodID(context_class, "getContentResolver",
                                            "()Landroid/content/ContentResolver;");
  jobject resolver = env->CallObjectMethod(context, get_resolver);
  env->DeleteLocalRef(context_class);
  if (ClearJavaException(env, "Context.getContentResolver") || !resolver) {
    env->DeleteLocalRef(uri_object);
    return -1;
  }

  jclass resolver_class = env->GetObjectClass(resolver);
  jmethodID open_fd = env->GetMethodID(
      resolver_class, "openFileDescriptor",
      "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
  jstring java_mode = env->NewStringUTF(ContentResolverMode(mode));
  jobject pfd = env->CallObjectMethod(resolver, open_fd, uri_object, java_mode);
  env->DeleteLocalRef(java_mode);
  env->DeleteLocalRef(resolver_class);
  env->DeleteLocalRef(resolver);
  env->DeleteLocalRef(uri_object);
  if (ClearJavaException(env, "ContentResolver.openFileDescriptor") || !pfd) {
    REXFS_ERROR("OpenAndroidContentFileDescriptor({}, {}): the provider returned nothing", uri,
                mode ? mode : "(null)");
    return -1;
  }

  jclass pfd_class = env->GetObjectClass(pfd);
  jmethodID detach_fd = env->GetMethodID(pfd_class, "detachFd", "()I");
  const jint fd = env->CallIntMethod(pfd, detach_fd);
  env->DeleteLocalRef(pfd_class);
  env->DeleteLocalRef(pfd);
  if (ClearJavaException(env, "ParcelFileDescriptor.detachFd")) {
    return -1;
  }
  REXFS_INFO("OpenAndroidContentFileDescriptor({}, {}) -> fd {}", uri, ContentResolverMode(mode),
             int(fd));
  return int(fd);
}

}  // namespace filesystem
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
