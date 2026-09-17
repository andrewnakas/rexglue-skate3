/**
 * @file        rex/core/execinfo_switch.cpp
 * @brief       <execinfo.h> stand-in for Horizon.
 *
 * Same three entry points as the Android file, over the same unwinder. The one
 * real difference is symbolisation: there is no dladdr here and the NRO carries
 * no symbol table, so a frame is printed as an offset from the image base.
 * Feed those offsets to
 *
 *     aarch64-none-elf-addr2line -f -C -e skate3.debug.elf +0x...
 *
 * which is what scripts/symbolize.sh does. The unstripped ELF is kept beside
 * the NRO by the build for exactly this reason.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/execinfo_android.h>

#include <unwind.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// Placed by libnx's linker script at the start of the loaded image. Every
// address in a backtrace is an offset from here.
extern "C" char __start__;

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

// "[0x<absolute>] +0x<offset>" - the absolute address is worth keeping because
// it is what a kernel fault report quotes, and the offset is what addr2line
// wants. Frames outside the image (the driver's own allocations, a corrupted
// return address) print with no offset rather than a nonsensical one.
int FormatFrame(char* out, size_t out_size, void* address) {
  const uintptr_t pc = reinterpret_cast<uintptr_t>(address);
  const uintptr_t base = reinterpret_cast<uintptr_t>(&__start__);
  if (pc >= base) {
    return snprintf(out, out_size, "[%p] +0x%zx", address, size_t(pc - base));
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
  // One allocation: the char* table followed by the strings, so the caller
  // frees it with a single free() exactly as with glibc.
  constexpr size_t kMaxFrameText = 128;
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
  // Never allocates: this runs from the exception handler, where the heap may
  // be the thing that is broken. The whole trace goes out in as few writes as
  // possible so that concurrent thread dumps do not interleave line by line.
  constexpr size_t kChunk = 4096;
  char chunk[kChunk];
  size_t used = 0;
  char line[128];

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
    memcpy(chunk + used, line, line_len);
    used += line_len;
  }
  if (used) {
    ssize_t ignored = write(fd, chunk, used);
    (void)ignored;
  }
}

#endif  // REX_PLATFORM_SWITCH
