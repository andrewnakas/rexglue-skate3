/**
 * @file        rex/core/execinfo_android.cpp
 * @brief       <execinfo.h> stand-in for Android
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <rex/execinfo_android.h>

#include <dlfcn.h>
#include <unwind.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

struct BacktraceState {
  void** current;
  void** end;
};

_Unwind_Reason_Code UnwindCallback(_Unwind_Context* context, void* arg) {
  auto* state = static_cast<BacktraceState*>(arg);
  const uintptr_t pc = _Unwind_GetIP(context);
  if (!pc) {
    return _URC_NO_REASON;
  }
  if (state->current == state->end) {
    return _URC_END_OF_STACK;
  }
  *state->current++ = reinterpret_cast<void*>(pc);
  return _URC_NO_REASON;
}

// "<binary>(<symbol>+0x<offset>) [0x<address>]", matching glibc closely enough
// for the existing log formatting.
int FormatFrame(char* out, size_t out_size, void* address) {
  Dl_info info;
  if (dladdr(address, &info) && info.dli_fname) {
    if (info.dli_sname) {
      const uintptr_t offset = reinterpret_cast<uintptr_t>(address) -
                               reinterpret_cast<uintptr_t>(info.dli_saddr);
      return snprintf(out, out_size, "%s(%s+0x%zx) [%p]", info.dli_fname, info.dli_sname,
                      size_t(offset), address);
    }
    return snprintf(out, out_size, "%s(+0x%zx) [%p]", info.dli_fname,
                    size_t(reinterpret_cast<uintptr_t>(address) -
                           reinterpret_cast<uintptr_t>(info.dli_fbase)),
                    address);
  }
  return snprintf(out, out_size, "[%p]", address);
}

}  // namespace

int backtrace(void** buffer, int size) {
  if (!buffer || size <= 0) {
    return 0;
  }
  BacktraceState state{buffer, buffer + size};
  _Unwind_Backtrace(&UnwindCallback, &state);
  return int(state.current - buffer);
}

char** backtrace_symbols(void* const* buffer, int size) {
  if (!buffer || size <= 0) {
    return nullptr;
  }
  // Single allocation: the char* table followed by the strings, so the caller
  // frees it with one free() exactly as with glibc.
  constexpr size_t kMaxFrameText = 512;
  const size_t table_bytes = size_t(size) * sizeof(char*);
  char* block = static_cast<char*>(malloc(table_bytes + size_t(size) * kMaxFrameText));
  if (!block) {
    return nullptr;
  }
  auto** table = reinterpret_cast<char**>(block);
  char* text = block + table_bytes;
  for (int i = 0; i < size; ++i) {
    table[i] = text;
    const int written = FormatFrame(text, kMaxFrameText, buffer[i]);
    text += (written > 0 && size_t(written) < kMaxFrameText) ? size_t(written) + 1 : kMaxFrameText;
  }
  return table;
}

void backtrace_symbols_fd(void* const* buffer, int size, int fd) {
  if (!buffer || size <= 0) {
    return;
  }
  // Async-signal-safe: formats into a stack buffer, never allocates.
  //
  // The whole trace goes out in as few write() calls as possible. One write per
  // frame lets concurrent threads interleave line-by-line, which is exactly
  // what happens when the hang watchdog dumps every thread at once and makes
  // the resulting report unreadable.
  constexpr size_t kChunk = 8192;
  char chunk[kChunk];
  size_t used = 0;
  char line[512];

  for (int i = 0; i < size; ++i) {
    int written = FormatFrame(line, sizeof(line) - 1, buffer[i]);
    if (written < 0) {
      continue;
    }
    if (size_t(written) >= sizeof(line) - 1) {
      written = int(sizeof(line) - 2);
    }
    line[written] = '\n';
    const size_t line_len = size_t(written) + 1;

    if (used + line_len > kChunk) {
      ssize_t ignored = write(fd, chunk, used);
      (void)ignored;
      used = 0;
    }
    if (line_len > kChunk) {
      ssize_t ignored = write(fd, line, line_len);
      (void)ignored;
      continue;
    }
    memcpy(chunk + used, line, line_len);
    used += line_len;
  }
  if (used) {
    ssize_t ignored = write(fd, chunk, used);
    (void)ignored;
  }
}

#endif  // REX_PLATFORM_ANDROID
