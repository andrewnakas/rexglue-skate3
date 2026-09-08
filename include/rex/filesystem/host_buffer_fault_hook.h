// A host read lands its bytes straight in guest memory. When a debugging watch
// has made the target page read-only, pread() fails with EFAULT instead of
// faulting, so the watch installs this hook: it unprotects the page, records
// which file was being read into static image data, and returns true to have
// the read retried once. Nothing is installed in normal play and the cost is
// one branch on an error path.
//
// Deliberately its own header: rex/filesystem.h is reached by every generated
// translation unit, and touching it rebuilds the whole recompiled game.
#pragma once

#include <cstddef>

namespace rex {
namespace filesystem {

using HostBufferFaultHook = bool (*)(void* buffer, size_t length, const char* path,
                                     size_t file_offset);
void SetHostBufferFaultHook(HostBufferFaultHook hook);

}  // namespace filesystem
}  // namespace rex
