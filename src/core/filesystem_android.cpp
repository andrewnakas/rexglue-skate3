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

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  // Not implemented: this build addresses game data by filesystem path under
  // the app's external files directory, so no content:// URI ever reaches
  // here - IsAndroidContentUri rejects plain paths before this is called.
  //
  // Implementing it means a JNI round trip through
  // ContentResolver.openFileDescriptor(Uri.parse(uri), mode) followed by
  // ParcelFileDescriptor.detachFd(). GetAndroidThreadJniEnv() and
  // GetAndroidApplicationContext() in <rex/main_android.h> provide what that
  // needs, should a Storage Access Framework picker ever be added.
  REXFS_ERROR("OpenAndroidContentFileDescriptor({}, {}): content:// URIs are not supported",
                uri, mode ? mode : "(null)");
  return -1;
}

}  // namespace filesystem
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
